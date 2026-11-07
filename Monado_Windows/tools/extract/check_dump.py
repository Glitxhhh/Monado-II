"""Validate a user-supplied dump against a game profile (FR-001..004, FR-006).

Accepts a folder holding exefs/ and romfs/ (or --exefs/--romfs separately). Never reads encrypted containers
(.nsp/.xci/.nca/.nsz/.xcz) and never opens key files (Constitution art. II).

Exit codes: 0 ok, 1 usage, 2 container, 3 exefs, 4 version, 5 romfs.
"""
import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from manifest import build_manifest  # noqa: E402

ERR_OK, ERR_USAGE, ERR_CONTAINER, ERR_EXEFS, ERR_VERSION, ERR_ROMFS = 0, 1, 2, 3, 4, 5
CONTAINER_EXT = ('.nsp', '.xci', '.nca', '.nsz', '.xcz')


class Result:
    def __init__(self):
        self.ok = True
        self.code = ERR_OK
        self.messages = []

    def fail(self, code, *msgs):
        self.ok = False
        self.code = code
        self.messages.extend(msgs)
        return self


def _containers_in(path):
    try:
        return [n for n in os.listdir(path) if n.lower().endswith(CONTAINER_EXT)]
    except OSError:
        return []


def _read_build_id(path):
    """First 8 bytes of the NSO build id (offset 0x40), or None if the file is not an NSO."""
    try:
        with open(path, 'rb') as f:
            head = f.read(0x48)
    except OSError:
        return None
    if len(head) < 0x48 or head[:4] != b'NSO0':
        return None
    return head[0x40:0x48].hex()


def check_dump(path, profile, exefs=None, romfs=None):
    r = Result()
    if path is not None:
        low = path.lower()
        if os.path.isfile(path):
            if low.endswith(CONTAINER_EXT):
                return r.fail(ERR_CONTAINER,
                              f'"{os.path.basename(path)}" is a container ({os.path.splitext(path)[1]}): '
                              'the extracted content is needed (exefs and romfs folders). The file was not read.')
            return r.fail(ERR_USAGE, 'Expected a folder with exefs/ and romfs/.')
        if not os.path.isdir(path):
            return r.fail(ERR_USAGE, f'No such folder: {path}')
        exefs = exefs or (os.path.join(path, 'exefs') if os.path.isdir(os.path.join(path, 'exefs')) else None)
        romfs = romfs or (os.path.join(path, 'romfs') if os.path.isdir(os.path.join(path, 'romfs')) else None)
        if exefs is None and os.path.isfile(os.path.join(path, 'main')):
            exefs = path  # the folder itself is the exefs
        if exefs is None and romfs is None and _containers_in(path):
            return r.fail(ERR_CONTAINER,
                          'The folder only holds encrypted containers (' + ', '.join(_containers_in(path)) + '). '
                          'The extracted content is needed (exefs and romfs).')
    if not exefs or not os.path.isdir(exefs):
        return r.fail(ERR_EXEFS, 'The exefs folder is missing.')

    # exefs: required files, NSO magic
    missing = [n for n in profile['exefs_required'] if not os.path.isfile(os.path.join(exefs, n))]
    if missing:
        return r.fail(ERR_EXEFS, 'Files missing in exefs: ' + ', '.join(missing))
    ids = {}
    for m in profile['modules']:
        bid = _read_build_id(os.path.join(exefs, m['name']))
        if bid is None:
            return r.fail(ERR_EXEFS, f'exefs/{m["name"]} is not a valid NSO.')
        ids[m['name']] = bid

    # version by build id
    bad = [(m['name'], ids[m['name']], m['build_id'][:16]) for m in profile['modules']
           if ids[m['name']] != m['build_id'][:16]]
    if bad:
        lines = [f'Unsupported version: {profile["version"]} is expected.']
        lines += [f'  {n}: build-id found {f}, expected {e}' for n, f, e in bad]
        return r.fail(ERR_VERSION, *lines)

    # romfs: aggregate manifest
    if not romfs or not os.path.isdir(romfs):
        return r.fail(ERR_ROMFS, 'The romfs folder is missing.')
    exp = profile['romfs_manifest']
    got = build_manifest(romfs)
    if got['sha256'] == exp['sha256']:
        r.messages.append(f'OK: version {profile["version"]}, {got["files"]} files in romfs.')
        return r
    msgs = ['The romfs does not match the expected version.']
    for name, (n, b) in exp['dirs'].items():
        g = got['dirs'].get(name)
        if g is None:
            msgs.append(f"  folder '{name}' is missing ({n} files)")
        elif g[0] != n:
            msgs.append(f"  '{name}': {g[0]} files, {n} expected ({n - g[0]} missing)" if g[0] < n
                        else f"  '{name}': {g[0]} files, {n} expected ({g[0] - n} extra)")
        elif g[1] != b:
            msgs.append(f"  '{name}': the same number of files but {g[1]} bytes instead of {b} (truncated or modified?)")
    for name in got['dirs']:
        if name not in exp['dirs']:
            msgs.append(f"  unexpected folder '{name}'")
    if len(msgs) == 1:
        msgs.append('  the same counts and sizes per folder, but different file names.')
    return r.fail(ERR_ROMFS, *msgs)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('dump', nargs='?')
    ap.add_argument('--exefs')
    ap.add_argument('--romfs')
    ap.add_argument('--profile', default=os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..',
                                                       'games', 'xcde', 'profile.json'))
    a = ap.parse_args(argv)
    with open(a.profile, encoding='utf-8') as f:
        profile = json.load(f)
    res = check_dump(a.dump, profile, exefs=a.exefs, romfs=a.romfs)
    for m in res.messages:
        print(m)
    return res.code


if __name__ == '__main__':
    sys.exit(main())
