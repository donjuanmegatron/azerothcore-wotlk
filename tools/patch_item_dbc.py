"""
Patches patch-enUS-3.MPQ / Item.dbc to add:
  1. Sanctum pet bag entries (700200-700213)
  2. All gear-tier variant entries from sanctum_item_variants DB table
     (Enchanted + Epic versions, entries 9000000+)

Variants copy their display fields (class, subclass, material,
displayInfoID, inventoryType, sheathType) from the base item so
icons match the original item.

Run this script any time new item types have been tiered in-game
(i.e. new entries appear in sanctum_item_variants).
"""

import mpyq, struct, shutil, os, zlib, subprocess, json

SRC       = r'C:\Users\Donald Lee Irwin Esq\Desktop\3.3.5\Data\enUS\patch-enUS-3.MPQ'
BAK       = SRC + '.bak'
ITEM_PATH = 'DBFilesClient\\Item.dbc'

# ─── MPQ crypto (seed = PLAINTEXT) ──────────────────────────────

def _ct():
    t = [0] * 0x500
    s = 0x00100001
    for i in range(0x100):
        idx = i
        for _ in range(5):
            s  = (s * 125 + 3) % 0x2AAAAB
            t1 = (s & 0xFFFF) << 16
            s  = (s * 125 + 3) % 0x2AAAAB
            t[idx] = t1 | (s & 0xFFFF)
            idx += 0x100
    return t

CT = _ct()

def _kstep(k):
    return (( ((~k & 0xFFFFFFFF) << 21) & 0xFFFFFFFF ) + 0x11111111 | (k >> 11)) & 0xFFFFFFFF

def mhash(s, ht):
    s1, s2 = 0x7FED7FED, 0xEEEEEEEE
    for c in s.upper().encode('ascii'):
        s1 = (CT[(ht << 8) + c] ^ (s1 + s2)) & 0xFFFFFFFF
        s2 = (c + s1 + s2 + (s2 << 5) + 3)   & 0xFFFFFFFF
    return s1

def decrypt(data, key):
    key &= 0xFFFFFFFF; seed = 0xEEEEEEEE; out = bytearray(len(data))
    for i in range(0, len(data), 4):
        seed  = (seed + CT[0x400 + (key & 0xFF)]) & 0xFFFFFFFF
        ciph  = struct.unpack_from('<I', data, i)[0]
        plain = (ciph ^ (key + seed)) & 0xFFFFFFFF
        struct.pack_into('<I', out, i, plain)
        key   = _kstep(key)
        seed  = (plain + seed + (seed << 5) + 3) & 0xFFFFFFFF
    return bytes(out)

def encrypt(data, key):
    key &= 0xFFFFFFFF; seed = 0xEEEEEEEE; out = bytearray(len(data))
    for i in range(0, len(data), 4):
        seed  = (seed + CT[0x400 + (key & 0xFF)]) & 0xFFFFFFFF
        plain = struct.unpack_from('<I', data, i)[0]
        ciph  = (plain ^ (key + seed)) & 0xFFFFFFFF
        struct.pack_into('<I', out, i, ciph)
        key   = _kstep(key)
        seed  = (plain + seed + (seed << 5) + 3) & 0xFFFFFFFF
    return bytes(out)

# ─── Query DB for variant entries ───────────────────────────────

def query_db(sql):
    cmd = [
        'docker', 'exec', 'azerothcore-ac-database-1',
        'mysql', '-uroot', '-ppassword', 'acore_world',
        '-se', sql
    ]
    result = subprocess.run(cmd, capture_output=True, text=True)
    rows = []
    for line in result.stdout.strip().splitlines():
        if line:
            rows.append([int(x) for x in line.split('\t')])
    return rows

print("Querying DB for variant entries...")
try:
    variant_rows = query_db(
        "SELECT base_entry, enchanted_entry, epic_entry FROM sanctum_item_variants"
    )
    print(f"  Found {len(variant_rows)} base items with variants")
except Exception as e:
    print(f"  DB query failed: {e} — skipping variants")
    variant_rows = []

# ─── Extract and parse Item.dbc ─────────────────────────────────

print("Extracting Item.dbc via mpyq...")
arch     = mpyq.MPQArchive(SRC)
item_raw = arch.read_file(ITEM_PATH)
assert item_raw

magic, rec_count, field_count, rec_size, _ = struct.unpack_from('<4sIIII', item_raw, 0)
assert magic == b'WDBC' and field_count == 8

FMT  = '<8I'
HDR  = 20
records     = [struct.unpack_from(FMT, item_raw, HDR + i * rec_size) for i in range(rec_count)]
existing_id = {r[0]: i for i, r in enumerate(records)}  # entry → index

