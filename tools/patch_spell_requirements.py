"""
patch_spell_requirements.py

Client-side Spell.dbc patch that MIRRORS the server-side strips in mod-multiclass
(StripShieldRequirements / StripWeaponRequirements / StripShapeshiftRequirements),
so the 3.3.5a client stops blocking casts the server already allows.

Why: equip/stance requirements are enforced CLIENT-side via Spell.dbc — the client
greys out / refuses the cast before it ever reaches the server. Server strips alone
do nothing for the player UI. This patch removes the same requirements client-side so
client and server AGREE (no SPELL_FAILED rejections).

What it does to EVERY Spell.dbc record:
  - SHIELD: requirement (EquippedItemClass=ARMOR & shield subclass, or INVTYPE_SHIELD)
            -> EquippedItemClass=-1, masks=0  (castable with no shield; matches server)
  - WEAPON: melee weapon-class locks -> broaden EquippedItemSubClassMask to ALL melee
            types (any melee weapon works); RANGED requirements left untouched; a weapon
            is still required (EquippedItemClass stays 2). Matches server exactly.
  - STANCE: Stances/StancesNot zeroed + clear SPELL_ATTR0_NOT_SHAPESHIFTED (0x10000)
            so nothing is stance/form-gated (also unblocks abilities during Metamorphosis).

Field indices EMPIRICALLY VERIFIED on this client (see inspect_spell_dbc.py):
  [4]  Attributes (NOT_SHAPESHIFTED = 0x10000)
  [12] Stances        [13] StancesNot
  [68] EquippedItemClass  [69] EquippedItemSubClassMask  [70] EquippedItemInventoryTypeMask

Reads from patch-enUS-4.MPQ (so the 44 AA spells 701xxx are preserved) and writes a
full patched Spell.dbc into patch-enUS-5.MPQ (loads AFTER enUS-4, overrides it).

After running: delete Cache/WDB/enUS/*.wdb and FULLY restart the client.
REVERT: just delete patch-enUS-5.MPQ (it's a standalone override).
"""
import mpyq, struct, shutil, os
import patch_aa_spells as base   # reuse load_dbc / build_dbc / create_mpq / MPQ crypto

DATA = r'C:\Users\Donald Lee Irwin Esq\Desktop\3.3.5\Data\enUS'
SRC_MPQ = os.path.join(DATA, 'patch-enUS-3.MPQ')   # real Blizzard MPQ (mpyq-readable)
# Write to enUS-4 (the custom patch we KNOW the client loads — AA spells already prove
# it). Earlier enUS-5 attempt didn't take effect (client didn't load patch number 5).
OUT_MPQ = os.path.join(DATA, 'patch-enUS-4.MPQ')
BAK_MPQ = OUT_MPQ + '.reqbak'   # distinct from the existing .bak so we don't clobber it
SPELL_PATH = 'DBFilesClient\\Spell.dbc'

# Verified 3.3.5a field indices
F_ATTR, F_STANCES, F_STANCES_NOT = 4, 12, 13
F_EQUIP_CLASS, F_EQUIP_SUBMASK, F_EQUIP_INVMASK = 68, 69, 70

ATTR_NOT_SHAPESHIFTED = 0x10000
ITEM_CLASS_WEAPON, ITEM_CLASS_ARMOR = 2, 4
SHIELD_SUBCLASS_BIT = 1 << 6     # 64
INVTYPE_SHIELD_BIT  = 1 << 14    # 16384
NEG1 = 0xFFFFFFFF                 # EquippedItemClass = -1 (no requirement)

# Weapon subclass masks — MUST match server StripWeaponRequirements
MELEE_MASK  = (1<<0)|(1<<1)|(1<<4)|(1<<5)|(1<<6)|(1<<7)|(1<<8)|(1<<10)|(1<<11)|(1<<12)|(1<<13)|(1<<14)|(1<<15)|(1<<17)|(1<<20)
RANGED_MASK = (1<<2)|(1<<3)|(1<<16)|(1<<18)|(1<<19)

def s32(v): return v - 0x100000000 if v >= 0x80000000 else v

