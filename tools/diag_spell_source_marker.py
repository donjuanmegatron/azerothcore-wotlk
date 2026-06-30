"""
diag_spell_source_marker.py  —  SINGLE-LOGIN diagnostic (fallback only).

ONLY run this if the patch_spell_in_patch4.py fix did NOT remove the Holy Shield
shield-requirement in-game (it should — patch-4 is the proven winner). It writes a
DISTINCT SpellIconID into Holy Shield (48952) field [133] in each readable Spell.dbc
candidate, so whichever ICON shows on the Holy Shield action button in-game tells you
exactly which archive the client is reading Spell.dbc from.

Marker icon IDs (all are real, visually distinct 3.3.5a SpellIconIDs):
  patch-4.MPQ            -> 165   (a fireball)            "fire"  => patch-4 wins
  patch-enUS-3.MPQ      -> 188   (a frost/ice nova)      "ice"   => enUS-3 wins
  patch-enUS-2.MPQ      -> NOTE: 240-field layout, [133] is a DIFFERENT column; skipped
                              (a 3.3.5a client can't load a 240-field Spell.dbc anyway)

After running: delete Cache\WDB\enUS\*.wdb, restart via WowExt.exe, look at the Holy
Shield button icon. Then re-run with RESTORE=1 (or restore the .diagbak files) to undo.
"""
import mpyq, struct, shutil, os, zlib, sys

CLIENT = r'C:\Users\Donald Lee Irwin Esq\Desktop\3.3.5'
RESTORE = os.environ.get('RESTORE') == '1'

# (path, marker_iconID).  Only 234-field archives — the others can't be the live one.
CANDIDATES = [
    (os.path.join(CLIENT, r'Data\patch-4.MPQ'),            165),
    (os.path.join(CLIENT, r'Data\enUS\patch-enUS-3.MPQ'),  188),
]
SPELL_PATH='DBFilesClient\\Spell.dbc'
ICON_FIELD=133

def _ct():
    t=[0]*0x500; s=0x00100001
    for i in range(0x100):
        idx=i
        for _ in range(5):
            s=(s*125+3)%0x2AAAAB; t1=(s&0xFFFF)<<16
            s=(s*125+3)%0x2AAAAB; t[idx]=t1|(s&0xFFFF); idx+=0x100
    return t
CT=_ct()
def _kstep(k): return ((((~k&0xFFFFFFFF)<<21)&0xFFFFFFFF)+0x11111111 | (k>>11))&0xFFFFFFFF
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
def block_for(raw,name):
    sig,hdrsz,arcsz,ver,ss,htoff,btoff,htsz,btsz=struct.unpack_from('<4sIIHHIIII',raw,0)
    ht=decrypt(raw[htoff:htoff+htsz*16],mhash('(hash table)',3))
    ha,hb=mhash(name,1),mhash(name,2); start=mhash(name,0)%htsz
    for p in range(htsz):
        s=(start+p)%htsz
        eha,ehb,loc,plat,bidx=struct.unpack_from('<IIHHI',ht,s*16)
        if bidx==0xFFFFFFFF: continue
        if eha==ha and ehb==hb: return bidx,btoff,btsz
    return None,btoff,btsz

for path,icon in CANDIDATES:
    bak=path+'.diagbak'
    if RESTORE:
        if os.path.exists(bak):
            shutil.copy2(bak,path); print('restored',os.path.basename(path))
        continue
    dbc=mpyq.MPQArchive(path).read_file(SPELL_PATH)
    m,rc,fc,rs,sb=struct.unpack_from('<4sIIII',dbc,0)
    assert fc==234
    dbc=bytearray(dbc)
    for i in range(rc):
        o=20+i*rs
        if struct.unpack_from('<I',dbc,o)[0]==48952:
            struct.pack_into('<i',dbc,o+ICON_FIELD*4,icon); break
    with open(path,'rb') as f: raw=bytearray(f.read())
    bidx,btoff,btsz=block_for(raw,SPELL_PATH)
    bt=bytearray(decrypt(bytes(raw[btoff:btoff+btsz*16]),mhash('(block table)',3)))
    comp=b'\x02'+zlib.compress(bytes(dbc),9); noff=len(raw); raw+=comp
    struct.pack_into('<IIII',bt,bidx*16,noff,len(comp),len(dbc),0x81000200)
    raw[btoff:btoff+btsz*16]=encrypt(bytes(bt),mhash('(block table)',3))
    struct.pack_into('<I',raw,8,len(raw))
    if not os.path.exists(bak): shutil.copy2(path,bak)
    with open(path,'wb') as f: f.write(raw)
    print(f'marked {os.path.basename(path)} Holy Shield icon -> {icon}')

print('Done.' if not RESTORE else 'Restore complete.')
print('Delete Cache\\WDB\\enUS\\*.wdb, restart via WowExt.exe, read the Holy Shield button icon.')
print('  fireball  (165) => patch-4.MPQ is the live Spell.dbc')
print('  frost nova(188) => patch-enUS-3.MPQ is the live Spell.dbc')
