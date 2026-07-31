# Inject custom BLP tabard textures into Data\patch-4.MPQ (override base tabards).
import struct, os, zlib, shutil
from PIL import Image
ROOT = r'C:\Users\Donald Lee Irwin Esq\Desktop\3.3.5\Data'
SP   = r'C:\Users\DONALD~1\AppData\Local\Temp\claude\C--Users-Donald-Lee-Irwin-Esq-Desktop-claude-code-dump-ClaudeCodeTest\faf928bf-6274-4c2d-9d6c-1457904d4d1f\scratchpad\tabard'
MPQ  = os.path.join(ROOT, 'patch-4.MPQ')

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
 (r'Item\TextureComponents\TorsoUpperTexture\TABARD_A_01ARGENTDAWN_CHEST_TU_U.BLP', SP+r'\vigil_TU.png'),
 (r'Item\TextureComponents\TorsoLowerTexture\TABARD_A_01ARGENTDAWN_CHEST_TL_U.BLP', SP+r'\vigil_TL.png'),
]

if not os.path.exists(MPQ+'.tabardbak'):
    shutil.copy(MPQ, MPQ+'.tabardbak'); print("backup -> patch-4.MPQ.tabardbak")
raw=bytearray(open(MPQ,'rb').read())
htoff=struct.unpack_from('<I',raw,0x10)[0]; htsz=struct.unpack_from('<I',raw,0x18)[0]
btoff=struct.unpack_from('<I',raw,0x14)[0]; btsz=struct.unpack_from('<I',raw,0x1C)[0]
ht=bytearray(decrypt(bytes(raw[htoff:htoff+htsz*16]), mhash('(hash table)',3)))
bt=bytearray(decrypt(bytes(raw[btoff:btoff+btsz*16]), mhash('(block table)',3)))
append=bytearray(); base=len(raw); newblocks=[]
for path,png in FILES:
    blp=enc_blp(Image.open(png)); comp=b'\x02'+zlib.compress(blp,9)
    foff=base+len(append); append+=comp
    newblocks.append((foff,len(comp),len(blp),0x81000200,path))
for foff,cs,us,fl,path in newblocks:
    bi=btsz; btsz+=1
    slot=mhash(path,0)%htsz; s=slot
    while True:
        exist=struct.unpack_from('<I',ht,s*16+12)[0]
        if exist in (0xFFFFFFFF,0xFFFFFFFE): break
        s=(s+1)%htsz
        if s==slot: raise SystemExit("hash table full")
    struct.pack_into('<IIHHI',ht,s*16, mhash(path,1),mhash(path,2),0,0,bi)
    bt+=struct.pack('<IIII',foff,cs,us,fl)
raw+=append
new_bt_off=len(raw); raw+=encrypt(bytes(bt), mhash('(block table)',3))
new_ht_off=len(raw); raw+=encrypt(bytes(ht), mhash('(hash table)',3))
struct.pack_into('<I',raw,0x10,new_ht_off); struct.pack_into('<I',raw,0x18,htsz)
struct.pack_into('<I',raw,0x14,new_bt_off); struct.pack_into('<I',raw,0x1C,btsz)
struct.pack_into('<I',raw,0x08,len(raw))
open(MPQ,'wb').write(raw)
print(f"injected {len(FILES)} files. patch-4 now {len(raw):,} bytes, blocks={btsz}")
