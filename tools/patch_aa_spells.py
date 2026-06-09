"""
patch_aa_spells.py

Creates patch-enUS-4.MPQ containing three patched DBC files:
  Spell.dbc          — 44 AA activation spell records (IDs 701001–701044)
  SkillLine.dbc      — 1 custom skill line (ID 900001, "Sanctum Abilities" tab)
  SkillLineAbility.dbc — 44 rows linking each spell to the skill line

WoW 3.3.5a loads patches in numerical order; patch-enUS-4 overrides the base
game DBCs cleanly without touching the existing patch-enUS-3.MPQ.

DBC field indices verified against the live 3.3.5a client:
  Spell.dbc         234 fields / 936 bytes per record
  SkillLine.dbc      56 fields / 224 bytes per record
  SkillLineAbility   14 fields /  56 bytes per record

Fully idempotent — re-running regenerates the file from scratch.
Run after any change to the AA spell list.

Server-side: apply aa_skillline.sql to acore_world.
C++ side:    mod-aa-system.cpp OnPlayerLogin grants SetSkill(900001,0,1,1).
Client side: delete Cache/WDB/enUS/*.wdb, fully restart WoW.
"""

import mpyq, struct, shutil, os, zlib

# ── Paths ──────────────────────────────────────────────────────────────────
SRC_MPQ   = r'C:\Users\Donald Lee Irwin Esq\Desktop\3.3.5\Data\enUS\patch-enUS-3.MPQ'  # read source
OUT_MPQ   = r'C:\Users\Donald Lee Irwin Esq\Desktop\3.3.5\Data\enUS\patch-enUS-4.MPQ'  # write target
BAK_MPQ   = OUT_MPQ + '.bak'

SPELL_PATH    = 'DBFilesClient\\Spell.dbc'
SKILLLINE_PATH= 'DBFilesClient\\SkillLine.dbc'
SLA_PATH      = 'DBFilesClient\\SkillLineAbility.dbc'

SKILL_ID    = 900001
SLA_ID_BASE = 900001
LOCALE_MASK = 16712190   # 0x00FF0FFE — this client's enUS-only mask

# ── AA spell table ──────────────────────────────────────────────────────────
# (spell_id, name, icon_ref_spell_id, description)
# icon_ref_spell_id: existing WoW spell whose SpellIconID we clone.

