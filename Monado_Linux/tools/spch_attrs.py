# [XCDE] T211: survey of the vertex input attributes (name, location) of every shader program in the game's SPCH
# containers ("HCPS": monolib/shader/*.wishp and the xbc1 blocks of models / maps / effects), and a check that each
# program's vertex-shader control section is where the program's sizes say it is. Format from the public description
# in ScanMountGoat/xc3_lib (spch.rs), read only, no code copied. Measured layout (offsets from the container start):
#   SPCH u32[2..3] SlctOffset list (offset into the SLCT section, unk) and count, u32[6] SLCT section offset,
#   u32[8] xv4 (code) section offset, u32[10] control-section area ("unk section") offset.
#   SLCT: +16 program count, +20 program list offset (entries: data offset, size; relative to the SLCT),
#   +40 control cursor start (relative to the control area). Program data (Nvsd): +24 NvsdShaders (offset, count;
#   entries 5 u32: unk, vertex code size, fragment code size, vertex control size, fragment control size - the last
#   entry counts), +80 attributes (count, offset; entries: name offset, location; names inside the program data).
#   usage: python tools/spch_attrs.py <romfs dir>
import collections, os, struct, sys, zlib

CTRL_MAGIC = 0x98761234


def u32(d, o):
    return struct.unpack_from('<I', d, o)[0]


def cstr(d, o):
    e = d.find(b'\0', o)
    return d[o:e].decode('ascii', 'replace') if 0 <= o < len(d) and e >= 0 else ''


def programs(d, base):
    """yields (vertex control offset, [(name, location)]) for each program of the container at base"""
    nslct, slct_list, slct_sec, ctrl_sec = u32(d, base + 12), u32(d, base + 8), u32(d, base + 24), u32(d, base + 40)
    for i in range(nslct):
        s = base + slct_sec + u32(d, base + slct_list + 8 * i)
        if d[s:s + 4] != b'SLCT':
            continue
        pn, po, cur = u32(d, s + 16), u32(d, s + 20), u32(d, s + 40)
        for k in range(pn):
            p, psz = s + u32(d, s + po + 8 * k), u32(d, s + po + 8 * k + 4)
            so, sn = u32(d, p + 24), u32(d, p + 28)
            if not sn or so + 20 * sn > psz:
                continue
            e = p + so + 20 * (sn - 1)
            vctl, fctl = u32(d, e + 12), u32(d, e + 16)
            an, ao = u32(d, p + 80), u32(d, p + 84)
            attrs = []
            if an <= 32 and ao + 8 * an <= psz:
                for j in range(an):
                    attrs.append((cstr(d, p + u32(d, p + ao + 8 * j)), u32(d, p + ao + 8 * j + 4)))
            yield base + ctrl_sec + cur, attrs
            cur += vctl + fctl


def blocks(path):
    d = open(path, 'rb').read()
    yield d
    i = 0
    while True:
        i = d.find(b'xbc1', i)
        if i < 0:
            break
        try:
            yield zlib.decompress(d[i + 0x30: i + 0x30 + u32(d, i + 12)])
        except Exception:
            pass
        i += 4


names, nprog, ctrl_ok = collections.Counter(), 0, 0
nfiles = 0
for dp, _, fs in os.walk(sys.argv[1]):
    for f in fs:
        if not f.endswith(('.wishp', '.wimdo', '.wismt', '.wismda', '.wiefb')):
            continue
        nfiles += 1
        for d in blocks(os.path.join(dp, f)):
            i = 0
            while True:
                i = d.find(b'HCPS', i)
                if i < 0:
                    break
                try:
                    for ctl, attrs in programs(d, i):
                        nprog += 1
                        if ctl + 4 <= len(d) and u32(d, ctl) == CTRL_MAGIC:
                            ctrl_ok += 1
                        for a in attrs:
                            names[a] += 1
                except struct.error:
                    pass
                i += 4
print('%d files, %d programs, vertex control found for %d' % (nfiles, nprog, ctrl_ok))
for (n, l), c in names.most_common(70):
    print('%-16s loc %2d  x%d' % (n, l, c))