def main():
    print(f"Reading Spell.dbc from {SRC_MPQ}")
    arch = mpyq.MPQArchive(SRC_MPQ)
    recs, fc, strblk = base.load_dbc(arch, SPELL_PATH)
    assert fc == 234, f"unexpected field count {fc} (expected 234)"

    # Also load the two AA-skill DBCs so the regenerated enUS-4 keeps its AA spellbook tab.
    sl_recs, sl_fc, sl_strblk   = base.load_dbc(arch, base.SKILLLINE_PATH)
    sla_recs, sla_fc, sla_strblk = base.load_dbc(arch, base.SLA_PATH)

    # Re-add the 44 Sanctum AA activation spells (701xxx) + skill line + SLAs (same logic
    # as patch_aa_spells) so the regenerated enUS-4 is a full superset.
    by_id = {r[0]: r for r in recs}
    ref_ids = {ir for _, _, ir, _ in base.AA_SPELLS}
    icon_map = {rid: (by_id[rid][133] if rid in by_id else 0) for rid in ref_ids}
    tab_icon = icon_map.get(46924, 0)
    recs    = base.patch_spell_dbc(recs, fc, strblk, icon_map)
    sl_recs = base.patch_skillline_dbc(sl_recs, sl_fc, sl_strblk, tab_icon)
    sla_recs = base.patch_sla_dbc(sla_recs, sla_fc)

    shield = weapon = stance = naked = 0
    for r in recs:
        # STANCE: zero stance masks + clear NOT_SHAPESHIFTED
        if r[F_STANCES] or r[F_STANCES_NOT]:
            r[F_STANCES] = 0; r[F_STANCES_NOT] = 0; stance += 1
        if r[F_ATTR] & ATTR_NOT_SHAPESHIFTED:
            r[F_ATTR] &= (~ATTR_NOT_SHAPESHIFTED) & 0xFFFFFFFF; naked += 1

        ec = s32(r[F_EQUIP_CLASS])
        is_shield = ((ec == ITEM_CLASS_ARMOR and (r[F_EQUIP_SUBMASK] & SHIELD_SUBCLASS_BIT))
                     or (r[F_EQUIP_INVMASK] & INVTYPE_SHIELD_BIT))
        if is_shield:
            r[F_EQUIP_CLASS] = NEG1; r[F_EQUIP_SUBMASK] = 0; r[F_EQUIP_INVMASK] = 0
            shield += 1
        elif ec == ITEM_CLASS_WEAPON:
            sub = r[F_EQUIP_SUBMASK]
            if (sub & RANGED_MASK) and not (sub & MELEE_MASK):
                pass  # ranged-only requirement (Auto Shot etc.) — keep
            else:
                if r[F_EQUIP_SUBMASK] != MELEE_MASK or r[F_EQUIP_INVMASK] != 0:
                    r[F_EQUIP_SUBMASK] = MELEE_MASK; r[F_EQUIP_INVMASK] = 0
                    weapon += 1

    print(f"  stripped: shield={shield}  weapon-broadened={weapon}  stance={stance}  not-shapeshifted-cleared={naked}")

    spell_blob = base.build_dbc(recs, fc, strblk)
    sl_blob    = base.build_dbc(sl_recs, sl_fc, sl_strblk)
    sla_blob   = base.build_dbc(sla_recs, sla_fc, sla_strblk)
    print(f"  patched Spell.dbc: {len(spell_blob):,} bytes ({len(recs)} records)")

    mpq_bytes = base.create_mpq({
        SPELL_PATH:          spell_blob,
        base.SKILLLINE_PATH: sl_blob,
        base.SLA_PATH:       sla_blob,
    })
    if os.path.exists(OUT_MPQ) and not os.path.exists(BAK_MPQ):
        shutil.copy2(OUT_MPQ, BAK_MPQ); print(f"  previous enUS-4 backed up -> {os.path.basename(BAK_MPQ)}")
    with open(OUT_MPQ, 'wb') as f:
        f.write(mpq_bytes)
    print(f"\nDone. {OUT_MPQ} written ({len(mpq_bytes):,} bytes).")
    print("Next: delete Cache/WDB/enUS/*.wdb, fully restart the client.")
    print("Revert: restore patch-enUS-4.MPQ.reqbak over patch-enUS-4.MPQ.")

if __name__ == '__main__':
    main()
