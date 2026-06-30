import struct, os
ROOT = r'C:\Users\Donald Lee Irwin Esq\Desktop\3.3.5\Data'
def s32(v): return v-0x100000000 if v>=0x80000000 else v
for t in ['patch-5.MPQ','patch-6.MPQ','patch-4.MPQ']:
    raw = open(os.path.join(ROOT, t),'rb').read()
    off = raw.find(b'WDBC', 0, 256)
    print('=====', t, 'plaintext WDBC at', off, '=====')
    if off < 0:
        print('  no plaintext WDBC near header (block is compressed)'); continue
    magic, rc, fc, rs, sb = struct.unpack_from('<4sIIII', raw, off)
    print(f'  rc={rc} fc={fc} rs={rs} strblk={sb}')
    if fc != 234 or not (40000 < rc < 60000):
        print('  not a sane Spell.dbc layout'); continue
    base = off + 20
    recs = {}
    for i in range(rc):
        bo = base + i*rs
        sid = struct.unpack_from('<I', raw, bo)[0]
        recs[sid] = bo
    def fld(sid, idx): return struct.unpack_from('<I', raw, recs[sid] + idx*4)[0]
    for sid, nm in [(48952,'HolyShield'),(1680,'Whirlwind'),(5487,'BearForm'),(53,'Backstab'),(75,'AutoShot')]:
        if sid in recs:
            print(f'  {nm} {sid}: [68]EquipClass={s32(fld(sid,68))} [69]SubMask={fld(sid,69)} [12]Stances={fld(sid,12)} [13]StancesNot={fld(sid,13)} [4]Attr=0x{fld(sid,4):08X}')
        else:
            print(f'  {nm} {sid} ABSENT')
