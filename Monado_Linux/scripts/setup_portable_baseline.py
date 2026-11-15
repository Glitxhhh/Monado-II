#!/usr/bin/env python3
"""Pinned Ubuntu 24.04 development sysroot; extraction only, never host install.

Package identities were resolved from the GPG-verified noble InRelease/SHA256
indexes. Maintainer scripts/services are never invoked. Source builds use an
explicit Zig GNU 2.39 target; they do not silently reuse the host C library.
"""
import argparse
import concurrent.futures
import hashlib
import json
import os
import stat
from pathlib import Path, PurePosixPath
import subprocess
import tarfile
import tempfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
LOCK = ROOT / 'portable-baseline-linux.lock.json'


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def ordinary(path, directory=False):
    p = Path(path).absolute()
    if p != p.resolve() or (p.exists() and (not p.is_dir() if directory else not p.is_file())):
        raise ValueError('portable dependency path must be ordinary: ' + str(p))
    return p


def fetch(row, cache):
    if Path(row['filename']).name != row['filename'] or not row['url'].startswith((
            'https://archive.ubuntu.com/ubuntu/', 'https://github.com/libsdl-org/SDL/releases/download/', 'https://ffmpeg.org/releases/')):
        raise ValueError('unapproved portable dependency origin/name')
    target = ordinary(cache / row['filename'])
    size = row.get('size', row.get('bytes'))
    if not target.exists():
        partial = target.with_suffix(target.suffix + '.partial')
        fd = os.open(partial, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
        try:
            with os.fdopen(fd, 'wb') as output, urllib.request.urlopen(row['url'], timeout=60) as source:
                total = 0
                while block := source.read(1024 * 1024):
                    total += len(block)
                    if total > size:
                        raise ValueError('portable dependency exceeds pinned size')
                    output.write(block)
            if partial.stat().st_size != size or sha(partial) != row['sha256']:
                raise ValueError('portable dependency hash mismatch: ' + row['filename'])
            partial.rename(target)
        except BaseException:
            partial.unlink(missing_ok=True)
            raise
    if target.stat().st_size != size or sha(target) != row['sha256']:
        raise ValueError('cached portable dependency identity differs: ' + target.name)
    return target


def archive_filter(member, destination):
    name = PurePosixPath(member.name)
    if name.is_absolute() or '..' in name.parts or not (member.isfile() or member.isdir() or member.issym() or member.islnk()):
        raise ValueError('unsafe portable archive member: ' + member.name)
    if member.issym() or member.islnk():
        target = PurePosixPath(member.linkname)
        if target.is_absolute():
            relative = target.relative_to('/')
            link = os.path.relpath(str(relative), str(name.parent)) if member.issym() else str(relative)
            member = member.replace(linkname=link)
    return tarfile.data_filter(member, destination)


SOURCE_MANIFEST = '.monado-source-manifest.json'


def source_inventory(root):
    root = ordinary(root, directory=True)
    files = {}
    for path in sorted(root.rglob('*')):
        name = path.relative_to(root).as_posix()
        if name == SOURCE_MANIFEST:
            continue
        info = path.lstat()
        if stat.S_ISLNK(info.st_mode):
            target = os.readlink(path)
            if Path(target).is_absolute() or not path.resolve().is_relative_to(root):
                raise ValueError('portable source link escaped its verified tree: ' + name)
            files[name] = {'kind': 'link', 'target': target}
        elif stat.S_ISDIR(info.st_mode):
            files[name] = {'kind': 'directory'}
        elif stat.S_ISREG(info.st_mode):
            files[name] = {'kind': 'file', 'bytes': info.st_size, 'mode': stat.S_IMODE(info.st_mode), 'sha256': sha(path)}
        else:
            raise ValueError('nonordinary portable source member: ' + name)
    return files


def archive_source_inventory(archives):
    expected = {}
    for archive in archives:
        with tarfile.open(archive) as tar:
            for original in tar:
                member = archive_filter(original, '/tmp/monado-source-inventory')
                name = PurePosixPath(member.name).as_posix()
                for parent in PurePosixPath(name).parents:
                    if str(parent) != '.': expected.setdefault(str(parent), {'kind': 'directory'})
                if member.isdir(): row = {'kind': 'directory'}
                elif member.issym(): row = {'kind': 'link', 'target': member.linkname}
                else:
                    with tar.extractfile(original) as stream:
                        content_sha = hashlib.file_digest(stream, 'sha256').hexdigest()
                    size = original.size if original.isfile() else tar.getmember(original.linkname).size
                    row = {'kind': 'file', 'bytes': size, 'mode': member.mode & 0o777, 'sha256': content_sha}
                if name in expected and expected[name] != row:
                    raise ValueError('conflicting pinned source archive member: ' + name)
                expected[name] = row
    return expected


def verify_sources(root, lock_sha256, archives, expected):
    marker = ordinary(Path(root) / SOURCE_MANIFEST)
    saved = json.loads(marker.read_text())
    if (saved.get('schema') != 1 or saved.get('lock_sha256') != lock_sha256 or saved.get('archives') != archives
            or saved.get('files') != expected or expected != source_inventory(root)):
        raise ValueError('portable source cache is incomplete or changed; preserved for inspection: ' + str(root))
    return sha(marker)


def prepare_sources(base, lock, archives, identity):
    parent = ordinary(base / 'sources', directory=True); parent.mkdir(exist_ok=True)
    # Legacy unreceipted roots remain untouched. New roots publish atomically,
    # and a partial/tampered verified root is rejected rather than repaired.
    target = ordinary(parent / ('verified-' + identity[:16]), directory=True)
    archive_ids = {row['name']: row['sha256'] for row in lock['sources']}
    expected = archive_source_inventory(archives)
    if not target.exists():
        with tempfile.TemporaryDirectory(prefix='.source-extract-', dir=parent) as directory:
            stage = Path(directory) / 'source'; stage.mkdir()
            for row, archive in zip(lock['sources'], archives):
                name = 'SDL3-3.4.8' if row['name'] == 'SDL3' else 'ffmpeg-8.0.1'
                before = set(stage.iterdir())
                with tarfile.open(archive) as tar:
                    tar.extractall(stage, filter=archive_filter)
                if set(stage.iterdir()) - before != {stage / name} or not (stage / name).is_dir():
                    raise ValueError('portable source archive root differs: ' + name)
            marker = {'schema': 1, 'lock_sha256': identity, 'archives': archive_ids, 'files': expected}
            (stage / SOURCE_MANIFEST).write_text(json.dumps(marker, sort_keys=True) + '\n')
            verify_sources(stage, identity, archive_ids, expected)
            stage.rename(target)
    manifest_sha = verify_sources(target, identity, archive_ids, expected)
    return target, manifest_sha


def prepare(*, offline=False):
    lock = json.loads(LOCK.read_text())
    base = ordinary(ROOT / '.deps/portable', directory=True)
    cache = ordinary(base / 'cache', directory=True)
    cache.mkdir(parents=True, exist_ok=True)
    rows = lock['packages'] + lock['sources']
    if offline and any(not (cache / r['filename']).is_file() for r in rows):
        raise ValueError('portable cache incomplete; explicit network preparation required')
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        files = list(pool.map(lambda row: fetch(row, cache), rows))
    identity = sha(LOCK)
    parent = ordinary(base / 'sysroots', directory=True)
    parent.mkdir(exist_ok=True)
    target = ordinary(parent / identity[:16], directory=True)
    if not target.exists():
        with tempfile.TemporaryDirectory(prefix='extract-', dir=parent) as directory:
            stage = Path(directory) / 'root'
            stage.mkdir()
            for row, archive in zip(lock['packages'], files):
                process = subprocess.Popen(['/usr/bin/dpkg-deb', '--fsys-tarfile', archive], stdout=subprocess.PIPE)
                try:
                    with tarfile.open(fileobj=process.stdout, mode='r|') as tar:
                        tar.extractall(stage, filter=archive_filter)
                    if process.wait() != 0:
                        raise ValueError('portable package extraction failed: ' + row['package'])
                finally:
                    process.stdout.close()
                    if process.poll() is None:
                        process.kill(); process.wait()
            (stage / '.monado-portable.json').write_text(json.dumps({'schema': 1, 'lock_sha256': identity, 'glibc_target': '2.39'}) + '\n')
            stage.rename(target)
    marker = target / '.monado-portable.json'
    if marker.is_symlink() or json.loads(marker.read_text()).get('lock_sha256') != identity:
        raise ValueError('portable sysroot marker differs')
    sources, source_manifest_sha = prepare_sources(base, lock, files[len(lock['packages']):], identity)
    return {'sysroot': str(target), 'sources': str(sources), 'lock_sha256': identity,
            'source_manifest_sha256': source_manifest_sha, 'scope': 'verified development headers/tools/source; runtime floor qualified separately'}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--prepare', action='store_true')
    ap.add_argument('--offline', action='store_true')
    args = ap.parse_args()
    if args.prepare:
        print(json.dumps(prepare(offline=args.offline), indent=2))
    else:
        print(LOCK.read_text())


if __name__ == '__main__':
    main()
