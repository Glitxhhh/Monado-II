#!/usr/bin/env python3
"""[XCDE] LINUX-017: verify a minimal, pinned NVIDIA Linux DLSS development SDK.

Downloads only exact locked official files into ignored .deps, retains NVIDIA's
license and writes a receipt. No vendor code, installer or build is executed.
Use --offline after caching the files. Existing content is verified, never
silently repaired or overwritten. This is not a distributable SDK bundle.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import tempfile
import urllib.error
import urllib.request


ROOT = Path(__file__).resolve().parent.parent
LOCK = ROOT / 'dlss-linux.lock.json'
ORIGIN = 'https://raw.githubusercontent.com/NVIDIA/DLSS/'
EXPECTED_FILES = {
    'LICENSE.txt',
    'include/nvsdk_ngx.h',
    'include/nvsdk_ngx_defs.h',
    'include/nvsdk_ngx_defs_vk.h',
    'include/nvsdk_ngx_params.h',
    'include/nvsdk_ngx_helpers.h',
    'include/nvsdk_ngx_helpers_vk.h',
    'include/nvsdk_ngx_vk.h',
    'lib/Linux_x86_64/libnvsdk_ngx.a',
    'lib/Linux_x86_64/rel/libnvidia-ngx-dlss.so.310.9.1',
}


def reject_symlinks(path):
    relative = path.relative_to(ROOT)
    if '..' in relative.parts:
        raise ValueError('managed dependency path escapes root')
    current = ROOT
    if current.is_symlink():
        raise RuntimeError(f'managed path is a symlink: {current}')
    for part in relative.parts:
        current = current / part
        if current.is_symlink():
            raise RuntimeError(f'managed path is a symlink: {current}')


def sha256(path):
    reject_symlinks(path)
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW)
    with os.fdopen(fd, 'rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def validate_lock(lock):
    source = lock.get('source', {})
    commit = source.get('commit', '')
    if (lock.get('schema') != 1 or source.get('repository') != 'https://github.com/NVIDIA/DLSS'
            or source.get('tag') != 'v310.9.1' or not re.fullmatch('[0-9a-f]{40}', commit)):
        raise ValueError('unsupported or invalid DLSS lock source')
    paths = []
    for entry in lock['files']:
        name = entry['path']
        path = PurePosixPath(name)
        if (path.is_absolute() or '..' in path.parts or name not in EXPECTED_FILES
                or entry['url'] != ORIGIN + commit + '/' + name
                or not isinstance(entry['size'], int) or not 0 < entry['size'] <= 200_000_000
                or not re.fullmatch('[0-9a-f]{64}', entry['sha256'])):
            raise ValueError('invalid DLSS file entry or download origin')
        paths.append(name)
    if len(paths) != len(EXPECTED_FILES) or set(paths) != EXPECTED_FILES:
        raise ValueError('minimal DLSS file list differs or contains duplicates')


def verify(path, entry):
    reject_symlinks(path)
    if not path.is_file() or path.stat().st_size != entry['size'] or sha256(path) != entry['sha256']:
        raise ValueError(f'DLSS hash/size mismatch: {path}; preserve and investigate')


class NoRedirects(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        raise urllib.error.URLError('locked DLSS download must not redirect')


def fetch(entry, path, offline):
    reject_symlinks(path)
    if path.exists():
        verify(path, entry)
        return
    if offline:
        raise FileNotFoundError(f'offline DLSS file missing: {path}')
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, name = tempfile.mkstemp(prefix=path.name + '.', suffix='.partial', dir=path.parent)
    temporary = Path(name)
    try:
        # Refuse redirects and bound the response by the exact pinned byte size.
        opener = urllib.request.build_opener(NoRedirects())
        with os.fdopen(fd, 'wb') as output, opener.open(entry['url'], timeout=30) as response:
            remaining = entry['size'] + 1
            while remaining:
                data = response.read(min(remaining, 1024 * 1024))
                if not data:
                    break
                output.write(data)
                remaining -= len(data)
        verify(temporary, entry)
        reject_symlinks(path)
        # link() is no-replace, including if a concurrent caller supplied path.
        os.link(temporary, path, follow_symlinks=False)
    finally:
        temporary.unlink(missing_ok=True)


def write_new(path, data):
    reject_symlinks(path)
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o644)
    with os.fdopen(fd, 'w', encoding='utf-8') as output:
        output.write(data)


def setup(lock_bytes, offline):
    lock = json.loads(lock_bytes)
    validate_lock(lock)
    identity = hashlib.sha256(lock_bytes).hexdigest()
    base = ROOT / '.deps/dlss'
    cache = base / 'cache' / lock['source']['commit']
    parent = base / 'sdk'
    target = parent / identity[:16]
    receipt = target / '.monado-dlss.json'
    active = base / 'active-sdk.txt'
    # Validate every setup-owned path before the first mutation, including any
    # destination that would otherwise be touched only after downloading files.
    for path in (cache, parent, target, receipt, active,
                 *(cache / entry['path'] for entry in lock['files']),
                 *(target / entry['path'] for entry in lock['files'])):
        reject_symlinks(path)
    if target.exists():
        if not receipt.is_file() or json.loads(receipt.read_text()).get('lock_sha256') != identity:
            raise RuntimeError('existing DLSS SDK has unknown identity; refusing overwrite')
        for entry in lock['files']:
            verify(target / entry['path'], entry)
    if active.exists():
        old_target = Path(active.read_text().strip())
        if old_target.parent != parent:
            raise RuntimeError('active DLSS SDK points outside the managed SDK directory')
        reject_symlinks(old_target / '.monado-dlss.json')
        if not (old_target / '.monado-dlss.json').is_file():
            raise RuntimeError('active DLSS SDK has no receipt; refusing overwrite')
    cache.mkdir(parents=True, exist_ok=True)
    for entry in lock['files']:
        fetch(entry, cache / entry['path'], offline)
    parent.mkdir(exist_ok=True)
    if not target.exists():
        with tempfile.TemporaryDirectory(prefix='stage-', dir=parent) as temporary:
            stage = Path(temporary) / 'sdk'
            stage.mkdir()
            for entry in lock['files']:
                output = stage / entry['path']
                output.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(cache / entry['path'], output)
                verify(output, entry)
            write_new(stage / '.monado-dlss.json', json.dumps({
                'lock_sha256': identity,
                'source': lock['source'],
                'license': lock['license'],
                'runtime_policy': lock['runtime_policy'],
                'files': lock['files'],
                'vendor_code_executed_by_setup': False,
            }, indent=2) + '\n')
            reject_symlinks(target)
            stage.rename(target)
    fd, temporary = tempfile.mkstemp(prefix='active-', suffix='.tmp', dir=base)
    try:
        with os.fdopen(fd, 'w', encoding='utf-8') as output:
            output.write(str(target) + '\n')
        reject_symlinks(active)
        Path(temporary).replace(active)
    finally:
        Path(temporary).unlink(missing_ok=True)
    print(f'sdk: {target}')
    print(f'license: {target / "LICENSE.txt"} (NVIDIA proprietary terms)')
    print(f'receipt: {receipt}')
    return target


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--offline', action='store_true', help='never contact the network')
    args = parser.parse_args()
    setup(LOCK.read_bytes(), args.offline)


if __name__ == '__main__':
    main()