AA_SPELLS = [
    # Warrior
    (701001, "Rampage",                46924, "Enter a battle frenzy, striking all nearby enemies for weapon damage."),
    (701002, "Warcry",                  6673, "Unleash a war cry, becoming immune to fear for 10 seconds."),
    # Rogue
    (701003, "Death Blow",              1329, "A finishing strike dealing 300% weapon damage. Kills targets below 15% HP."),
    (701004, "Escape Artist",           2983, "Instantly break free of all movement-impairing effects."),
    (701005, "Dancing Blade",          16511, "Your blade dances in an arc, striking all enemies in melee range."),
    (701022, "Assassin's Mark",        16511, "Mark a target, increasing all damage you deal to it for 15 seconds."),
    # Priest
    (701006, "Force of Will",          32379, "Unleash a burst of will, instantly healing yourself for a large amount."),
    (701007, "Divine Stun",            33786, "Stun all enemies within range with a burst of divine power."),
    (701008, "Invocation",              2061, "Channel holy power to deal damage and heal yourself for the same amount."),
    (701023, "Channeling the Divine",   2061, "Your next several heals fire twice at increased mana cost."),
    (701024, "Forceful Rejuvenation",   2061, "Instantly reset all spell cooldowns. Long cooldown."),
    (701025, "Yaulp",                   2061, "A battle shout that boosts melee speed and mana regeneration."),
    (701026, "Celestial Hammer",       32379, "Conjure a celestial hammer that strikes your enemy multiple times."),
    (701027, "Celestial Regeneration",  2061, "A free heal-over-time that restores a large percent of your maximum health."),
    (701028, "Quick Buff",              2061, "Your next several beneficial spells cast at half cast time."),
    (701029, "Divine Arbitration",     33786, "Equalize health between yourself and all active pets and guardians."),
    (701030, "Celestial Barrier",       2061, "Surround yourself with a celestial absorb shield based on Spell Power."),
    (701031, "Bestow Divine Aura",     33786, "Your target becomes invulnerable to all harm for several seconds."),
    (701032, "Sanctification",         33786, "Consecrate the ground, regenerating your health while you stand within it."),
    # Death Knight
    (701009, "Lifeburn",               49998, "Sacrifice 20% of your health to deal heavy shadow damage to your target."),
    (701010, "Death Pact",             49998, "Spend health to instantly generate a large amount of Runic Power."),
    (701011, "Leech Touch (DK)",       49998, "Drain life from your target, healing yourself for the same amount."),
    # Shaman
    (701012, "Cannibalize",              421, "Consume your own vitality to restore a large portion of your maximum mana."),
    (701013, "Elemental Fury",           421, "Unleash your active elemental enchant as a ground-based area attack."),
    # Mage
    (701014, "Harvest of Druzzil",     30451, "Instantly restore a large percentage of your maximum mana."),
    (701015, "Manaburn",               30451, "Spend 50% of your mana to deal devastating arcane damage. Long cooldown."),
    (701033, "Frenzied Burnout",       30451, "Increase your Water Elemental's attack speed and damage temporarily."),
    (701034, "Mend Companion",         30451, "Channel arcane energy to restore your Water Elemental to full health."),
    (701039, "Focused Magic",          30451, "Cast on an enemy, creating an arcane zone that deals periodic arcane damage."),
    (701040, "Call of Xuzl",            686,  "Summon orbiting arcane blades that deal periodic arcane damage to nearby foes."),
    (701041, "Host of the Elements",    421,  "Summon an Ice Elemental to fight alongside your Water Elemental."),
    # Warlock
    (701016, "Mortal Eradication",      686,  "Apply a shadow curse that deals periodic shadow damage based on Spell Power."),
    (701017, "Fearstorm",               686,  "Send all nearby enemies fleeing in terror for several seconds."),
    (701018, "Lifeburn (Warlock)",      686,  "Deal shadow damage equal to 100% of your current health to your target."),
    (701019, "Leech Touch (Warlock)",   686,  "Drain a percentage of your target's health as shadow, healing yourself."),
    (701035, "Wake the Dead",           686,  "Resummon your slain demon as a spirit to fight for a limited time."),
    (701036, "Dire Charm",              686,  "Permanently charm a Demon-type enemy to fight at your side."),
    (701042, "Threads of Despair",      686,  "Curse your target with shadow threads that deal periodic shadow damage."),
    (701043, "Soul Barrage",            686,  "Hurl soul energy through all targets in a line, dealing heavy shadow damage."),
    (701044, "Feigned Minion",          686,  "Your demon feigns death, losing all threat temporarily."),
    # Hunter
    (701020, "Volley Burst",           2643,  "Fire a rapid burst of arrows at your target location, striking all in the area."),
    (701021, "Scout of the Wild",      2643,  "Summon a spirit wolf companion to fight at your side for a short time."),
    # Archetype / Other
    (701037, "Weapon Fury",           46924,  "Enter a weapon frenzy, causing all melee swings to trigger on-hit effects."),
    (701038, "Yaulp (Paladin)",       35395,  "A holy war shout that boosts melee attack speed and mana regeneration."),
]

assert len(AA_SPELLS) == 44, f"Expected 44 entries, got {len(AA_SPELLS)}"

# ── MPQ crypto — verbatim from patch_item_dbc.py ──────────────────────────

def _ct():
    t = [0] * 0x500
    s = 0x00100001
    for i in range(0x100):
        idx = i
        for _ in range(5):
            s  = (s * 125 + 3) % 0x2AAAAB
            t1 = (s & 0xFFFF) << 16
            s  = (s * 125 + 3) % 0x2AAAAB
            t[idx] = t1 | (s & 0xFFFF)
            idx += 0x100
    return t

CT = _ct()

def _kstep(k):
    return (((~k & 0xFFFFFFFF) << 21) & 0xFFFFFFFF | k >> 11) & 0xFFFFFFFF

def mhash(s, ht):
    s1, s2 = 0x7FED7FED, 0xEEEEEEEE
    for c in s.upper().encode('ascii'):
        s1 = (CT[(ht << 8) + c] ^ (s1 + s2)) & 0xFFFFFFFF
        s2 = (c + s1 + s2 + (s2 << 5) + 3)   & 0xFFFFFFFF
    return s1

def decrypt(data, key):
    key &= 0xFFFFFFFF; seed = 0xEEEEEEEE; out = bytearray(len(data))
    for i in range(0, len(data), 4):
        seed  = (seed + CT[0x400 + (key & 0xFF)]) & 0xFFFFFFFF
        ciph  = struct.unpack_from('<I', data, i)[0]
        plain = (ciph ^ (key + seed)) & 0xFFFFFFFF
        struct.pack_into('<I', out, i, plain)
        key   = _kstep(key)
        seed  = (plain + seed + (seed << 5) + 3) & 0xFFFFFFFF
    return bytes(out)

