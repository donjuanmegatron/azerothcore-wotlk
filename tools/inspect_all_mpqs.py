"""
inspect_all_mpqs.py — READ-ONLY diagnostic.
Enumerates every MPQ in Data\ (root) and Data\enUS\ (locale), determines which
contain DBFilesClient\Spell.dbc, and for each such file reports:
  - Holy Shield (48952) field [68] EquippedItemClass (-1 == strip applied)
  - Holy Shield [69] SubClassMask, [4] Attributes
  - Bear Form (5487) effect-0 shapeshift info (to spot the Druid-form-patched file)
Tries mpyq first (real Blizzard MPQs w/ listfile); falls back to a manual
hash-table extractor (our hand-built MPQs that lack a (listfile)).
Modifies nothing.
"""
import struct, zlib, os, sys, glob
import patch_aa_spells as base  # mhash, decrypt, CT

ROOT   = r'C:\Users\Donald Lee Irwin Esq\Desktop\3.3.5\Data'
LOCALE = r'C:\Users\Donald Lee Irwin Esq\Desktop\3.3.5\Data\enUS'
SPELL_PATH = 'DBFilesClient\\Spell.dbc'

def manual_extract(mpq_path, path_in_mpq):
    """Read a file from an MPQ via hash table + block table (no listfile needed).
    Handles single-unit compressed (0x02 zlib) and stored. Returns bytes or None."""
    raw = open(mpq_path, 'rb').read()
    # find MPQ header (may have an offset, but ours/Blizzard's start at 0 or have a shunt)
    off0 = raw.find(b'MPQ\x1a')
    if off0 < 0:
        return None
    try:
        magic, hdr, arch_sz, fmt, ss, ht_off, bt_off, ht_n, bt_n = struct.unpack_from('<4sIIHHIIII', raw, off0)
    except struct.error:
        return None
    if magic != b'MPQ\x1a':
        return None
    ht_off += off0; bt_off += off0
    try:
        ht = base.decrypt(raw[ht_off:ht_off+ht_n*16], base.mhash('(hash table)', 3))
        bt = base.decrypt(raw[bt_off:bt_off+bt_n*16], base.mhash('(block table)', 3))
    except Exception:
        return None
    ha, hb = base.mhash(path_in_mpq, 1), base.mhash(path_in_mpq, 2)
    start = base.mhash(path_in_mpq, 0) % ht_n
    bidx = None
    for p in range(ht_n):
        slot = (start + p) % ht_n
        e_ha, e_hb, loc, plat, e_bidx = struct.unpack_from('<IIHHI', ht, slot*16)
        if e_bidx == 0xFFFFFFFF:
            continue  # don't break — Blizzard tables may have deleted slots; scan all
        if e_ha == ha and e_hb == hb:
            bidx = e_bidx; break
    if bidx is None or bidx >= bt_n:
        return None
    f_off, csize, usize, flags = struct.unpack_from('<IIII', bt, bidx*16)
    f_off += off0
    comp = raw[f_off:f_off+csize]
    if len(comp) == 0:
        return None
    # Single-unit (no sector table) is what our create_mpq writes (flags 0x81000200).
    # Blizzard files are usually multi-sector — for those mpyq path handles it; this
    # manual path is mainly for our hand-built single-unit archives.
    try:
        if flags & 0x00000200:  # compressed
            if comp[0] == 0x02:
                data = zlib.decompress(comp[1:])
            else:
                # could be multi-sector; bail to mpyq
                return None
        else:
            data = comp[:usize]
        if len(data) != usize:
            return None
        return data
    except Exception:
        return None

def try_mpyq(mpq_path, path_in_mpq):
    try:
        import mpyq
        arch = mpyq.MPQArchive(mpq_path)
        data = arch.read_file(path_in_mpq)
        return data
    except Exception:
        return None

def get_spell_dbc(mpq_path):
    data = try_mpyq(mpq_path, SPELL_PATH)
    src = 'mpyq'
    if data is None:
        data = manual_extract(mpq_path, SPELL_PATH)
        src = 'manual'
    if data is None:
        return None, None
    if data[:4] != b'WDBC':
        return None, None
    return data, src

def s32(v): return v - 0x100000000 if v >= 0x80000000 else v

def parse_dbc(data):
    magic, rc, fc, rs, sbsz = struct.unpack_from('<4sIIII', data, 0)
    recs = {}
    for i in range(rc):
        boff = 20 + i*rs
        sid = struct.unpack_from('<I', data, boff)[0]
        recs[sid] = boff
    return recs, fc, rs, data

def fld(data, boff, idx):
    return struct.unpack_from('<I', data, boff + idx*4)[0]

def main():
    mpqs = sorted(glob.glob(os.path.join(ROOT, '*.MPQ'))) + \
           sorted(glob.glob(os.path.join(LOCALE, '*.MPQ')))
    print(f"{'MPQ':45} {'Spell.dbc?':10} {'src':7} {'HS[68]':8} {'HS[69]':8} {'HS attr4':10} {'Bear[eff0 shift]':18}")
    print('-'*120)
    for m in mpqs:
        name = os.path.relpath(m, ROOT)
        data, src = get_spell_dbc(m)
        if data is None:
            print(f"{name:45} {'no':10} {'-':7}")
            continue
        recs, fc, rs, data = parse_dbc(data)
        if 48952 not in recs:
            print(f"{name:45} {'yes':10} {src:7} {'NO-HS':8} fc={fc}")
            continue
        hb = recs[48952]
        hs68 = s32(fld(data, hb, 68))
        hs69 = fld(data, hb, 69)
        hs4  = fld(data, hb, 4)
        # Bear Form 5487: effect indices for 3.3.5a Spell.dbc (234 fields):
        #   Effect_1 starts at [107]; EffectMiscValue_1 cluster — shapeshift form is
        #   stored in EffectMiscValue. We just dump [110..112] region for effect-0
        #   plus a couple candidate misc fields. We mainly want "does it look stripped".
        bear = recs.get(5487)
        if bear is not None:
            # dump Effect_1[107], EffectMiscValue_1 (commonly [128] in 234-field layout)
            eff1 = fld(data, bear, 107)
            misc = fld(data, bear, 128)
            bearstr = f"eff1={eff1},misc={misc}"
        else:
            bearstr = "ABSENT"
        print(f"{name:45} {'yes':10} {src:7} {hs68:<8} {hs69:<8} 0x{hs4:08X} {bearstr:18} fc={fc} rc={len(recs)}")

if __name__ == '__main__':
    main()
