"""
patch_spell_in_patch4.py  —  THE REAL FIX.

ROOT CAUSE (proven 2026-06-30): the 3.3.5a client's EFFECTIVE Spell.dbc is the one
inside  Data\patch-4.MPQ  (root), NOT  Data\enUS\patch-enUS-3.MPQ.

Evidence:
  * Binary load list in Wow.exe @0x5e0ea0 registers, in order:
      patch-enUS-2, patch-2, patch-enUS-3, patch-3, patch-enUS-4, patch-4, patch, ...
    Later-registered archives get HIGHER MPQ priority, so root patch-4 OVERRIDES
    locale patch-enUS-3 for any file present in BOTH.
  * patch-4.MPQ contains ONLY DBFilesClient\Spell.dbc (its (listfile) confirms it),
    valid 234-field WotLK layout, Holy Shield[68]=4 (shield still required).
  * patch-4 does NOT contain Item.dbc, so enUS-3's patched Item.dbc still wins =>
    custom item icons work (matches reality).
  * patch-5/6 and patch-enUS-4 are MALFORMED hand-built MPQs (garbage hash/block
    tables; mpyq + a correct manual walk both fail to read Spell.dbc from them) =>
    every prior patch to those was invisible.

This script takes the ALREADY-PATCHED Spell.dbc (Holy Shield[68]=-1, plus the full
shield/weapon/stance strips) out of patch-enUS-3.MPQ and surgically injects it into
patch-4.MPQ's Spell.dbc block, using the EXACT proven method of patch_item_dbc.py
(append single-unit-zlib block, redirect the block-table entry, fix archive size).

Backup: patch-4.MPQ.spellfix.bak  (revert by restoring it).
After running: delete Cache\WDB\enUS\*.wdb and FULLY restart via WowExt.exe.
"""
import mpyq, struct, shutil, os, zlib, sys

CLIENT = r'C:\Users\Donald Lee Irwin Esq\Desktop\3.3.5'
PATCHED_SRC = os.path.join(CLIENT, r'Data\enUS\patch-enUS-3.MPQ')  # has the GOOD Spell.dbc
TARGET      = os.path.join(CLIENT, r'Data\patch-4.MPQ')             # what the client reads
BAK         = TARGET + '.spellfix.bak'
SPELL_PATH  = 'DBFilesClient\\Spell.dbc'

# ---- MPQ crypto (CORRECT _kstep, +0x11111111) ----
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
        s1=(CT[(ht<<8)+c]^(s1+s2))&0xFFFFFFFF
        s2=(c+s1+s2+(s2<<5)+3)&0xFFFFFFFF
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

def find_block_idx(raw, name):
    sig,hdrsz,arcsz,ver,ss,htoff,btoff,htsz,btsz=struct.unpack_from('<4sIIHHIIII',raw,0)
    assert sig==b'MPQ\x1a'
    ht=decrypt(raw[htoff:htoff+htsz*16], mhash('(hash table)',3))
    ha,hb=mhash(name,1),mhash(name,2); start=mhash(name,0)%htsz
    for probe in range(htsz):
        s=(start+probe)%htsz
        eha,ehb,loc,plat,bidx=struct.unpack_from('<IIHHI',ht,s*16)
        if bidx==0xFFFFFFFF: continue
        if eha==ha and ehb==hb:
            return bidx, btoff, btsz
    return None, btoff, btsz

# 1) Get the GOOD, already-patched Spell.dbc bytes from enUS-3
print("Reading patched Spell.dbc from patch-enUS-3.MPQ ...")
good = mpyq.MPQArchive(PATCHED_SRC).read_file(SPELL_PATH)
assert good and good[:4]==b'WDBC'
magic,rc,fc,rs,sb=struct.unpack_from('<4sIIII',good,0)
assert fc==234 and rs==936, f"unexpected layout fc={fc} rs={rs}"
# sanity: Holy Shield [68] must be -1 in the source
hdr=20
for i in range(rc):
    o=hdr+i*rs
    if struct.unpack_from('<I',good,o)[0]==48952:
        assert struct.unpack_from('<i',good,o+68*4)[0]==-1, "source Spell.dbc not patched!"
        break
print(f"  source OK: {rc} records, 234 fields, Holy Shield[68]=-1")

# 2) Surgically inject into patch-4.MPQ
print("Patching patch-4.MPQ ...")
with open(TARGET,'rb') as f: raw=bytearray(f.read())
bidx, btoff, btsz = find_block_idx(raw, SPELL_PATH)
assert bidx is not None, "Spell.dbc block not found in patch-4 hash table"
bt=bytearray(decrypt(bytes(raw[btoff:btoff+btsz*16]), mhash('(block table)',3)))
boff,bcsz,busz,bflags=struct.unpack_from('<IIII',bt,bidx*16)
print(f"  old Spell.dbc block: off={boff} csize={bcsz} usize={busz} flags={bflags:#010x}")

compressed = b'\x02' + zlib.compress(bytes(good), 9)   # single-unit zlib, like patch_item_dbc.py
new_off = len(raw)
raw += compressed
struct.pack_into('<IIII', bt, bidx*16, new_off, len(compressed), len(good), 0x81000200)
print(f"  new Spell.dbc block: off={new_off} csize={len(compressed)} usize={len(good)} flags=0x81000200")

bt_enc = encrypt(bytes(bt), mhash('(block table)',3))
raw[btoff:btoff+btsz*16] = bt_enc
struct.pack_into('<I', raw, 8, len(raw))   # archive size field

if not os.path.exists(BAK):
    print("Backing up patch-4.MPQ -> patch-4.MPQ.spellfix.bak")
    shutil.copy2(TARGET, BAK)
with open(TARGET,'wb') as f: f.write(raw)
print(f"Done. patch-4.MPQ updated ({len(raw):,} bytes).")

# 3) VERIFY by re-following the real block table (NOT a naive offset scan)
print("Verifying via real block-table walk ...")
ver = mpyq.MPQArchive(TARGET).read_file(SPELL_PATH)
assert ver and ver[:4]==b'WDBC'
m,vrc,vfc,vrs,vsb=struct.unpack_from('<4sIIII',ver,0)
for i in range(vrc):
    o=20+i*vrs
    if struct.unpack_from('<I',ver,o)[0]==48952:
        f68=struct.unpack_from('<i',ver,o+68*4)[0]
        f69=struct.unpack_from('<i',ver,o+69*4)[0]
        print(f"  patch-4 Spell.dbc Holy Shield: [68]={f68} [69]={f69}  (expect -1 / 0)")
        assert f68==-1, "VERIFY FAILED"
        break
print("VERIFIED: patch-4.MPQ now serves Holy Shield[68]=-1. Delete Cache\\WDB\\enUS\\*.wdb and restart via WowExt.exe.")
