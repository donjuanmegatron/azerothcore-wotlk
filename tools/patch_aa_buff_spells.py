"""
patch_aa_buff_spells.py  —  Custom AA buff-aura spells (client + server, one source).

WHY: AA actives apply effects via raw server code (ApplyAttackTimePercentMod, stack maps,
SetPower...) which creates NO Aura object -> NO buff icon in the client buff bar. Fix
("option C done right"): a real custom spell the SERVER applies as a native aura AND the
CLIENT knows for display. Most are DUMMY "marker" auras (no mechanical effect — the existing
handler code still does the work) that just carry icon + name + timer + stack count.
Yaulp (720000) is the one FUNCTIONAL aura (real +20% melee haste) — proven working.

ONE CATALOG below drives BOTH halves:
  * CLIENT: `python patch_aa_buff_spells.py`         -> appends records to Data\patch-4.MPQ
  * SERVER: `python patch_aa_buff_spells.py --sql X` -> writes spell_dbc INSERT SQL to X

Field indices EMPIRICALLY VERIFIED on live 3.3.5a Spell.dbc (234 fields/936b):
  [0]ID [4]Attr(0=visible) [28]CastTime [40]DurationIndex [46]Range [49]CumulativeAura(stack)
  [71]Effect1 [74]DieSides1 [80]BasePoints1 [86]ImplicitTargetA1 [95]ApplyAuraName1 [110]MiscValue1
  [133]Icon [134]ActiveIcon [136]Name [152]NameMask [170]Desc [186]DescMask
DurationIndex 9=30000ms (default for timed; the handler overrides via aura->SetDuration),
21=infinite (charges / persistent — no countdown). Aura 4=DUMMY (marker), 138=MOD_MELEE_HASTE.

Client timer + stack number come from the server's SMSG_AURA_UPDATE at apply time, so the
handler's SetDuration()/SetStackAmount() are the source of truth; the DBC values are defaults.

Backup: patch-4.MPQ.spellfix.bak. After client run: delete Cache\WDB\enUS\*.wdb, restart via WowExt.exe.
Idempotent (rebuilds Spell.dbc from the enUS-3 strip-patched source each run).
"""
import mpyq, struct, shutil, os, zlib, sys

CLIENT      = r'C:\Users\Donald Lee Irwin Esq\Desktop\3.3.5'
PATCHED_SRC = os.path.join(CLIENT, r'Data\enUS\patch-enUS-3.MPQ')
TARGET      = os.path.join(CLIENT, r'Data\patch-4.MPQ')
BAK         = TARGET + '.spellfix.bak'
SPELL_PATH  = 'DBFilesClient\\Spell.dbc'
LOCALE_MASK = 16712190

# field indices
F_ID,F_ATTR,F_CAST,F_DUR,F_RANGE,F_STACK = 0,4,28,40,46,49
F_EFF1,F_DIE1,F_BP1,F_TGTA1,F_AURA1,F_MISC1 = 71,74,80,86,95,110
F_ICON,F_ACTIVEICON,F_NAME,F_NAMEMASK,F_DESC,F_DESCMASK = 133,134,136,152,170,186

DUR_TIMED, DUR_INF = 9, 21      # 30s default (handler overrides) / infinite
AURA_DUMMY, AURA_MELEE_HASTE = 4, 138