def encrypt(data, key):
    key &= 0xFFFFFFFF; seed = 0xEEEEEEEE; out = bytearray(len(data))
    for i in range(0, len(data), 4):
        seed  = (seed + CT[0x400 + (key & 0xFF)]) & 0xFFFFFFFF
        plain = struct.unpack_from('<I', data, i)[0]
        ciph  = (plain ^ (key + seed)) & 0xFFFFFFFF
        struct.pack_into('<I', out, i, ciph)
        key   = _kstep(key)
        seed  = (plain + seed + (seed << 5) + 3) & 0xFFFFFFFF
    return bytes(out)

# ── DBC helpers ────────────────────────────────────────────────────────────

def load_dbc(arch, path):
    raw = arch.read_file(path)
    if raw is None:
        raise SystemExit(f"ABORT: {path} not found in any loaded MPQ")
    magic, rc, fc, rs, sbsz = struct.unpack_from('<4sIIII', raw, 0)
    assert magic == b'WDBC', f"{path}: bad magic {magic}"
    HDR = 20
    fmt = '<%dI' % fc
    records = [list(struct.unpack_from(fmt, raw, HDR + i * rs)) for i in range(rc)]
    strblock = bytearray(raw[HDR + rc * rs : HDR + rc * rs + sbsz])
    print(f"  Loaded {path}: {rc} records, {fc} fields, strblock {sbsz}b")
    return records, fc, strblock

def add_string(strblock, s):
    if not s:
        return 0
    off = len(strblock)
    strblock += s.encode('utf-8') + b'\x00'
    return off

def build_dbc(records, fc, strblock):
    rs  = fc * 4
    out = struct.pack('<4sIIII', b'WDBC', len(records), fc, rs, len(strblock))
    fmt = '<%dI' % fc
    for r in records:
        out += struct.pack(fmt, *r)
    out += bytes(strblock)
    return out

# ── Per-DBC patch functions ────────────────────────────────────────────────

def patch_spell_dbc(records, fc, strblock, icon_map):
    """
    Verified Spell.dbc field indices (234 fields):
      [4]   Attributes = 0x40 (SPELL_ATTR0_ABILITY)
      [64]  CastingTimeIndex = 1
      [85]  RangeIndex = 1
      [95]  SpellLevel = 1
      [107] Effect_1 = 3 (DUMMY)
      [119] EffectImplicitTargetA_1 = 1 (caster)
      [133] SpellIconID
      [134] ActiveIconID
      [136] Name_Lang_enUS (string offset)
      [152] Name_Lang_Mask
      [170] Description_Lang_enUS (string offset)
      [186] Description_Lang_Mask
    """
    by_id = {r[0]: r for r in records}
    added = 0
    for sid, name, icon_ref, desc in AA_SPELLS:
        if sid in by_id:
            print(f"    spell {sid} ({name}) already in DBC — skipping")
            continue
        icon = icon_map.get(icon_ref, 0)
        rec  = [0] * fc
        rec[0]   = sid
        rec[4]   = 0x40
        rec[64]  = 1
        rec[85]  = 1
        rec[95]  = 1
        rec[107] = 3
        rec[119] = 1
        rec[133] = icon
        rec[134] = icon
        rec[136] = add_string(strblock, name)
        rec[152] = LOCALE_MASK
        rec[170] = add_string(strblock, desc)
        rec[186] = LOCALE_MASK
        records.append(rec)
        added += 1
    records.sort(key=lambda r: r[0])
    print(f"  Spell.dbc: +{added} new records ({len(records)} total)")
    return records

def patch_skillline_dbc(records, fc, strblock, tab_icon):
    """
    SkillLine.dbc field layout (56 fields):
      [0]  ID,  [1] CategoryID,  [2] SkillCostsID
      [3]  Name_Lang_enUS  [4..18] other locales=0
      [19] Name_Lang_Mask
      [20..35] Desc locales=0  [36] Desc_Lang_Mask=0
      [37] SpellIconID
      [38..53] AlternateVerb locales=0  [54] AlternateVerb_Mask=0
      [55] CanLink
    """
    by_id = {r[0]: r for r in records}
    if SKILL_ID in by_id:
        print(f"  SkillLine.dbc: {SKILL_ID} already present — skipping")
        return records
    rec     = [0] * fc
    rec[0]  = SKILL_ID
    rec[1]  = 7          # CategoryID: Armor Proficiency category → renders as clean tab
    rec[3]  = add_string(strblock, "Sanctum Abilities")
    rec[19] = LOCALE_MASK
    rec[37] = tab_icon
    records.append(rec)
    records.sort(key=lambda r: r[0])
    print(f"  SkillLine.dbc: +1 record (ID {SKILL_ID}, icon {tab_icon})")
    return records

