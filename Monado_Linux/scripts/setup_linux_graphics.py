#!/usr/bin/env python3
"""[XCDE] LINUX-002: verify/extract pinned Ubuntu graphics headers/shared libraries locally.

No APT installation, maintainer scripts, system writes or compiler changes. This
is a development sysroot on the recorded Ubuntu host, not a distributable bundle.
Installed host libraries supply its transitive runtime dependencies. Run with
--offline after caching the exact locked .deb files; all hashes are rechecked.
"""
import argparse
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shlex
import shutil
import subprocess
import tarfile
import tempfile
import urllib.parse
import urllib.request


ROOT = Path(__file__).resolve().parent.parent
LOCK = ROOT / 'graphics-deps-linux.lock.json'


def reject_managed_symlinks(path):
    """Check only setup-owned paths; library links inside a sysroot are valid."""
    relative = path.relative_to(ROOT)
    if '..' in relative.parts:
        raise ValueError('managed dependency path escapes the project root')
    current = ROOT
    for part in ('', *relative.parts):
        current = current / part
        if current.is_symlink():  # also reject dangling links, which exists() misses
            raise RuntimeError(f'managed dependency path is a symlink: {current}')


def write_managed_text(path, text, mode=None):
    reject_managed_symlinks(path)
    # O_NOFOLLOW also refuses a final-path symlink introduced after the check.
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC | os.O_NOFOLLOW, 0o666)
    with os.fdopen(fd, 'w', encoding='utf-8') as output:
        if mode is not None:
            os.fchmod(output.fileno(), mode)
        output.write(text)


def package_path(package, cache):
    name = package['filename']
    if Path(name).name != name or not name.endswith('.deb'):
        raise ValueError('invalid locked package filename')
    return cache / name