# ── CATALOG ─ id, name, icon, dur_idx, aura, basepoints, diesides, stackcap, description ──
# stackcap>0 => stacking icon (handler SetStackAmount). aura 4 = pure display marker.
AA_BUFFS = [
 (720000,"Yaulp",                  456, DUR_TIMED, AURA_MELEE_HASTE,19,1, 0,"Increases melee attack speed by 20% for 30 sec."),
 (720001,"Rampage",               2782, DUR_TIMED, AURA_DUMMY,0,0, 0,"Your melee swings cleave all nearby enemies."),
 (720002,"Iron Warrior",           281, DUR_TIMED, AURA_DUMMY,0,0, 0,"Absorbing a portion of incoming damage."),
 (720003,"Furious Charge",         457, DUR_TIMED, AURA_DUMMY,0,0, 0,"Empowered after Charge — increased damage."),
 (720004,"Vengeful Bulwark",        28, DUR_TIMED, AURA_DUMMY,0,0,10,"Retaliating against attackers; damage rises per stack."),
 (720005,"Judge",                  205, DUR_TIMED, AURA_DUMMY,0,0, 0,"Each swing deals bonus Holy damage."),
 (720006,"Radiance",               242, DUR_INF,   AURA_DUMMY,0,0, 5,"Your next Flash of Light heals are empowered."),
 (720007,"Unyielding Light",        81, DUR_TIMED, AURA_DUMMY,0,0, 0,"Empowered by holy light — increased damage and healing."),
 (720008,"Celestial Regeneration", 321, DUR_TIMED, AURA_DUMMY,0,0, 0,"Restoring health over time."),
 (720009,"Channeling the Divine", 2219, DUR_INF,   AURA_DUMMY,0,0, 0,"Your next heals cast twice."),
 (720010,"Celestial Barrier",      566, DUR_TIMED, AURA_DUMMY,0,0, 0,"Shielded by celestial energy."),
 (720011,"Bestow Divine Aura",      73, DUR_TIMED, AURA_DUMMY,0,0, 0,"Protected by a divine aura."),
 (720012,"Inspire",               1872, DUR_TIMED, AURA_DUMMY,0,0, 0,"Inspired — increased damage."),
 (720013,"Corrupted Carapace",    2720, DUR_TIMED, AURA_DUMMY,0,0,10,"Diseased carapace reflects damage; intensifies per stack."),
 (720014,"Final Rune",            2739, DUR_TIMED, AURA_DUMMY,0,0, 0,"Sustained by the Final Rune."),
 (720015,"Battle Endurance",       177, DUR_TIMED, AURA_DUMMY,0,0, 0,"Enduring — reduced damage taken."),
 (720016,"Frenzied Burnout",        33, DUR_TIMED, AURA_DUMMY,0,0, 0,"Your elemental burns with frenzy — increased haste and power."),
 (720017,"Molten Shell",          2307, DUR_TIMED, AURA_DUMMY,0,0,10,"Molten heat reflects damage and burns hotter per stack."),
 (720018,"Spell Weaving",         2294, DUR_TIMED, AURA_DUMMY,0,0, 5,"Weaving spell schools — increased damage per stack."),
 (720019,"Survival Instincts",    3707, DUR_TIMED, AURA_DUMMY,0,0, 0,"Survival instincts — reduced damage taken."),
 (720020,"Spirit of the Wood",     100, DUR_TIMED, AURA_DUMMY,0,0, 0,"Blessed by the spirit of the wood — healing and protection."),
 (720021,"Stampeding Roar",        959, DUR_TIMED, AURA_DUMMY,0,0, 0,"Stampeding — increased movement speed."),
 (720022,"Heart of the Wild",      112, DUR_TIMED, AURA_DUMMY,0,0, 0,"Heart of the Wild — increased damage and healing."),
 (720023,"Feral Charge Mastery",  3930, DUR_TIMED, AURA_DUMMY,0,0, 0,"Empowered after Feral Charge."),
 (720024,"Nature's Chosen",         10, DUR_INF,   AURA_DUMMY,0,0, 0,"Your next spell is instant."),
 (720025,"Wrath of the Wild",      689, DUR_INF,   AURA_DUMMY,0,0, 0,"Warded by the wrath of the wild."),
 (720026,"Ironfur",               1558, DUR_TIMED, AURA_DUMMY,0,0,10,"Iron fur increases armor and Thorns per stack."),
 (720027,"Cheer: Offensive",      1680, DUR_TIMED, AURA_DUMMY,0,0, 0,"Your pet attacks with increased ferocity."),
 (720028,"Cheer: Defensive",        83, DUR_TIMED, AURA_DUMMY,0,0, 0,"Your pet takes reduced damage."),
 (720029,"Cheer: Swiftness",      1181, DUR_TIMED, AURA_DUMMY,0,0, 0,"Your pet moves with increased speed."),
 (720030,"Weapon Fury",            138, DUR_TIMED, AURA_DUMMY,0,0, 0,"Weapon fury — increased melee damage."),
 (720031,"Vengeance",              169, DUR_TIMED, AURA_DUMMY,0,0, 0,"Vengeance — increased damage after being struck."),
 (720032,"Apex Predator",         2852, DUR_TIMED, AURA_DUMMY,0,0, 0,"Apex Predator — all stats increased."),
 (720033,"Hardening",              281, DUR_TIMED, AURA_DUMMY,0,0,10,"Hardening — reduced damage taken per stack."),
 (720034,"Hindsight",               99, DUR_TIMED, AURA_DUMMY,0,0, 0,"Shielded by hindsight."),
 (720035,"Last Stand",             177, DUR_TIMED, AURA_DUMMY,0,0, 0,"Last Stand — reduced damage taken."),
]