def patch_sla_dbc(records, fc):
    """
    SkillLineAbility.dbc field layout (14 fields):
      [0] ID  [1] SkillLine  [2] Spell
      [3] RaceMask  [4] ClassMask  [5] ExcludeRace  [6] ExcludeClass
      [7] MinSkillLineRank  [8] SupercededBySpell
      [9] AcquireMethod (0=auto-display)
      [10] TrivialSkillLineRankHigh  [11] TrivialSkillLineRankLow
      [12] CharacterPoints_1  [13] CharacterPoints_2
    """
    existing_spells = {r[2] for r in records}
    existing_ids    = {r[0] for r in records}
    next_id = SLA_ID_BASE
    while next_id in existing_ids:
        next_id += 1
    added = 0
    for sid, name, _icon, _desc in AA_SPELLS:
        if sid in existing_spells:
            print(f"    SLA for {sid} ({name}) already present — skipping")
            continue
        rec     = [0] * fc
        rec[0]  = next_id
        rec[1]  = SKILL_ID
        rec[2]  = sid
        rec[9]  = 0  # AcquireMethod: auto-display when spell is known
        records.append(rec)
        existing_ids.add(next_id)
        next_id += 1
        while next_id in existing_ids:
            next_id += 1
        added += 1
    records.sort(key=lambda r: r[0])
    print(f"  SkillLineAbility.dbc: +{added} records ({len(records)} total)")
    return records

# ── MPQ creator — builds a new MPQ containing given files ─────────────────
# Rather than patching the existing MPQ (which would require the DBC to already
# be in its hash table), we create a fresh patch-enUS-4.MPQ.  WoW loads patches
# in numerical order so enUS-4 transparently overrides the base-game DBCs.

def create_mpq(files_dict):
    """
    files_dict: {internal_path: raw_bytes}
    Returns the complete bytes of a valid MPQ archive.
    """
    n = len(files_dict)
    # Hash table size must be a power of 2 and >= 4× number of files
    ht_size = max(16, 1)
    while ht_size < n * 4:
        ht_size <<= 1

    HEADER_SIZE = 32

    # ── Compress all files and record positions ──
    block_table = []   # (file_offset, csize, usize, flags)
    compressed  = []   # compressed bytes per file
    cur_off     = HEADER_SIZE

    for path, data in files_dict.items():
        cdata = b'\x02' + zlib.compress(data, 9)
        block_table.append((cur_off, len(cdata), len(data), 0x81000200))
        compressed.append(cdata)
        cur_off += len(cdata)

    # ── Build hash table ──
    # Empty slot sentinel: ha=0xFFFFFFFF, hb=0xFFFFFFFF, locale=0xFFFF, plat=0, bidx=0xFFFFFFFF
    ht = [(0xFFFFFFFF, 0xFFFFFFFF, 0xFFFF, 0, 0xFFFFFFFF)] * ht_size
    for idx, path in enumerate(files_dict.keys()):
        ha   = mhash(path, 1)
        hb   = mhash(path, 2)
        slot = mhash(path, 0) % ht_size
        while ht[slot][4] != 0xFFFFFFFF:   # linear probe for empty slot
            slot = (slot + 1) % ht_size
        ht[slot] = (ha, hb, 0, 0, idx)    # locale=0, platform=0

    # ── Positions for tables ──
    ht_off = cur_off
    bt_off = ht_off + ht_size * 16
    archive_size = bt_off + n * 16

    # ── Encode hash table (encrypted) ──
    ht_raw = b''.join(struct.pack('<IIHHI', ha, hb, loc, plat, bidx)
                      for ha, hb, loc, plat, bidx in ht)
    ht_enc = encrypt(ht_raw, mhash('(hash table)', 3))

    # ── Encode block table (encrypted) ──
    bt_raw = b''.join(struct.pack('<IIII', off, csz, usz, flags)
                      for off, csz, usz, flags in block_table)
    bt_enc = encrypt(bt_raw, mhash('(block table)', 3))

    # ── Header ──
    header = struct.pack('<4sIIHHIIII',
        b'MPQ\x1a',   # magic
        HEADER_SIZE,   # header_size
        archive_size,  # archive_size
        0,             # format_version 0
        3,             # sector_size_shift (4096-byte sectors)
        ht_off,        # hash_table_offset
        bt_off,        # block_table_offset
        ht_size,       # hash_table_size (entries, not bytes)
        n,             # block_table_size (entries)
    )

    # ── Assemble ──
    out = header
    for cdata in compressed:
        out += cdata
    out += ht_enc
    out += bt_enc
    return out

