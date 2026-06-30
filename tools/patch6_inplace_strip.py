"""
patch6_inplace_strip.py

THE client's effective Spell.dbc lives in Data\patch-6.MPQ (root non-locale patch,
the Druid-form-suppression patch) — root patches override locale patch-enUS-N for
DBFilesClient files. patch-6 stores Spell.dbc as an UNCOMPRESSED WDBC blob right
after its header (located via the 'WDBC' magic). We edit the requirement fields
IN PLACE (integers only — no record/string changes), so the file length and the
archive's exact byte structure are preserved (client reads it identically; Druid
form strips, which are already -1/0, stay intact — the strip is idempotent).

Strips mirror the server (mod-multiclass) and patch_spell_requirements.py:
  SHIELD -> EquippedItemClass=-1, masks=0
  WEAPON -> broaden melee subclass mask to all-melee (ranged kept; weapon still required)
  STANCE -> Stances/StancesNot=0; clear NOT_SHAPESHIFTED (0x10000)

Backs up patch-6 to patch-6.MPQ.reqbak first. Revert = restore that.
After: delete Cache\WDB\enUS\*.wdb, fully restart client.
"""
import struct, shutil, os

PATCH6 = r'C:\Users\Donald Lee Irwin Esq\Desktop\3.3.5\Data\patch-6.MPQ'
BAK    = PATCH6 + '.reqbak'

F_ATTR, F_STANCES, F_STANCES_NOT = 4, 12, 13
F_EQUIP_CLASS, F_EQUIP_SUBMASK, F_EQUIP_INVMASK = 68, 69, 70
ATTR_NOT_SHAPESHIFTED = 0x10000
ITEM_CLASS_WEAPON, ITEM_CLASS_ARMOR = 2, 4
SHIELD_SUBCLASS_BIT = 1 << 6
INVTYPE_SHIELD_BIT  = 1 << 14
NEG1 = 0xFFFFFFFF
MELEE_MASK  = (1<<0)|(1<<1)|(1<<4)|(1<<5)|(1<<6)|(1<<7)|(1<<8)|(1<<10)|(1<<11)|(1<<12)|(1<<13)|(1<<14)|(1<<15)|(1<<17)|(1<<20)
RANGED_MASK = (1<<2)|(1<<3)|(1<<16)|(1<<18)|(1<<19)

def s32(v): return v - 0x100000000 if v >= 0x80000000 else v

def main():
    raw = bytearray(open(PATCH6, 'rb').read())
    wdbc = raw.find(b'WDBC', 0, 4096)
    assert wdbc != -1, "WDBC magic not found in patch-6 header region"
    magic, rc, fc, rs, sbsz = struct.unpack_from('<4sIIII', raw, wdbc)
    assert magic == b'WDBC' and fc == 234, f"unexpected DBC fc={fc}"
    rec0 = wdbc + 20
    print(f"patch-6: WDBC at offset {wdbc}, {rc} records, {fc} fields, rs={rs}, strblk={sbsz}")
    # sanity: record region must fit in the file
    assert rec0 + rc*rs + sbsz <= len(raw), "record/string region overruns file"

    def g(i, f): return struct.unpack_from('<I', raw, rec0 + i*rs + f*4)[0]
    def setf(i, f, v): struct.pack_into('<I', raw, rec0 + i*rs + f*4, v & 0xFFFFFFFF)

    shield = weapon = stance = naked = 0
    for i in range(rc):
        st, stn = g(i, F_STANCES), g(i, F_STANCES_NOT)
        if st or stn:
            setf(i, F_STANCES, 0); setf(i, F_STANCES_NOT, 0); stance += 1
        attr = g(i, F_ATTR)
        if attr & ATTR_NOT_SHAPESHIFTED:
            setf(i, F_ATTR, attr & ~ATTR_NOT_SHAPESHIFTED); naked += 1
        ec = s32(g(i, F_EQUIP_CLASS))
        sub, inv = g(i, F_EQUIP_SUBMASK), g(i, F_EQUIP_INVMASK)
        if (ec == ITEM_CLASS_ARMOR and (sub & SHIELD_SUBCLASS_BIT)) or (inv & INVTYPE_SHIELD_BIT):
            setf(i, F_EQUIP_CLASS, NEG1); setf(i, F_EQUIP_SUBMASK, 0); setf(i, F_EQUIP_INVMASK, 0)
            shield += 1
        elif ec == ITEM_CLASS_WEAPON:
            if (sub & RANGED_MASK) and not (sub & MELEE_MASK):
                pass  # ranged-only — keep
            elif sub != MELEE_MASK or inv != 0:
                setf(i, F_EQUIP_SUBMASK, MELEE_MASK); setf(i, F_EQUIP_INVMASK, 0); weapon += 1

    print(f"  stripped: shield={shield} weapon-broadened={weapon} stance={stance} not-shapeshifted-cleared={naked}")

    orig_len = os.path.getsize(PATCH6)
    if not os.path.exists(BAK):
        shutil.copy2(PATCH6, BAK); print(f"  backed up -> {os.path.basename(BAK)}")
    assert len(raw) == orig_len, "length changed! aborting"
    with open(PATCH6, 'wb') as f:
        f.write(raw)
    print(f"\nDone. patch-6.MPQ edited in place ({len(raw):,} bytes, length unchanged).")

if __name__ == '__main__':
    main()
