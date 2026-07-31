# Replace tabard BLP textures inside common.MPQ via in-place block-entry redirect
# (the proven patch_item_dbc method). Seek-based (no full-file rewrite). Reversible.
import struct, os, zlib, json
from PIL import Image
ROOT = r'C:\Users\Donald Lee Irwin Esq\Desktop\3.3.5\Data'
SP   = r'C:\Users\DONALD~1\AppData\Local\Temp\claude\C--Users-Donald-Lee-Irwin-Esq-Desktop-claude-code-dump-ClaudeCodeTest\faf928bf-6274-4c2d-9d6c-1457904d4d1f\scratchpad\tabard'
MPQ  = os.path.join(ROOT, 'common.MPQ')
REVERT = MPQ + '.tabardrevert.json'

def _prep():
    ct=[0]*0x500; seed=0x00100001
    for i in range(0x100):
        for j in range(5):
            seed=(seed*125+3)%0x2AAAAB; a=(seed&0xFFFF)<<16
            seed=(seed*125+3)%0x2AAAAB; b=(seed&0xFFFF); ct[i+j*0x100]=a|b
    return ct
CT=_prep()
def mhash(s,ty):
    s=s.upper().replace('/','\\'); s1=0x7FED7FED; s2=0xEEEEEEEE
    for ch in s.encode('ascii'):
        s1=CT[(ty<<8)+ch]^((s1+s2)&0xFFFFFFFF); s1&=0xFFFFFFFF
        s2=(ch+s1+s2+((s2<<5)&0xFFFFFFFF)+3)&0xFFFFFFFF
    return s1
def decrypt(d,key):
    out=bytearray(d); n=len(out)//4; s2=0xEEEEEEEE
    for i in range(n):
        s2=(s2+CT[0x400+(key&0xFF)])&0xFFFFFFFF
        v,=struct.unpack_from('<I',out,i*4); p=v^((key+s2)&0xFFFFFFFF)
        struct.pack_into('<I',out,i*4,p&0xFFFFFFFF)
        key=((((~key)<<21)&0xFFFFFFFF)+0x11111111)&0xFFFFFFFF | (key>>11)
        s2=(p+s2+((s2<<5)&0xFFFFFFFF)+3)&0xFFFFFFFF
    return bytes(out)
def encrypt(d,key):
    out=bytearray(d); n=len(out)//4; s2=0xEEEEEEEE
    for i in range(n):
        s2=(s2+CT[0x400+(key&0xFF)])&0xFFFFFFFF
        v,=struct.unpack_from('<I',out,i*4); c=v^((key+s2)&0xFFFFFFFF)
        struct.pack_into('<I',out,i*4,c&0xFFFFFFFF)
        key=((((~key)<<21)&0xFFFFFFFF)+0x11111111)&0xFFFFFFFF | (key>>11)
        s2=(v+s2+((s2<<5)&0xFFFFFFFF)+3)&0xFFFFFFFF
    return bytes(out)
def enc_blp(img):
    img=img.convert('RGBA'); w,h=img.size
    alpha=img.getchannel('A'); pimg=img.convert('RGB').convert('P',palette=Image.ADAPTIVE,colors=256)
    pal=(pimg.getpalette()[:768]+[0]*768)[:768]
    hdr=bytearray(148); hdr[0:4]=b'BLP2'; struct.pack_into('<I',hdr,4,1)
    hdr[8]=1; hdr[9]=8; hdr[10]=0; hdr[11]=1; struct.pack_into('<II',hdr,12,w,h)
    palb=bytearray()
    for i in range(256): palb+=bytes([pal[i*3+2],pal[i*3+1],pal[i*3],0])
    body=bytearray(); moff=[0]*16; msz=[0]*16; cur=148+1024
    cw,ch=w,h; lvl=0; p_l=pimg; a_l=alpha
    while lvl<16:
        idx=p_l.tobytes(); al=a_l.tobytes(); moff[lvl]=cur; msz[lvl]=len(idx)+len(al)
        body+=idx+al; cur+=len(idx)+len(al)
        if cw==1 and ch==1: break
        cw=max(1,cw//2); ch=max(1,ch//2); lvl+=1; p_l=pimg.resize((cw,ch)); a_l=alpha.resize((cw,ch))
    struct.pack_into('<16I',hdr,20,*moff); struct.pack_into('<16I',hdr,84,*msz)
    return bytes(hdr)+bytes(palb)+bytes(body)

FILES = [
 (r'Item\TextureComponents\TorsoUpperTexture\Tabard_A_01AllianceOfficerPVP_Chest_TU_U.blp',  SP+r'\ring_TU.png'),
 (r'Item\TextureComponents\TorsoLowerTexture\Tabard_A_01AllianceOfficerPVP_Chest_TL_U.blp',  SP+r'\ring_TL.png'),
 (r'Item\TextureComponents\TorsoUpperTexture\Tabard_A_01AllianceEnlistedPVP_Chest_TU_U.blp', SP+r'\bane_TU.png'),
 (r'Item\TextureComponents\TorsoLowerTexture\Tabard_A_01AllianceEnlistedPVP_Chest_TL_U.blp', SP+r'\bane_TL.png'),
]

f=open(MPQ,'r+b')
probe=f.read(0x400); base=probe.find(b'MPQ\x1a')
assert base>=0, "MPQ magic not found"
sig,hsz,asz,fmt,ssh,htoff,btoff,htn,btn=struct.unpack_from('<4sIIHHIIII',probe,base)
print(f"header at file offset {base}; htoff={htoff} btoff={btoff} htn={htn} btn={btn}")
htoff+=base; btoff+=base  # absolute file positions
f.seek(htoff); ht=decrypt(f.read(htn*16), mhash('(hash table)',3))
f.seek(btoff); btraw=f.read(btn*16); bt=bytearray(decrypt(btraw, mhash('(block table)',3)))

orig_size=os.path.getsize(MPQ)
if not os.path.exists(REVERT):
    json.dump({'btoff':btoff,'btn':btn,'orig_bt_enc':btraw.hex(),'orig_asz':asz,'orig_size':orig_size}, open(REVERT,'w'))
    print("wrote revert record ->", os.path.basename(REVERT))

# find block indices
def find_block(path):
    ha,hb=mhash(path,1),mhash(path,2); start=mhash(path,0)%htn
    for pr in range(htn):
        s=(start+pr)%htn; eha,ehb,loc,pl,bi=struct.unpack_from('<IIHHI',ht,s*16)
        if bi==0xFFFFFFFF: return None
        if eha==ha and ehb==hb: return bi
    return None

f.seek(0,2)  # EOF
for path,png in FILES:
    bi=find_block(path)
    assert bi is not None, "not found: "+path
    old=struct.unpack_from('<IIII',bt,bi*16)
    blp=enc_blp(Image.open(png)); comp=b'\x02'+zlib.compress(blp,9)
    newoff=f.tell()-base; f.write(comp)  # offset relative to archive header
    struct.pack_into('<IIII',bt,bi*16, newoff,len(comp),len(blp),0x81000200)
    print(f"  {os.path.basename(path)}: block {bi} old_off={old[0]} -> new_off={newoff} csize={len(comp)}")
# write block table back in place + archive size
new_size=f.tell()
f.seek(btoff); f.write(encrypt(bytes(bt), mhash('(block table)',3)))
f.seek(base+8); f.write(struct.pack('<I', (new_size-base) & 0xFFFFFFFF))
f.close()
print(f"done. common.MPQ {orig_size:,} -> {new_size:,}")