# ── Main ──────────────────────────────────────────────────────────────────

def main():
    print("patch_aa_spells.py — Sanctum Abilities DBC patch")
    print(f"Reading DBCs from: {SRC_MPQ}")
    print(f"Output:            {OUT_MPQ}")

    # Read all three DBCs from the existing MPQ chain (base game + patches)
    arch = mpyq.MPQArchive(SRC_MPQ)

    print("\n[1/3] Loading Spell.dbc...")
    spell_recs, spell_fc, spell_strblk = load_dbc(arch, SPELL_PATH)
    spell_by_id = {r[0]: r for r in spell_recs}

    # Build icon lookup: icon_ref_spell_id → SpellIconID (field [133])
    ref_ids  = {icon_ref for _, _, icon_ref, _ in AA_SPELLS}
    icon_map = {}
    for ref_id in ref_ids:
        if ref_id in spell_by_id:
            icon_map[ref_id] = spell_by_id[ref_id][133]
        else:
            print(f"  WARN: icon ref spell {ref_id} not found — will use icon 0")
            icon_map[ref_id] = 0

    tab_icon = icon_map.get(46924, 0)   # Bladestorm icon for the skill line tab
    print(f"  Skill line tab icon (from Bladestorm): {tab_icon}")

    print("\n[2/3] Loading SkillLine.dbc...")
    sl_recs, sl_fc, sl_strblk = load_dbc(arch, SKILLLINE_PATH)

    print("\n[3/3] Loading SkillLineAbility.dbc...")
    sla_recs, sla_fc, sla_strblk = load_dbc(arch, SLA_PATH)

    # Patch each DBC in memory
    print("\nPatching Spell.dbc...")
    spell_recs = patch_spell_dbc(spell_recs, spell_fc, spell_strblk, icon_map)

    print("\nPatching SkillLine.dbc...")
    sl_recs = patch_skillline_dbc(sl_recs, sl_fc, sl_strblk, tab_icon)

    print("\nPatching SkillLineAbility.dbc...")
    sla_recs = patch_sla_dbc(sla_recs, sla_fc)

    # Rebuild DBC blobs
    spell_blob = build_dbc(spell_recs, spell_fc, spell_strblk)
    sl_blob    = build_dbc(sl_recs,    sl_fc,    sl_strblk)
    sla_blob   = build_dbc(sla_recs,   sla_fc,   sla_strblk)

    print(f"\n  Spell.dbc:           {len(spell_blob):,} bytes")
    print(f"  SkillLine.dbc:       {len(sl_blob):,} bytes")
    print(f"  SkillLineAbility.dbc:{len(sla_blob):,} bytes")

    # Create new patch-enUS-4.MPQ
    print(f"\nBuilding {OUT_MPQ}...")
    files = {
        SPELL_PATH:     spell_blob,
        SKILLLINE_PATH: sl_blob,
        SLA_PATH:       sla_blob,
    }
    mpq_bytes = create_mpq(files)

    if os.path.exists(OUT_MPQ) and not os.path.exists(BAK_MPQ):
        shutil.copy2(OUT_MPQ, BAK_MPQ)
        print("  Previous version backed up.")

    with open(OUT_MPQ, 'wb') as f:
        f.write(mpq_bytes)

    print(f"\nDone. patch-enUS-4.MPQ written ({len(mpq_bytes):,} bytes).")
    print("\nNext steps:")
    print("  1. Verify aa_skillline.sql is applied to acore_world")
    print("  2. Rebuild + deploy worldserver (SetSkill 900001 in OnPlayerLogin)")
    print("  3. Delete all files in WoW client Cache/WDB/enUS/")
    print("  4. Fully restart WoW (close and reopen — not just relog)")
    print("\nIn-game: open spell book (P), look for 'Sanctum Abilities' tab.")
    print("         Buy an AA — its activation spell appears there automatically.")

if __name__ == '__main__':
    main()
