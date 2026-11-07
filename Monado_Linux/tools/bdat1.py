"""Read the game's BDAT tables (Xenoblade 1 format: names / strings XOR-scrambled per table).

  python tools/bdat1.py <file.bdat> [table substring] [max rows]
Lists the tables (name, rows, columns); with a substring, dumps the matching tables' rows.
"""
import struct
import sys


def decrypt(b, start, end, key):
    k1, k2 = ((key >> 8) & 0xFF) ^ 0xFF, (key & 0xFF) ^ 0xFF
    for i in range(start, end - 1, 2):
        b1, b2 = b[i], b[i + 1]
        b[i], b[i + 1] = b1 ^ k1, b2 ^ k2
        k1, k2 = (k1 + b1) & 0xFF, (k2 + b2) & 0xFF


def cstr(b, o):
    e = b.index(0, o)
    return b[o:e].decode('utf-8', 'replace')


VT = {1: ('<B', 1), 2: ('<H', 2), 3: ('<I', 4), 4: ('<b', 1), 5: ('<h', 2), 6: ('<i', 4), 7: ('str', 4), 8: ('<f', 4)}


def table(f, base):
    t = bytearray(f[base:])
    flags, names_off, row_size, hash_off, hash_n, rows_off, nrows, base_id, _, key, str_off, str_len, mem_off, nmem = \
        struct.unpack_from('<HHHHHHHHHHIIHH', t, 4)
    if flags & 2:
        decrypt(t, names_off, hash_off, key)
        decrypt(t, str_off, str_off + str_len, key)
    name = cstr(t, names_off)
    cols = []
    for m in range(nmem):
        mo = mem_off + m * 6
        info, _nxt, nameo = struct.unpack_from('<HHH', t, mo)
        kind, vt, off = t[info], t[info + 1], struct.unpack_from('<H', t, info + 2)[0]
        cnt = struct.unpack_from('<H', t, info + 4)[0] if kind == 2 else 1
        cols.append((cstr(t, nameo), kind, vt, off, cnt))
    rows = []
    for r in range(nrows):
        ro = rows_off + r * row_size
        row = {}
        for cname, kind, vt, off, cnt in cols:
            if kind == 3 or vt not in VT:
                continue
            fmt, sz = VT[vt]
            vals = []
            for k in range(cnt):
                if fmt == 'str':
                    so = struct.unpack_from('<I', t, ro + off + k * sz)[0]
                    vals.append(cstr(t, so) if so < len(t) else '?')
                else:
                    vals.append(struct.unpack_from(fmt, t, ro + off + k * sz)[0])
            row[cname] = vals[0] if cnt == 1 else vals
        rows.append((base_id + r, row))
    return name, cols, rows


def main():
    f = open(sys.argv[1], 'rb').read()
    n = struct.unpack_from('<I', f, 0)[0]
    offs = struct.unpack_from('<%dI' % n, f, 8)
    want = sys.argv[2] if len(sys.argv) > 2 else None
    maxr = int(sys.argv[3]) if len(sys.argv) > 3 else 1000
    for o in offs:
        try:
            name, cols, rows = table(f, o)
        except Exception as e:
            print('table at %#x: %s' % (o, e))
            continue
        if not want:
            print('%-28s %5d rows  %s' % (name, len(rows), ' '.join(c[0] for c in cols)[:150]))
        elif want.lower() in name.lower():
            print('== %s (%d rows)' % (name, len(rows)))
            for rid, row in rows[:maxr]:
                print(rid, row)


main()
