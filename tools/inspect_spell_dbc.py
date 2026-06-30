"""
inspect_spell_dbc.py — READ-ONLY. Dumps key Spell.dbc fields for known spells so we
can EMPIRICALLY verify the 3.3.5a field indices for EquippedItemClass /
EquippedItemSubClassMask / EquippedItemInventoryTypeMask / Stances / StancesNot /
Attributes(NOT_SHAPESHIFTED) before writing the shield/weapon/stance client patch.
Does NOT modify anything.
"""
import mpyq, struct

SRC_MPQ    = r'C:\Users\Donald Lee Irwin Esq\Desktop\3.3.5\Data\enUS\patch-enUS-3.MPQ'
SPELL_PATH = 'DBFilesClient\\Spell.dbc'

def load_dbc(arch, path):
    raw = arch.read_file(path)
    magic, rc, fc, rs, sbsz = struct.unpack_from('<4sIIII', raw, 0)
    assert magic == b'WDBC', f"{path}: bad magic {magic}"
    HDR = 20
    fmt = '<%dI' % fc
    records = [list(struct.unpack_from(fmt, raw, HDR + i * rs)) for i in range(rc)]
    return records, fc

# Known reference spells and what we EXPECT to find (signed where relevant):
#  48952 Holy Shield      -> EquippedItemClass=4 (ARMOR), SubClassMask has bit6 (64) shield
#  75    Auto Shot        -> EquippedItemClass=2 (WEAPON), SubClassMask = ranged (bow/gun/xbow)
#  2098  Eviscerate       -> EquippedItemClass=2 (WEAPON) generic melee
#  53    Backstab (rank1) -> EquippedItemClass=2 (WEAPON), SubClassMask bit15 (32768) dagger
#  1680  Whirlwind        -> Stances field non-zero (requires Battle/Berserker stance)
#  5487  Bear Form        -> shapeshift form spell
TARGETS = [48952, 75, 2098, 53, 1680, 5487, 845, 23922]  # +Cleave 845, Shield Slam 23922

def main():
    arch = mpyq.MPQArchive(SRC_MPQ)
    recs, fc = load_dbc(arch, SPELL_PATH)
    print(f"Spell.dbc: {len(recs)} records, {fc} fields/record")
    by_id = {r[0]: r for r in recs}

    # Signed view helper (EquippedItemClass is int32, -1 default)
    def s32(v): return v - 0x100000000 if v >= 0x80000000 else v

    for sid in TARGETS:
        r = by_id.get(sid)
        if not r:
            print(f"\n--- spell {sid}: NOT FOUND")
            continue
        print(f"\n--- spell {sid} ---  Attributes[4]=0x{r[4]:08X}")
        # Scan fields 60-75 (the EquippedItem* cluster lives here in 3.3.5a) and
        # 10-20 (Stances cluster). Print index:value (and signed) for anything notable.
        print("  [fields 60-75]:")
        for i in range(60, 76):
            print(f"    [{i}] = {r[i]} (s32={s32(r[i])})")
        print("  [fields 10-20] (stance cluster):")
        for i in range(10, 21):
            print(f"    [{i}] = {r[i]}")

if __name__ == '__main__':
    main()
