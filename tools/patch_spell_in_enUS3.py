"""
patch_spell_in_enUS3.py

THE definitive client Spell.dbc patch. patch-enUS-3.MPQ is the archive the 3.3.5a
client actually reads its DBCs from (PROVEN: patch_item_dbc.py patches Item.dbc here
and the custom item icons work in-game). A stock client ignores patch-4/5/6 and
patch-enUS-4/5 — which is why every earlier attempt was invisible.

This strips shield/weapon/stance requirements from Spell.dbc and SURGICALLY injects
the result into patch-enUS-3.MPQ using the SAME proven method as patch_item_dbc.py
(append compressed DBC, redirect the block-table entry for DBFilesClient\\Spell.dbc).
It preserves the existing Item.dbc patch (only the Spell.dbc block entry is touched)
and the full Spell.dbc string block.

Backup -> patch-enUS-3.MPQ.spellbak (revert by restoring it). After: delete
Cache\\WDB\\enUS\\*.wdb and fully restart the client.
"""
import mpyq, struct, shutil, os, zlib

SRC = r'C:\Users\Donald Lee Irwin Esq\Desktop\3.3.5\Data\enUS\patch-enUS-3.MPQ'
BAK = SRC + '.spellbak'
SPELL_PATH = 'DBFilesClient\\Spell.dbc'

# ─── MPQ crypto — copied verbatim from patch_item_dbc.py (the CORRECT _kstep) ───
def _ct():
    t = [0]*0x500; s = 0x00100001
    for i in range(0x100):
        idx = i
        for _ in range(5):
            s = (s*125+3) % 0x2AAAAB; t1 = (s & 0xFFFF) << 16
            s = (s*125+3) % 0x2AAAAB; t[idx] = t1 | (s & 0xFFFF); idx += 0x100
    return t
CT = _ct()
def _kstep(k):
    return (( ((~k & 0xFFFFFFFF) << 21) & 0xFFFFFFFF ) + 0x11111111 | (k >> 11)) & 0xFFFFFFFF
def mhash(s, ht):
    s1, s2 = 0x7FED7FED, 0xEEEEEEEE
    for c in s.upper().encode('ascii'):
        s1 = (CT[(ht<<8)+c] ^ (s1+s2)) & 0xFFFFFFFF
        s2 = (c + s1 + s2 + (s2<<5) + 3) & 0xFFFFFFFF
    return s1
def decrypt(data, key):
    key &= 0xFFFFFFFF; seed = 0xEEEEEEEE; out = bytearray(len(data))
    for i in range(0, len(data), 4):
        seed = (seed + CT[0x400 + (key & 0xFF)]) & 0xFFFFFFFF
        ciph = struct.unpack_from('<I', data, i)[0]
        plain = (ciph ^ (key + seed)) & 0xFFFFFFFF
        struct.pack_into('<I', out, i, plain)
        key = _kstep(key); seed = (plain + seed + (seed<<5) + 3) & 0xFFFFFFFF
    return bytes(out)
def encrypt(data, key):
    key &= 0xFFFFFFFF; seed = 0xEEEEEEEE; out = bytearray(len(data))
    for i in range(0, len(data), 4):
        seed = (seed + CT[0x400 + (key & 0xFF)]) & 0xFFFFFFFF
        plain = struct.unpack_from('<I', data, i)[0]
        ciph = (plain ^ (key + seed)) & 0xFFFFFFFF
        struct.pack_into('<I', out, i, ciph)
        key = _kstep(key); seed = (plain + seed + (seed<<5) + 3) & 0xFFFFFFFF
    return bytes(out)

# ─── strip constants (mirror server / patch6_inplace_strip) ───
F_ATTR, F_STANCES, F_STANCES_NOT = 4, 12, 13
F_EQ_CLASS, F_EQ_SUB, F_EQ_INV = 68, 69, 70
ATTR_NOT_SHAPESHIFTED = 0x10000
WEAPON, ARMOR = 2, 4
SHIELD_SUB = 1 << 6
INV_SHIELD = 1 << 14
NEG1 = 0xFFFFFFFF
MELEE  = (1<<0)|(1<<1)|(1<<4)|(1<<5)|(1<<6)|(1<<7)|(1<<8)|(1<<10)|(1<<11)|(1<<12)|(1<<13)|(1<<14)|(1<<15)|(1<<17)|(1<<20)
RANGED = (1<<2)|(1<<3)|(1<<16)|(1<<18)|(1<<19)
def s32(v): return v - 0x100000000 if v >= 0x80000000 else v

