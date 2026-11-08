#!/usr/bin/env python3
"""Monado II source-only publication checks; never inspect the player's input.

These checks support a human-reviewed source allowlist. They cannot determine
provenance or legal rights for arbitrarily disguised data. No network or writes.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import stat

MAX_FILE = 8 * 1024 * 1024
MANIFEST = 'publication-manifest.json'
POLICY = 'publication-policy.json'
# [XCDE] LINUX-026: one reviewed generated illustration, not a general image grant.
ARTWORK_PATH = 'Monado_Linux/tools/launcher/assets/gaur_plain.png'
ARTWORK_SHA256 = '4a863642bba750089667e10727cf72353875a7baf80a0903911e47d897d8fd0a'
ARTWORK_BYTES = 2621747
ARTWORK_WIDTH, ARTWORK_HEIGHT = 1536, 1024
FORBIDDEN_DIRS = {
    'game_files', 'exefs', 'romfs', 'targets', 'extracted', 'generated', 'dump',
    'build', 'cache', 'caches', 'runs', 'accepted', 'save', 'saves', 'backup',
    'backups', 'play', 'crash', 'logs', 'mods', 'reports', 'captures', 'screenshots',
    'updates', 'keys', 'sdk', '.deps', '.venv', '__pycache__', 'node_modules', 'local_assets',
    '.codex', '.agents', '.aws', '.ssh',
}
FORBIDDEN_NAMES = {
    'installation.json', 'xcde.ini', 'prod.keys', 'title.keys', 'dev.keys', 'variants_seed.bin',
    'variants.bin', 'spirv.pack', 'spirv.idx', 'vk_pipelines.bin',
    'make_dist.sh', 'gaur_plain.png', 'monado-icon.png',
    'project_memory.md', 'monado_linux_handoff.md', 'game_input_ready.md',
}
FORBIDDEN_EXT = {
    '.appimage', '.squashfs',
    '.nsp', '.xci', '.nca', '.nsz', '.xcz', '.nso', '.nro', '.bfttf', '.bfotf',
    '.exe', '.dll', '.so', '.dylib', '.o', '.obj', '.a', '.lib', '.pdb', '.pyc',
    '.spv', '.spvasm', '.cso', '.dxil', '.bin', '.pack', '.idx', '.sav', '.save',
    '.journal', '.tmb', '.log', '.dmp', '.keys', '.zip', '.gz', '.7z',
    '.png', '.jpg', '.jpeg', '.bmp', '.webp', '.gif', '.ico', '.icns', '.svg',
    '.tga', '.tif', '.tiff', '.dds', '.ktx', '.ktx2',
    '.mp4', '.webm', '.wav', '.mp3', '.ogg', '.opus', '.flac', '.aac',
}
LIFTED = re.compile(rb'\bvoid\s+f_[0-9a-fA-F]{6,}\s*\(\s*Cpu\s*\*\s*'
                    rb'(?:(?:restrict|__restrict|__restrict__|const|volatile)\s+)*c\s*\)\s*\{')
KEY = re.compile(rb'(?:master_key|titlekek|key_area_key|aes_kek_generation)[a-z_0-9]*\s*=\s*[0-9a-fA-F]{32}')
SECRET = re.compile(rb'(?:gh[pousr]_[A-Za-z0-9]{30,}|github_pat_[A-Za-z0-9_]{50,}|'
                    rb'-----BEGIN (?:RSA |OPENSSH |EC |DSA |ENCRYPTED )?PRIVATE KEY-----|'
                    rb'-----BEGIN PGP ' rb'PRIVATE KEY BLOCK-----|'
                    rb'\b(?:AKIA|ASIA)[A-Z0-9]{16}\b|\bsk-(?:proj-|svcacct-)?[A-Za-z0-9_-]{32,})')
FONT_XOR = re.compile(rb'\b(?:XOR_)?KEY\s*=\s*0x[0-9a-fA-F]{8}\b')
PRIVATE_PATH = re.compile(rb'(?:/(?:home|Users)/[^/\s\"\']+/|[A-Z]:\\Users\\[^\\\s\"\']+\\)', re.I)
CONTROL = re.compile(rb'[\x00-\x08\x0b\x0c\x0e-\x1f\x7f]')
HASH = re.compile(r'[0-9a-f]{64}\Z')
WINDOWS_DEVICES = {'con', 'prn', 'aux', 'nul'} | {
    prefix + str(number) for prefix in ('com', 'lpt') for number in range(1, 10)}


def digest(data):
    return hashlib.sha256(data).hexdigest()


def path_problem(name):
    if not isinstance(name, str) or not name or '\\' in name or '\x00' in name:
        return 'invalid path'
    parts = PurePosixPath(name).parts
    if (PurePosixPath(name).is_absolute() or '..' in parts or '.' in parts or
            name != '/'.join(parts) or ':' in name):
        return 'noncanonical path'
    if any((part.endswith((' ', '.')) or any(ord(char) < 32 or char in '<>"|?*' for char in part)
            or part.split('.')[0].lower() in WINDOWS_DEVICES) for part in parts):
        return 'nonportable/unsafe path component'
    low = [part.lower() for part in parts]
    if any(part.endswith('.appdir') for part in low):
        return 'generated AppImage staging directory'
    if any(part in FORBIDDEN_DIRS or re.match(r'^save_[0-9]+(?:$|[._])', part) for part in low):
        return 'private/input/generated/runtime directory'
    reviewed_artwork = name == ARTWORK_PATH
    if '.git' in low or (low[-1] in FORBIDDEN_NAMES and not reviewed_artwork):
        return 'private/unsafe file'
    if ((PurePosixPath(name).suffix.lower() in FORBIDDEN_EXT and not reviewed_artwork)
            or re.search(r'\.so(?:\.\d+)+$', name.lower())):
        return 'binary/input/asset/archive extension'
    return None


def content_problem(name, data, licensed_media=None):
    if len(data) > MAX_FILE:
        return 'file exceeds reviewed source limit'
    if name == ARTWORK_PATH:
        if (licensed_media or {}).get(name) != ARTWORK_SHA256:
            return 'artwork missing exact reviewed licensed-media identity'
        if len(data) != ARTWORK_BYTES or digest(data) != ARTWORK_SHA256:
            return 'artwork differs from the fixed reviewed identity'
        if (data[:16] != b'\x89PNG\r\n\x1a\n\x00\x00\x00\rIHDR'
                or int.from_bytes(data[16:20], 'big') != ARTWORK_WIDTH
                or int.from_bytes(data[20:24], 'big') != ARTWORK_HEIGHT
                or data[24:29] != bytes((8, 2, 0, 0, 0))):
            return 'invalid reviewed RGB PNG artwork header/dimensions'
        return None
    if name.lower().endswith(('.ttf', '.otf')):
        expected = (licensed_media or {}).get(name)
        if not isinstance(expected, str) or digest(data) != expected:
            return 'font missing exact licensed-media identity'
        if data[:4] not in (b'\x00\x01\x00\x00', b'OTTO', b'ttcf'):
            return 'invalid licensed font signature'
        return None
    if data.startswith((b'NSO0', b'\x7fELF', b'MZ', b'!<arch>\n', b'!<thin>\n', b'XCVC', b'PK\x03\x04', b'\x1f\x8b')):
        return 'compiled/input/cache/archive magic'
    if data[:4] in (b'\x03\x02\x23\x07', b'\x07\x23\x02\x03', b'\xfe\xed\xfa\xce', b'\xfe\xed\xfa\xcf', b'\xcf\xfa\xed\xfe', b'\xca\xfe\xba\xbe'):
        return 'compiled shader/binary magic'
    if CONTROL.search(data):
        return 'unreviewed binary content'
    try:
        data.decode('utf-8', errors='strict')
    except UnicodeDecodeError:
        return 'unreviewed non-UTF-8 binary content'
    if KEY.search(data) or SECRET.search(data):
        return 'key/credential signature'
    if LIFTED.search(data):
        return 'generated game-function definition'
    if name.endswith('/runtime/hle/pl.c') and FONT_XOR.search(data):
        return 'excluded proprietary-font decoder constant'
    if PRIVATE_PATH.search(data) or PRIVATE_PATH.search(data.replace(b'\\\\', b'\\')):
        return 'private development path'
    return None


def read_regular(path):
    path = Path(path).absolute()
    if path.resolve() != path:
        raise ValueError('source path contains a symlink')
    info = path.lstat()
    if not stat.S_ISREG(info.st_mode) or info.st_size > MAX_FILE:
        raise ValueError('nonordinary or oversized source file')
    fd = os.open(path, os.O_RDONLY | getattr(os, 'O_NOFOLLOW', 0) |
                 getattr(os, 'O_NONBLOCK', 0) | getattr(os, 'O_BINARY', 0))
    with os.fdopen(fd, 'rb') as stream:
        actual = os.fstat(stream.fileno())
        def snapshot(value):
            return (value.st_dev, value.st_ino, value.st_mode, value.st_size,
                    value.st_mtime_ns, value.st_ctime_ns,
                    getattr(value, 'st_birthtime_ns', None))

        def same_object(left, right):
            # Windows pathname stat and descriptor fstat can report different
            # executable bits and ctime meanings (CPython issue157671). Compare
            # common identity here; keep full snapshots within each API below.
            def identity(value):
                return (value.st_dev, value.st_ino, stat.S_IFMT(value.st_mode),
                        value.st_size, value.st_mtime_ns)
            left_birth = getattr(left, 'st_birthtime_ns', None)
            right_birth = getattr(right, 'st_birthtime_ns', None)
            return (identity(left) == identity(right) and
                    (left_birth is None or right_birth is None or left_birth == right_birth))

        if not stat.S_ISREG(actual.st_mode) or not same_object(actual, info):
            raise ValueError('source file identity changed')
        data = stream.read(MAX_FILE + 1)
        after = os.fstat(stream.fileno())
    final_path = path.lstat()
    if (snapshot(after) != snapshot(actual) or snapshot(final_path) != snapshot(info)
            or not same_object(final_path, after)
            or len(data) != actual.st_size or len(data) > MAX_FILE or path.resolve() != path):
        raise ValueError('source file changed while reading')
    return data


def _json_document(raw):
    def unique_object(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError('duplicate JSON object key')
            result[key] = value
        return result

    def reject_constant(value):
        raise ValueError('nonfinite JSON constant')

    document = json.loads(raw, object_pairs_hook=unique_object, parse_constant=reject_constant)
    if not isinstance(document, dict):
        raise ValueError('JSON document must be an object')
    return document


def verify_tree(root):
    root = Path(root).absolute()
    if root.is_symlink() or root.resolve() != root or not root.is_dir():
        return ['source root must be an existing canonical directory']
    try:
        manifest_raw = read_regular(root / MANIFEST)
        policy_raw = read_regular(root / POLICY)
        document = _json_document(manifest_raw)
        policy = _json_document(policy_raw)
    except (OSError, ValueError, TypeError):
        return ['missing/invalid publication manifest or policy']
    if (type(document.get('schema')) is not int or document.get('schema') != 1
            or document.get('project') != 'Monado II' or not isinstance(document.get('files'), dict)):
        return ['invalid publication manifest schema/project/files']
    if document.get('platform') not in (None, 'linux', 'windows'):
        return ['invalid source-kit platform']
    licensed = policy.get('licensed_media', {})
    if type(policy.get('schema')) is not int or policy.get('schema') != 1 or not isinstance(licensed, dict):
        return ['invalid publication policy']
    violations, actual_names = [], set()
    for name, identity in licensed.items():
        if (path_problem(name)
                or not (name.lower().endswith(('.ttf', '.otf')) or name == ARTWORK_PATH)
                or not isinstance(identity, str) or not HASH.fullmatch(identity)):
            return ['invalid licensed-media policy entry']
        if name == ARTWORK_PATH and identity != ARTWORK_SHA256:
            return ['invalid fixed artwork policy identity']
    for name, expected in document['files'].items():
        if (path_problem(name) or name == MANIFEST or not isinstance(expected, dict)
                or not isinstance(expected.get('sha256'), str) or not HASH.fullmatch(expected['sha256'])
                or type(expected.get('bytes')) is not int or not 0 <= expected['bytes'] <= MAX_FILE):
            return ['invalid/unsafe reviewed manifest entry: ' + str(name)]
    folded = [name.casefold() for name in document['files']]
    if len(folded) != len(set(folded)):
        return ['manifest paths collide on a case-insensitive filesystem']
    for name, data in ((MANIFEST, manifest_raw), (POLICY, policy_raw)):
        problem = content_problem(name, data)
        if problem:
            return [problem + ': ' + name]

    def walk_error(error):
        violations.append('cannot completely enumerate publication source')

    for directory, dirs, files in os.walk(root, followlinks=False, onerror=walk_error):
        base = Path(directory)
        for name in list(dirs):
            item = base / name
            relative = item.relative_to(root).as_posix()
            try:
                mode = item.lstat().st_mode
            except OSError:
                violations.append('unreadable directory: ' + relative); dirs.remove(name); continue
            if not stat.S_ISDIR(mode):
                violations.append('symlink/special directory: ' + relative); dirs.remove(name)
            elif base == root and name == '.git':
                # Only ordinary local Git metadata is excluded. Never follow a
                # .git symlink; history has a separate publication audit.
                dirs.remove(name)
            elif path_problem(relative):
                violations.append('forbidden/unsafe directory: ' + relative); dirs.remove(name)
        for name in files:
            item = base / name
            relative = item.relative_to(root).as_posix()
            if relative == MANIFEST:
                continue
            if base == root and name == '.git':
                try:
                    if stat.S_ISREG(item.lstat().st_mode):
                        continue  # ordinary linked-worktree pointer, never read/copied
                except OSError:
                    pass
            actual_names.add(relative)
            problem = path_problem(relative)
            if problem:
                violations.append(problem + ': ' + relative); continue
            expected = document['files'].get(relative)
            if not isinstance(expected, dict):
                violations.append('file outside reviewed manifest: ' + relative); continue
            try:
                data = read_regular(item)
            except (OSError, ValueError):
                violations.append('unsafe/unreadable source: ' + relative); continue
            if (expected.get('sha256') != digest(data) or type(expected.get('bytes')) is not int or expected['bytes'] != len(data)):
                violations.append('reviewed source identity mismatch: ' + relative)
            problem = content_problem(relative, data, licensed)
            if problem:
                violations.append(problem + ': ' + relative)
    for name in document['files']:
        if path_problem(name) or name == MANIFEST:
            violations.append('unsafe manifest entry: ' + str(name))
        elif name not in actual_names:
            violations.append('missing reviewed source: ' + name)
    if POLICY not in document['files']:
        violations.append('policy must itself be bound by reviewed manifest')
    try:
        if read_regular(root / MANIFEST) != manifest_raw or read_regular(root / POLICY) != policy_raw:
            violations.append('publication manifest/policy changed during verification')
    except (OSError, ValueError):
        violations.append('publication manifest/policy became unreadable during verification')
    return violations


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    args = parser.parse_args()
    bad = verify_tree(args.source)
    for reason in bad:
        print('REJECT:', reason)
    if not bad:
        print('PASS: exact source manifest and publication guard; provenance review still required')
    return bool(bad)


if __name__ == '__main__':
    raise SystemExit(main())
