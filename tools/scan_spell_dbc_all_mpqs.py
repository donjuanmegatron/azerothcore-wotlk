"""
Rigorously read Holy Shield (48952) field [68] EquippedItemClass from the
REAL stored Spell.dbc inside every MPQ in Data\ and Data\enUS\ that contains
DBFilesClient\Spell.dbc. Follows the actual hash+block tables, decrypts with
the correct key, decompresses per block flags (multi-sector aware).
"""
import struct, os, zlib, bz2, glob

# ---- MPQ crypto (CORRECT _kstep with +0x11111111) ----
def _ct():
    t=[0]*0x500; s=0x00100001
    for i in range(0x100):
        idx=i
        for _ in range(5):
            s=(s*125+3)%0x2AAAAB; t1=(s&0xFFFF)<<16
            s=(s*125+3)%0x2AAAAB; t[idx]=t1|(s&0xFFFF); idx+=0x100
    return t
CT=_ct()
def _kstep(k):
    return ((((~k & 0xFFFFFFFF)<<21)&0xFFFFFFFF)+0x11111111 | (k>>11))&0xFFFFFFFF
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
        c=struct.unpack_from('<I',data,i)[0]
        p=(c^(key+seed))&0xFFFFFFFF
        struct.pack_into('<I',out,i,p)
        key=_kstep(key); seed=(p+seed+(seed<<5)+3)&0xFFFFFFFF
    return bytes(out)

def decompress_sector(blob):
    if not blob: return blob
    m=blob[0]; body=blob[1:]
    # compression mask
    if m & 0x02:   return zlib.decompress(body)
    if m & 0x10:   return bz2.decompress(body)
    if m == 0:     return body  # rare: no flag byte (shouldn't happen for compressed flag)
    # if mask byte is actually data (IMPLODE etc.) fall back
    try: return zlib.decompress(body)
    except: return blob

SPELL='DBFilesClient\\Spell.dbc'

def find_block(raw, name):
    sig,hdrsz,arcsz,ver,ss,htoff,btoff,htsz,btsz=struct.unpack_from('<4sIIHHIIII',raw,0)
    if sig!=b'MPQ\x1a': return None
    ht=decrypt(raw[htoff:htoff+htsz*16], mhash('(hash table)',3))
    bt=decrypt(raw[btoff:btoff+btsz*16], mhash('(block table)',3))
    ha,hb=mhash(name,1),mhash(name,2)
    start=mhash(name,0)%htsz
    for probe in range(htsz):
        s=(start+probe)%htsz
        eha,ehb,loc,plat,bidx=struct.unpack_from('<IIHHI',ht,s*16)
        if bidx==0xFFFFFFFF:
            continue
        if eha==ha and ehb==hb:
            boff,bcsz,busz,bflags=struct.unpack_from('<IIII',bt,bidx*16)
            return dict(bidx=bidx,off=boff,csize=bcsz,usize=busz,flags=bflags,
                        ss=ss, archofs=0)
    return None

def read_file_from_mpq(path, name):
    with open(path,'rb') as f: raw=f.read()
    # MPQ may not start at offset 0 (some have a 512-byte header). Find 'MPQ\x1a'
    base=0
    if raw[:4]!=b'MPQ\x1a':
        i=raw.find(b'MPQ\x1a')
        if i<0: return None,'no MPQ magic'
        base=i; raw=raw[i:]
    blk=find_block(raw,name)
    if not blk: return None,'not in archive'
    flags=blk['flags']; off=blk['off']; csize=blk['csize']; usize=blk['usize']
    data=raw[off:off+csize]
    if flags & 0x00010000:  # encrypted - skip key calc complexity, note it
        return None, f'ENCRYPTED block flags={hex(flags)}'
    SINGLE = flags & 0x01000000
    COMPRESSED = flags & 0x00000200
    IMPLODE = flags & 0x00000100
    if SINGLE:
        if COMPRESSED or IMPLODE:
            out=decompress_sector(data)
        else:
            out=data
        return out[:usize], dict(off=off,csize=csize,usize=usize,flags=hex(flags),mode='single')
    # multi-sector
    secsize=512<<blk['ss']
    nsec=(usize+secsize-1)//secsize
    # sector offset table: nsec+1 uint32
    ntbl=nsec+1
    offs=struct.unpack_from('<%dI'%ntbl,data,0)
    out=bytearray()
    for i in range(nsec):
        s0=offs[i]; s1=offs[i+1]
        sect=data[s0:s1]
        rem=usize-len(out)
        expect=min(secsize,rem)
        if (COMPRESSED or IMPLODE) and len(sect)<expect:
            out+=decompress_sector(sect)
        else:
            out+=sect
    return bytes(out[:usize]), dict(off=off,csize=csize,usize=usize,flags=hex(flags),mode='multi',nsec=nsec)

def read_hs(dbc):
    if dbc[:4]!=b'WDBC': return 'NOT WDBC'
    magic,rc,fc,rs,ss=struct.unpack_from('<4sIIII',dbc,0)
    hdr=20
    for i in range(rc):
        o=hdr+i*rs
        if struct.unpack_from('<I',dbc,o)[0]==48952:
            f68=struct.unpack_from('<i',dbc,o+68*4)[0]
            f69=struct.unpack_from('<i',dbc,o+69*4)[0]
            f133=struct.unpack_from('<i',dbc,o+133*4)[0]
            return dict(rc=rc,fc=fc,rs=rs,f68=f68,f69=f69,iconID133=f133)
    return dict(rc=rc,fc=fc,rs=rs,note='48952 not found')

ROOT=r'C:\Users\Donald Lee Irwin Esq\Desktop\3.3.5'
mpqs=glob.glob(os.path.join(ROOT,'Data','*.MPQ'))+glob.glob(os.path.join(ROOT,'Data','enUS','*.MPQ'))
print(f"{'archive':<45} {'block(off/csz/usz/flags/mode)':<55} HolyShield[68]/[69]/icon[133]")
print('-'*150)
for m in sorted(mpqs):
    name=os.path.relpath(m,ROOT)
    try:
        dbc,info=read_file_from_mpq(m,SPELL)
    except Exception as e:
        dbc,info=None,f'ERR {e}'
    if dbc is None:
        # only print if it claims to contain or errored meaningfully
        if info!='not in archive':
            print(f"{name:<45} {str(info):<55}")
        continue
    hs=read_hs(dbc)
    print(f"{name:<45} {str(info):<55} {hs}")
