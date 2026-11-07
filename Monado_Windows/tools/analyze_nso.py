import struct, sys, re, os, collections, json

EX = sys.argv[1]
OUT = sys.argv[2]
os.makedirs(OUT, exist_ok=True)


def lz4(src, dstlen):
    d = bytearray()
    i = 0
    n = len(src)
    while i < n:
        tok = src[i]; i += 1
        lit = tok >> 4
        if lit == 15:
            while True:
                b = src[i]; i += 1
                lit += b
                if b != 255:
                    break
        d += src[i:i + lit]; i += lit
        if i >= n:
            break
        off = src[i] | (src[i + 1] << 8); i += 2
        ml = tok & 15
        if ml == 15:
            while True:
                b = src[i]; i += 1
                ml += b
                if b != 255:
                    break
        ml += 4
        for _ in range(ml):
            d.append(d[-off])
    return bytes(d[:dstlen])


def load(path):
    raw = open(path, 'rb').read()
    assert raw[:4] == b'NSO0'
    flags = struct.unpack_from('<I', raw, 0xC)[0]
    segs = []
    for k in range(3):
        fo, mo, sz = struct.unpack_from('<III', raw, 0x10 + k * 0x10)
        csz = struct.unpack_from('<I', raw, 0x60 + k * 4)[0]
        data = raw[fo:fo + csz]
        if flags & (1 << k):
            data = lz4(data, sz)
        segs.append((mo, data))
    return segs


SVC = collections.Counter()
report = {}
for name in ['rtld', 'main', 'sdk', 'subsdk0']:
    text, ro, data = [s[1] for s in load(os.path.join(EX, name))]
    open(os.path.join(OUT, name + '.text'), 'wb').write(text)
    open(os.path.join(OUT, name + '.ro'), 'wb').write(ro)
    words = struct.unpack('<%dI' % (len(text) // 4), text[:len(text) // 4 * 4])
    r = collections.Counter()
    svcs = collections.Counter()
    for w in words:
        if (w & 0xFFE0001F) == 0xD4000001:
            svcs[(w >> 5) & 0xFFFF] += 1
        op0 = (w >> 25) & 0xF
        if op0 in (0x7, 0xF, 0x5, 0xD):
            r['simd_fp'] += 1
        if (w & 0x3F000000) == 0x08000000:
            r['excl_or_acqrel'] += 1
            if (w >> 21) & 1:
                r['excl_pair'] += 1
        if (w & 0x7FE0E000) == 0x1AC04000:
            r['crc32'] += 1
        if (w & 0xFFFFFC00) in (0x4E284800, 0x4E285800, 0x4E286800, 0x4E287800):
            r['aes'] += 1
        if (w & 0xFC000000) == 0x94000000:
            r['bl'] += 1
        if (w & 0xFFFFFC1F) == 0xD61F0000:
            r['br_reg'] += 1
        if (w & 0xFFFFFC1F) == 0xD63F0000:
            r['blr'] += 1
        if (w & 0xFFC003E0) == 0xA9400000 or (w & 0xFFC00000) == 0xA9800000:
            pass
    r['words'] = len(words)
    # function prologue-ish: stp x29,x30,[sp,#-N]!  (0xA9Bx7BFD)
    r['stp_fp_lr_prologue'] = sum(1 for w in words if (w & 0xFFC07FFF) == 0xA9807BFD)
    report[name] = {'text': len(text), 'ro': len(ro), 'data': len(data), 'counts': dict(r), 'svc': {hex(k): v for k, v in sorted(svcs.items())}}
    SVC.update(svcs)

    # strings
    ro_s = re.findall(rb'[\x20-\x7e]{6,}', ro)
    open(os.path.join(OUT, name + '.strings.txt'), 'w', encoding='utf-8', errors='replace').write('\n'.join(s.decode('ascii') for s in ro_s))

report['svc_total'] = {hex(k): v for k, v in sorted(SVC.items())}
json.dump(report, open(os.path.join(OUT, 'report.json'), 'w'), indent=1)
print(json.dumps(report, indent=1))