# ─── Build custom entries ────────────────────────────────────────

new_records = []

# 1. Pet bags — fixed display (Traveler's Backpack icon, class=1 container)
PET_BAGS = [(e, 1, 0, 0xFFFFFFFF, 8, 6430, 18, 0) for e in range(700200, 700214)]
for r in PET_BAGS:
    if r[0] not in existing_id:
        new_records.append(r)

# 2. Gear variants — copy all display fields from base item
variant_added = 0
missing_base  = []
for base, ench, epic in variant_rows:
    if base not in existing_id:
        missing_base.append(base)
        continue
    base_rec = records[existing_id[base]]
    # Copy all fields except entry ID; keep same class/subclass/material/displayInfoID/invType/sheath
    for variant_entry in (ench, epic):
        if variant_entry not in existing_id:
            new_rec = (variant_entry,) + base_rec[1:]
            new_records.append(new_rec)
            variant_added += 1

# 3. Custom Sanctum tabards — clone the source tabard's display record (icon+model)
TABARDS = [(700500, 22999), (700501, 15198), (700502, 15196)]
tabard_added = 0
for new_e, src_e in TABARDS:
    if new_e not in existing_id and src_e in existing_id:
        new_records.append((new_e,) + records[existing_id[src_e]][1:])
        tabard_added += 1
print(f"  Tabard entries: {tabard_added}")

if missing_base:
    print(f"  WARNING: {len(missing_base)} base entries not in item.dbc: {missing_base[:5]}...")
print(f"  Pet bag entries:    {sum(1 for r in new_records if 700200 <= r[0] <= 700213)}")
print(f"  Gear variant entries: {variant_added}")

if not new_records:
    print("Nothing new to add — item.dbc already up to date.")
    exit(0)

# Merge and sort
all_records = records + new_records
all_records.sort(key=lambda r: r[0])
print(f"  Total records: {rec_count} + {len(new_records)} = {len(all_records)}")

# ─── Rebuild DBC bytes ───────────────────────────────────────────

new_dbc  = struct.pack('<4sIIII', b'WDBC', len(all_records), 8, 32, 1)
new_dbc += b''.join(struct.pack(FMT, *r) for r in all_records)
new_dbc += b'\x00'

# ─── Surgical MPQ patch ──────────────────────────────────────────

print("Reading raw MPQ...")
with open(SRC, 'rb') as f:
    raw = bytearray(f.read())

sig, hdr_sz, arc_sz, fmtver, sec_shift, ht_off, bt_off, ht_size, bt_size = \
    struct.unpack_from('<4sIIHHIIII', raw, 0)
assert sig == b'MPQ\x1a'

ht_dec    = decrypt(raw[ht_off : ht_off + ht_size * 16], mhash('(hash table)',  3))
ha        = mhash(ITEM_PATH, 1)
hb        = mhash(ITEM_PATH, 2)
start     = mhash(ITEM_PATH, 0) % ht_size
block_idx = None
for probe in range(ht_size):
    s = (start + probe) % ht_size
    eha, ehb, loc, plat, bidx = struct.unpack_from('<IIHHI', ht_dec, s * 16)
    if bidx == 0xFFFFFFFF: break
    if eha == ha and ehb == hb:
        block_idx = bidx
        print(f"  Item.dbc: hash slot {s}, block_idx={block_idx}")
        break

assert block_idx is not None, "Item.dbc not found in hash table"

bt_dec = bytearray(decrypt(raw[bt_off : bt_off + bt_size * 16], mhash('(block table)', 3)))
boff, bcsz, busz, bflags = struct.unpack_from('<IIII', bt_dec, block_idx * 16)
print(f"  Old block: off={boff} csize={bcsz} usize={busz}")

compressed  = b'\x02' + zlib.compress(new_dbc, 9)
new_off     = len(raw)
raw        += compressed
struct.pack_into('<IIII', bt_dec, block_idx * 16, new_off, len(compressed), len(new_dbc), 0x81000200)
print(f"  New block: off={new_off} csize={len(compressed)} usize={len(new_dbc)}")

bt_enc = encrypt(bytes(bt_dec), mhash('(block table)', 3))
raw[bt_off : bt_off + bt_size * 16] = bt_enc
struct.pack_into('<I', raw, 8, len(raw))

if not os.path.exists(BAK):
    print("Backing up patch-enUS-3.MPQ...")
    shutil.copy2(SRC, BAK)

with open(SRC, 'wb') as f:
    f.write(raw)

print(f"Done. patch-enUS-3.MPQ updated ({len(raw):,} bytes). Fully restart WoW.")
