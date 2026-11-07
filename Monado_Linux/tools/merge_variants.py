# [XCDE] T211: merge cache/variants.bin files (exec.c T163 records: header "XCVC" u32 + record size u32, then fixed-size
# records) into one deduplicated seed, data/variants_seed.bin, that the game and the precompilation read besides the
# player's own cache/variants.bin. Records of another size (an older layout) are skipped.
#   python tools/merge_variants.py <out> <in> [<in> ...]
import struct, sys

MAGIC = 0x56435843


def records(path):
    with open(path, 'rb') as f:
        data = f.read()
    if len(data) < 8:
        return None, []
    magic, size = struct.unpack_from('<II', data, 0)
    if magic != MAGIC or size == 0:
        return None, []
    n = (len(data) - 8) // size
    return size, [data[8 + i * size: 8 + (i + 1) * size] for i in range(n)]


out, ins = sys.argv[1], sys.argv[2:]
size, seen, order = None, set(), []
for p in ins:
    s, recs = records(p)
    if s is None:
        print('skip (not a variants file):', p)
        continue
    if size is None:
        size = s
    if s != size:
        print('skip (record size %d, not %d):' % (s, size), p)
        continue
    new = 0
    for r in recs:
        if r not in seen:
            seen.add(r)
            order.append(r)
            new += 1
    print('%s: %d records, %d new' % (p, len(recs), new))
with open(out, 'wb') as f:
    f.write(struct.pack('<II', MAGIC, size))
    f.write(b''.join(order))
print('%s: %d variants (%d bytes each)' % (out, len(order), size))