def emit_sql(path):
    cols = ("ID,Attributes,CastingTimeIndex,DurationIndex,RangeIndex,SpellLevel,"
            "Effect_1,EffectDieSides_1,EffectBasePoints_1,EffectAura_1,ImplicitTargetA_1,"
            "EffectMiscValue_1,SpellIconID,ActiveIconID,SchoolMask,CumulativeAura,Name_Lang_enUS")
    lines = ["-- Auto-generated by patch_aa_buff_spells.py --sql. Custom AA buff-aura spells (server side).",
             "-- Re-runnable: DELETE then INSERT. Requires worldserver RESTART (spell_dbc read at startup).",
             f"DELETE FROM spell_dbc WHERE ID BETWEEN 720000 AND 720099;"]
    for sid,name,icon,dur,aura,bp,die,stack,desc in AA_BUFFS:
        nm = name.replace("'", "''")
        lines.append(
            f"INSERT INTO spell_dbc ({cols}) VALUES "
            f"({sid},0,1,{dur},1,1,6,{die},{bp},{aura},1,0,{icon},{icon},0,{stack},'{nm}');")
    with open(path,'w',encoding='utf-8') as f: f.write("\n".join(lines)+"\n")
    print(f"Wrote {len(AA_BUFFS)} spell_dbc INSERTs -> {path}")

# ── MPQ crypto (correct _kstep) ──
def _ct():
    t=[0]*0x500; s=0x00100001
    for i in range(0x100):
        idx=i
        for _ in range(5):
            s=(s*125+3)%0x2AAAAB; t1=(s&0xFFFF)<<16
            s=(s*125+3)%0x2AAAAB; t[idx]=t1|(s&0xFFFF); idx+=0x100
    return t
CT=_ct()
def _kstep(k): return ((((~k & 0xFFFFFFFF)<<21)&0xFFFFFFFF)+0x11111111 | (k>>11))&0xFFFFFFFF
def mhash(s,ht):
    s1,s2=0x7FED7FED,0xEEEEEEEE
    for c in s.upper().encode('ascii'):
        s1=(CT[(ht<<8)+c]^(s1+s2))&0xFFFFFFFF; s2=(c+s1+s2+(s2<<5)+3)&0xFFFFFFFF
    return s1
def decrypt(data,key):
    key&=0xFFFFFFFF; seed=0xEEEEEEEE; out=bytearray(len(data))
    for i in range(0,len(data)&~3,4):
        seed=(seed+CT[0x400+(key&0xFF)])&0xFFFFFFFF
        c=struct.unpack_from('<I',data,i)[0]; p=(c^(key+seed))&0xFFFFFFFF
        struct.pack_into('<I',out,i,p); key=_kstep(key); seed=(p+seed+(seed<<5)+3)&0xFFFFFFFF
    return bytes(out)
def encrypt(data,key):
    key&=0xFFFFFFFF; seed=0xEEEEEEEE; out=bytearray(len(data))
    for i in range(0,len(data)&~3,4):
        seed=(seed+CT[0x400+(key&0xFF)])&0xFFFFFFFF
        p=struct.unpack_from('<I',data,i)[0]; c=(p^(key+seed))&0xFFFFFFFF
        struct.pack_into('<I',out,i,c); key=_kstep(key); seed=(p+seed+(seed<<5)+3)&0xFFFFFFFF
    return bytes(out)
def find_block_idx(raw,name):
    sig,hdrsz,arcsz,ver,ss,htoff,btoff,htsz,btsz=struct.unpack_from('<4sIIHHIIII',raw,0)
    assert sig==b'MPQ\x1a'
    ht=decrypt(raw[htoff:htoff+htsz*16], mhash('(hash table)',3))
    ha,hb=mhash(name,1),mhash(name,2); start=mhash(name,0)%htsz
    for probe in range(htsz):
        s=(start+probe)%htsz
        eha,ehb,loc,plat,bidx=struct.unpack_from('<IIHHI',ht,s*16)
        if bidx==0xFFFFFFFF: continue
        if eha==ha and ehb==hb: return bidx,btoff,btsz
    return None,btoff,btsz

