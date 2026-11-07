"""RomFS manifest: aggregate per-directory counts/bytes plus a sha256 over sorted "path<TAB>size" lines.

Only aggregates and a hash are stored (Constitution art. I): no list of game file names is published.
"""
import hashlib
import os


def build_manifest(romfs):
    dirs, lines = {}, []
    for root, _, files in os.walk(romfs):
        for f in files:
            p = os.path.join(root, f)
            rel = os.path.relpath(p, romfs).replace(os.sep, '/')
            size = os.path.getsize(p)
            top = rel.split('/', 1)[0] if '/' in rel else '.'
            d = dirs.setdefault(top, [0, 0])
            d[0] += 1
            d[1] += size
            lines.append(f'{rel}\t{size}')
    lines.sort()
    h = hashlib.sha256('\n'.join(lines).encode('utf-8')).hexdigest()
    return {'dirs': dict(sorted(dirs.items())), 'files': len(lines), 'bytes': sum(v[1] for v in dirs.values()), 'sha256': h}


if __name__ == '__main__':
    import json
    import sys
    print(json.dumps(build_manifest(sys.argv[1]), indent=1))
