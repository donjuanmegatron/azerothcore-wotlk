"""
verify_spell_patch.py — READ-ONLY read-back check of patch-enUS-5.MPQ.
Manually extracts DBFilesClient\\Spell.dbc from the generated MPQ (mpyq can't open it
— no listfile) and confirms the strips landed correctly on known spells. Verifies the
MPQ is well-formed AND the patch is correct before the client ever loads it.
"""
import struct, zlib, os
import patch_aa_spells as base   # mhash, decrypt

MPQ = r'C:\Users\Donald Lee Irwin Esq\Desktop\3.3.5\Data\enUS\patch-enUS-4.MPQ'
SPELL_PATH = 'DBFilesClient\\Spell.dbc'

MELEE_MASK  = (1<<0)|(1<<1)|(1<<4)|(1<<5)|(1<<6)|(1<<7)|(1<<8)|(1<<10)|(1<<11)|(1<<12)|(1<<13)|(1<<14)|(1<<15)|(1<<17)|(1<<20)

def extract(path_in_mpq):
    raw = open(MPQ, 'rb').read()
    magic, hdr, arch_sz, fmt, ss, ht_off, bt_off, ht_n, bt_n = struct.unpack_from('<4sIIHHIIII', raw, 0)
    assert magic == b'MPQ\x1a', "bad MPQ magic"
    ht = base.decrypt(raw[ht_off:ht_off+ht_n*16], base.mhash('(hash table)', 3))
    bt = base.decrypt(raw[bt_off:bt_off+bt_n*16], base.mhash('(block table)', 3))
    ha, hb = base.mhash(path_in_mpq, 1), base.mhash(path_in_mpq, 2)
    start = base.mhash(path_in_mpq, 0) % ht_n
    bidx = None
    for p in range(ht_n):
        slot = (start + p) % ht_n
        e_ha, e_hb, loc, plat, e_bidx = struct.unpack_from('<IIHHI', ht, slot*16)
        if e_bidx == 0xFFFFFFFF:
            break
        if e_ha == ha and e_hb == hb:
            bidx = e_bidx; break
    assert bidx is not None, f"{path_in_mpq} not found in MPQ hash table"
    off, csize, usize, flags = struct.unpack_from('<IIII', bt, bidx*16)
    comp = raw[off:off+csize]
    data = zlib.decompress(comp[1:]) if comp[0] == 0x02 else comp
    assert len(data) == usize, f"size mismatch {len(data)} != {usize}"
    return data

def main():
    print(f"Extracting {SPELL_PATH} from {os.path.basename(MPQ)} ...")
    data = extract(SPELL_PATH)
    magic, rc, fc, rs, sbsz = struct.unpack_from('<4sIIII', data, 0)
    assert magic == b'WDBC' and fc == 234, "bad DBC"
    print(f"  OK: {rc} records, {fc} fields. Checking known spells:\n")
    recs = {}
    for i in range(rc):
        base_off = 20 + i*rs
        sid = struct.unpack_from('<I', data, base_off)[0]
        recs[sid] = base_off
    def fld(sid, idx): return struct.unpack_from('<I', data, recs[sid] + idx*4)[0]
    def s32(v): return v - 0x100000000 if v >= 0x80000000 else v

    checks = []
    # Holy Shield 48952: EquippedItemClass -> -1, masks 0, NOT_SHAPESHIFTED cleared
    checks.append(("Holy Shield 48952 EquipClass==-1", s32(fld(48952,68)) == -1))
    checks.append(("Holy Shield 48952 SubMask==0",     fld(48952,69) == 0))
    checks.append(("Holy Shield 48952 NOT_SHAPESHIFTED cleared", (fld(48952,4) & 0x10000) == 0))
    # Whirlwind 1680: Stances zeroed
    checks.append(("Whirlwind 1680 Stances==0",        fld(1680,12) == 0))
    # Backstab 53: weapon kept, submask broadened to all-melee
    checks.append(("Backstab 53 EquipClass==2",        s32(fld(53,68)) == 2))
    checks.append(("Backstab 53 SubMask==MELEE_MASK",  fld(53,69) == MELEE_MASK))
    # Auto Shot 75: ranged requirement UNCHANGED (must still need a bow/gun/xbow)
    checks.append(("Auto Shot 75 EquipClass==2",       s32(fld(75,68)) == 2))
    checks.append(("Auto Shot 75 ranged mask kept (==262156)", fld(75,69) == 262156))
    # AA spell preserved
    checks.append(("AA Manaburn 701015 present",        701015 in recs))

    ok = True
    for name, passed in checks:
        print(f"   [{'PASS' if passed else 'FAIL'}] {name}")
        ok = ok and passed
    print("\n" + ("ALL CHECKS PASSED — patch is correct." if ok else "*** SOME CHECKS FAILED — DO NOT LOAD ***"))
    return 0 if ok else 1

if __name__ == '__main__':
    raise SystemExit(main())