def build_patched_spell_dbc(good):
    magic,rc,fc,rs,sb=struct.unpack_from('<4sIIII',good,0)
    assert magic==b'WDBC' and fc==234 and rs==936
    HDR=20
    records=[list(struct.unpack_from('<234I',good,HDR+i*rs)) for i in range(rc)]
    strblock=bytearray(good[HDR+rc*rs:HDR+rc*rs+sb])
    existing={r[F_ID] for r in records}
    def add_string(s):
        if not s: return 0
        off=len(strblock); strblock.extend(s.encode('utf-8')+b'\x00'); return off
    added=0
    for sid,name,icon,dur,aura,bp,die,stack,desc in AA_BUFFS:
        if sid in existing: continue
        rec=[0]*234
        rec[F_ID]=sid; rec[F_ATTR]=0; rec[F_CAST]=1; rec[F_DUR]=dur; rec[F_RANGE]=1; rec[F_STACK]=stack
        rec[F_EFF1]=6; rec[F_DIE1]=die; rec[F_BP1]=bp; rec[F_TGTA1]=1; rec[F_AURA1]=aura; rec[F_MISC1]=0
        rec[F_ICON]=icon; rec[F_ACTIVEICON]=icon
        rec[F_NAME]=add_string(name); rec[F_NAMEMASK]=LOCALE_MASK
        rec[F_DESC]=add_string(desc); rec[F_DESCMASK]=LOCALE_MASK
        records.append(rec); added+=1
    records.sort(key=lambda r:r[F_ID])
    out=struct.pack('<4sIIII',b'WDBC',len(records),234,936,len(strblock))
    for r in records: out+=struct.pack('<234I',*r)
    out+=bytes(strblock)
    print(f"  Spell.dbc rebuilt: {len(records)} records (+{added}), {len(out):,} bytes")
    return out

def patch_client():
    print("Reading patched Spell.dbc from patch-enUS-3.MPQ ...")
    good=mpyq.MPQArchive(PATCHED_SRC).read_file(SPELL_PATH)
    assert good and good[:4]==b'WDBC'
    m,rc,fc,rs,sb=struct.unpack_from('<4sIIII',good,0)
    for i in range(rc):
        o=20+i*rs
        if struct.unpack_from('<I',good,o)[0]==48952:
            assert struct.unpack_from('<i',good,o+68*4)[0]==-1,"source not strip-patched!"; break
    print(f"  source OK: {rc} records, strips intact")
    good=build_patched_spell_dbc(good)
    print("Patching patch-4.MPQ ...")
    with open(TARGET,'rb') as f: raw=bytearray(f.read())
    bidx,btoff,btsz=find_block_idx(raw,SPELL_PATH)
    assert bidx is not None
    bt=bytearray(decrypt(bytes(raw[btoff:btoff+btsz*16]), mhash('(block table)',3)))
    compressed=b'\x02'+zlib.compress(bytes(good),9)
    new_off=len(raw); raw+=compressed
    struct.pack_into('<IIII',bt,bidx*16,new_off,len(compressed),len(good),0x81000200)
    raw[btoff:btoff+btsz*16]=encrypt(bytes(bt), mhash('(block table)',3))
    struct.pack_into('<I',raw,8,len(raw))
    if not os.path.exists(BAK):
        print("Backing up -> patch-4.MPQ.spellfix.bak"); shutil.copy2(TARGET,BAK)
    with open(TARGET,'wb') as f: f.write(raw)
    print(f"Done. patch-4.MPQ updated ({len(raw):,} bytes). Verifying ...")
    ver=mpyq.MPQArchive(TARGET).read_file(SPELL_PATH)
    m,vrc,vfc,vrs,vsb=struct.unpack_from('<4sIIII',ver,0)
    ids=set(); hs=None
    for i in range(vrc):
        o=20+i*vrs; sid=struct.unpack_from('<I',ver,o)[0]; ids.add(sid)
        if sid==48952: hs=struct.unpack_from('<i',ver,o+68*4)[0]
    assert hs==-1,"VERIFY FAILED: strips lost"
    miss=[sid for sid,*_ in AA_BUFFS if sid not in ids]
    assert not miss, f"VERIFY FAILED: missing {miss}"
    print(f"  VERIFIED: strips intact + all {len(AA_BUFFS)} buff spells present.")
    print("  Delete Cache\\WDB\\enUS\\*.wdb and restart via WowExt.exe.")

if __name__=='__main__':
    if len(sys.argv)>2 and sys.argv[1]=='--sql':
        emit_sql(sys.argv[2])
    else:
        patch_client()