print("Extracting Spell.dbc from patch-enUS-3.MPQ via mpyq...")
arch = mpyq.MPQArchive(SRC)
raw_dbc = arch.read_file(SPELL_PATH)
assert raw_dbc, "Spell.dbc not found"
magic, rc, fc, rs, sbsz = struct.unpack_from('<4sIIII', raw_dbc, 0)
assert magic == b'WDBC' and fc == 234, f"bad DBC fc={fc}"
HDR = 20
recs = [list(struct.unpack_from('<234I', raw_dbc, HDR + i*rs)) for i in range(rc)]
strblk = raw_dbc[HDR + rc*rs : HDR + rc*rs + sbsz]
print(f"  {rc} records, strblock {sbsz}b")

shield = weapon = stance = naked = 0
for r in recs:
    if r[F_STANCES] or r[F_STANCES_NOT]:
        r[F_STANCES] = 0; r[F_STANCES_NOT] = 0; stance += 1
    if r[F_ATTR] & ATTR_NOT_SHAPESHIFTED:
        r[F_ATTR] &= (~ATTR_NOT_SHAPESHIFTED) & 0xFFFFFFFF; naked += 1
    ec = s32(r[F_EQ_CLASS])
    if (ec == ARMOR and (r[F_EQ_SUB] & SHIELD_SUB)) or (r[F_EQ_INV] & INV_SHIELD):
        r[F_EQ_CLASS] = NEG1; r[F_EQ_SUB] = 0; r[F_EQ_INV] = 0; shield += 1
    elif ec == WEAPON:
        if (r[F_EQ_SUB] & RANGED) and not (r[F_EQ_SUB] & MELEE):
            pass
        elif r[F_EQ_SUB] != MELEE or r[F_EQ_INV] != 0:
            r[F_EQ_SUB] = MELEE; r[F_EQ_INV] = 0; weapon += 1
print(f"  stripped: shield={shield} weapon={weapon} stance={stance} not-shapeshifted={naked}")

new_dbc = struct.pack('<4sIIII', b'WDBC', rc, fc, rs, sbsz)
new_dbc += b''.join(struct.pack('<234I', *r) for r in recs)
new_dbc += strblk
assert len(new_dbc) == len(raw_dbc), f"size drift {len(new_dbc)} != {len(raw_dbc)}"

print("Surgical inject into MPQ...")
raw = bytearray(open(SRC, 'rb').read())
sig, hdr_sz, arc_sz, fmtver, sec_shift, ht_off, bt_off, ht_size, bt_size = struct.unpack_from('<4sIIHHIIII', raw, 0)
assert sig == b'MPQ\x1a'
ht_dec = decrypt(raw[ht_off:ht_off+ht_size*16], mhash('(hash table)', 3))
ha, hb = mhash(SPELL_PATH, 1), mhash(SPELL_PATH, 2)
start = mhash(SPELL_PATH, 0) % ht_size
block_idx = None
for probe in range(ht_size):
    sslot = (start + probe) % ht_size
    eha, ehb, loc, plat, bidx = struct.unpack_from('<IIHHI', ht_dec, sslot*16)
    if bidx == 0xFFFFFFFF: break
    if eha == ha and ehb == hb: block_idx = bidx; break
assert block_idx is not None, "Spell.dbc not in hash table"
bt_dec = bytearray(decrypt(raw[bt_off:bt_off+bt_size*16], mhash('(block table)', 3)))
boff, bcsz, busz, bflags = struct.unpack_from('<IIII', bt_dec, block_idx*16)
print(f"  old Spell.dbc block: off={boff} csize={bcsz} usize={busz} flags=0x{bflags:08X}")
compressed = b'\x02' + zlib.compress(bytes(new_dbc), 9)
new_off = len(raw)
raw += compressed
struct.pack_into('<IIII', bt_dec, block_idx*16, new_off, len(compressed), len(new_dbc), 0x81000200)
print(f"  new Spell.dbc block: off={new_off} csize={len(compressed)} usize={len(new_dbc)}")
raw[bt_off:bt_off+bt_size*16] = encrypt(bytes(bt_dec), mhash('(block table)', 3))
struct.pack_into('<I', raw, 8, len(raw))  # update archive_size

if not os.path.exists(BAK):
    shutil.copy2(SRC, BAK); print(f"  backed up -> {os.path.basename(BAK)}")
with open(SRC, 'wb') as f:
    f.write(raw)
print(f"Done. patch-enUS-3.MPQ updated ({len(raw):,} bytes). Clear WDB cache + fully restart client.")