def digest(path):
    with path.open('rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()


def fetch(package, cache, offline):
    name = package['filename']
    target = package_path(package, cache)
    reject_managed_symlinks(target)
    if target.exists():
        if target.is_symlink() or target.stat().st_size != package['size'] or digest(target) != package['sha256']:
            raise ValueError(f'cached package hash/size mismatch: {name}; preserve and investigate it')
        return target
    if offline:
        raise FileNotFoundError(f'offline package missing: {name}')
    url = urllib.parse.urlsplit(package['url'])
    if url.scheme != 'https' or url.hostname != 'archive.ubuntu.com' or not url.path.startswith('/ubuntu/pool/'):
        raise ValueError('unexpected package origin in lock')
    fd, temporary = tempfile.mkstemp(prefix=name + '.', suffix='.partial', dir=cache)
    try:
        with os.fdopen(fd, 'wb') as out, urllib.request.urlopen(package['url'], timeout=30) as response:
            shutil.copyfileobj(response, out)
        path = Path(temporary)
        if path.stat().st_size != package['size'] or digest(path) != package['sha256']:
            raise ValueError(f'download hash/size mismatch: {name}')
        reject_managed_symlinks(target)
        path.rename(target)
    finally:
        Path(temporary).unlink(missing_ok=True)
    print(f'verified {name}', flush=True)
    return target


def inspect_archive(path):
    # Review member paths before dpkg-deb extracts data; control scripts never run.
    process = subprocess.Popen(['dpkg-deb', '--fsys-tarfile', str(path)], stdout=subprocess.PIPE)
    try:
        with tarfile.open(fileobj=process.stdout, mode='r|') as archive:
            for member in archive:
                pure = PurePosixPath(member.name)
                if pure.is_absolute() or '..' in pure.parts:
                    raise ValueError(f'unsafe archive member in {path.name}')
                if not (member.isfile() or member.isdir() or member.issym() or member.islnk()):
                    raise ValueError(f'unexpected archive member type in {path.name}')
                if member.issym() or member.islnk():
                    link = PurePosixPath(member.linkname)
                    if link.is_absolute():
                        raise ValueError(f'absolute archive link in {path.name}')
                    # Resolve relative symlink targets lexically inside extraction root.
                    parts = list(pure.parent.parts) if member.issym() else []
                    for part in link.parts:
                        if part == '..':
                            if not parts:
                                raise ValueError(f'escaping archive link in {path.name}')
                            parts.pop()
                        elif part != '.':
                            parts.append(part)
        if process.wait() != 0:
            raise RuntimeError(f'cannot inspect {path.name}')
    finally:
        process.stdout.close()
        if process.poll() is None:
            process.kill()
            process.wait()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--offline', action='store_true')
    args = parser.parse_args()
    lock_bytes = LOCK.read_bytes()
    lock = json.loads(lock_bytes)
    if lock['schema'] != 1:
        raise ValueError('unsupported dependency-lock schema')
    identity = hashlib.sha256(lock_bytes).hexdigest()
    base = ROOT / '.deps' / 'graphics'
    cache = base / 'debs'
    parent = base / 'sysroots'
    target = parent / identity[:16]
    marker = target / '.monado-deps.json'
    bin_dir = ROOT / '.deps' / 'bin'
    wrapper = bin_dir / 'pkg-config-graphics'
    active = base / 'active-sysroot.txt'
    # [XCDE] LINUX-002: reject every existing managed-path link before the first
    # mkdir/download/write, including outputs used only at the end of setup.
    for path in (cache, parent, target, marker, bin_dir, wrapper, active,
                 *(package_path(package, cache) for package in lock['packages'])):
        reject_managed_symlinks(path)
    cache.mkdir(parents=True, exist_ok=True)
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        archives = list(pool.map(lambda p: fetch(p, cache, args.offline), lock['packages']))
    reject_managed_symlinks(parent)
    parent.mkdir(exist_ok=True)
    reject_managed_symlinks(marker)
    if target.exists():
        if target.is_symlink() or not marker.is_file() or json.loads(marker.read_text())['lock_sha256'] != identity:
            raise RuntimeError('existing sysroot has unknown identity; refusing to overwrite')
    else:
        with tempfile.TemporaryDirectory(prefix='extract-', dir=parent) as temporary:
            stage = Path(temporary) / 'sysroot'
            stage.mkdir()
            for package, archive in zip(lock['packages'], archives):
                inspect_archive(archive)
                subprocess.run(['dpkg-deb', '--extract', str(archive), str(stage)], check=True)
            write_managed_text(stage / '.monado-deps.json', json.dumps({'lock_sha256': identity, 'packages': lock['packages']}, indent=2) + '\n')
            reject_managed_symlinks(target)
            stage.rename(target)
    reject_managed_symlinks(bin_dir)
    bin_dir.mkdir(exist_ok=True)
    # Deliberately do not mix other project/local .pc files into this sysroot.
    write_managed_text(wrapper, '#!/usr/bin/env bash\nset -euo pipefail\n' +
                       'export PKG_CONFIG_SYSROOT_DIR=' + shlex.quote(str(target)) + '\n' +
                       'export PKG_CONFIG_LIBDIR=' + shlex.quote(str(target / 'usr/lib/x86_64-linux-gnu/pkgconfig') + ':' + str(target / 'usr/share/pkgconfig')) + '\n' +
                       # [XCDE] LINUX-028: FDO flag rooting preserves literal paths
                       # on tested pkgconf 1.8.1/2.5.1; embedded quotes differ.
                       # Keep ordinary variable-query behavior unchanged.
                       'unset PKG_CONFIG_FDO_SYSROOT_RULES\n' +
                       'for monado_pkg_arg in "$@"; do\n' +
                       '  case "$monado_pkg_arg" in\n' +
                       '    --cflags|--cflags-only-*|--libs|--libs-only-*)\n' +
                       '      export PKG_CONFIG_FDO_SYSROOT_RULES=1\n' +
                       '      break;;\n' +
                       '  esac\ndone\n' +
                       'unset PKG_CONFIG_PATH\nexec /usr/bin/pkg-config "$@"\n', mode=0o755)
    write_managed_text(active, str(target) + '\n')
    print(f'sysroot: {target}')
    print(f'pkg-config: {wrapper}')


if __name__ == '__main__':
    main()
