#!/usr/bin/env python3
"""[XCDE] LINUX-002/042: verify/extract pinned private graphics/runtime libraries.

No APT installation, maintainer scripts, system writes or compiler changes. This
is a project-local sysroot, not a distributable binary bundle. Its declared
non-platform runtime closure is pinned; native host interfaces are checked by
the build/runner preflight rather than copied from the build host. Run with
--offline after caching the exact locked .deb files; all hashes are rechecked.
"""
import argparse
import concurrent.futures
import errno
import hashlib
import http.client
import importlib.util
import json
import os
from pathlib import Path, PurePosixPath
import shlex
import socket
import ssl
import stat
import subprocess
import tarfile
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request


ROOT = Path(__file__).resolve().parent.parent
LOCK = ROOT / 'graphics-deps-linux.lock.json'
DOWNLOAD_ATTEMPTS = 3
DOWNLOAD_TIMEOUT = 30


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


def verified_package(path, package, output=None):
    """Read only ordinary pinned bytes; optionally copy into our new partial."""
    before = path.lstat()
    if (not stat.S_ISREG(before.st_mode) or before.st_nlink != 1
            or before.st_size != package['size']):
        raise ValueError(f'cached package type/hash/size mismatch: {path.name}; preserve and investigate it')
    flags = os.O_RDONLY | getattr(os, 'O_NOFOLLOW', 0) | getattr(os, 'O_CLOEXEC', 0)
    with os.fdopen(os.open(path, flags), 'rb') as source:
        opened = os.fstat(source.fileno())
        if (not stat.S_ISREG(opened.st_mode) or opened.st_nlink != 1
                or (opened.st_dev, opened.st_ino) != (before.st_dev, before.st_ino)):
            raise ValueError('cached package changed before reading; preserve and investigate it')
        total, identity = 0, hashlib.sha256()
        while block := source.read(min(1024 * 1024, package['size'] - total + 1)):
            total += len(block)
            if total > package['size']:
                raise ValueError('cached package exceeds locked size; preserve and investigate it')
            identity.update(block)
            if output is not None:
                output.write(block)
        finished = os.fstat(source.fileno())
    after = path.lstat()
    snapshot = lambda s: (s.st_dev, s.st_ino, s.st_size, s.st_mtime_ns, s.st_ctime_ns, s.st_nlink)
    if snapshot(before) != snapshot(after) or snapshot(opened) != snapshot(finished):
        raise ValueError('cached package changed while reading; preserve and investigate it')
    if total != package['size'] or identity.hexdigest() != package['sha256']:
        raise ValueError(f'cached package hash/size mismatch: {path.name}; preserve and investigate it')
    return path


def original_cached_package(package):
    """Only the bound original installation's raw .deb cache may be borrowed."""
    if not os.path.lexists(ROOT.parent / 'installation.json'):
        return None
    # Use the existing exact owner/workspace/game binding, not guessed nearby
    # directories, environment overrides, sysroots or previously built binaries.
    recipe = Path(__file__).resolve().with_name('install_layout.py')
    spec = importlib.util.spec_from_file_location('_graphics_install_layout', recipe)
    layout = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(layout)
    owner_port = layout.installation_root(ROOT) / 'Monado_Linux'
    cache = owner_port / '.deps/graphics/debs'
    current = owner_port
    for piece in ('.deps', 'graphics', 'debs'):
        current /= piece
        try:
            info = current.lstat()
        except FileNotFoundError:
            return None
        if not stat.S_ISDIR(info.st_mode):
            raise ValueError('original installation cache must use ordinary directories; preserve and investigate it')
    source = package_path(package, cache)
    return source if os.path.lexists(source) else None


def approved_origin(value):
    url = urllib.parse.urlsplit(value)
    if (url.scheme != 'https' or url.hostname != 'archive.ubuntu.com'
            or url.port not in (None, 443) or url.username is not None or url.password is not None
            or not url.path.startswith('/ubuntu/pool/')):
        raise ValueError('unexpected package origin in lock or response')


class ApprovedRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, response, code, message, headers, url):
        # Reject an insecure/foreign redirect before contacting its destination.
        approved_origin(url)
        return super().redirect_request(request, response, code, message, headers, url)


def open_package(url):
    return urllib.request.build_opener(ApprovedRedirect()).open(url, timeout=DOWNLOAD_TIMEOUT)


def transient_network(error):
    if isinstance(error, urllib.error.HTTPError):
        return error.code in (408, 429) or 500 <= error.code <= 599
    if isinstance(error, urllib.error.URLError):
        return transient_network(error.reason) if isinstance(error.reason, BaseException) else False
    if isinstance(error, ssl.SSLCertVerificationError):
        return False
    if isinstance(error, ssl.SSLError):
        return isinstance(error, ssl.SSLEOFError)
    if isinstance(error, socket.gaierror):
        return error.errno == socket.EAI_AGAIN
    return (isinstance(error, (TimeoutError, ConnectionError, http.client.IncompleteRead))
            or isinstance(error, OSError) and error.errno in
            (errno.ETIMEDOUT, errno.ECONNRESET, errno.ECONNABORTED, errno.ECONNREFUSED,
             errno.ENETUNREACH, errno.EHOSTUNREACH, errno.EPIPE))


class RetryDownload(Exception):
    def __init__(self, cause):
        self.cause = cause


def network_call(operation):
    # Only network operations enter this classifier. A disk-full, permission or
    # other output error must not be retried as a connection failure.
    try:
        return operation()
    except Exception as error:
        if isinstance(error, urllib.error.HTTPError):
            error.close()
        if transient_network(error):
            raise RetryDownload(error) from error
        raise


def download_package(package, output):
    response = network_call(lambda: open_package(package['url']))
    with response:
        approved_origin(response.geturl())
        length = response.headers.get('Content-Length')
        if length is not None and (not length.isdecimal() or int(length) != package['size']):
            raise ValueError(f'download Content-Length differs from locked size: {package["filename"]}')
        total, identity = 0, hashlib.sha256()
        while block := network_call(lambda: response.read(min(1024 * 1024, package['size'] - total + 1))):
            total += len(block)
            if total > package['size']:
                raise ValueError(f'download hash/size mismatch: {package["filename"]}')
            identity.update(block)
            output.write(block)
    if total < package['size']:
        raise RetryDownload(http.client.IncompleteRead(b'', package['size'] - total))
    if identity.hexdigest() != package['sha256']:
        raise ValueError(f'download hash/size mismatch: {package["filename"]}')


def publish_package(temporary, target, package):
    reject_managed_symlinks(target)
    # A hard link from our newly copied/downloaded partial gives no-overwrite
    # publication. It never links to the original installation's cache inode.
    os.link(temporary, target)
    temporary.unlink()
    return verified_package(target, package)


def fetch(package, cache, offline):
    name = package['filename']
    target = package_path(package, cache)
    reject_managed_symlinks(target)
    if target.exists():
        return verified_package(target, package)
    original = original_cached_package(package)
    if original is not None:
        fd, temporary = tempfile.mkstemp(prefix=name + '.', suffix='.partial', dir=cache)
        temporary = Path(temporary)
        try:
            with os.fdopen(fd, 'wb') as output:
                verified_package(original, package, output)
                output.flush()
                os.fsync(output.fileno())
            publish_package(temporary, target, package)
        finally:
            temporary.unlink(missing_ok=True)
        print(f'reused verified original-installation package {name}', flush=True)
        return target
    if offline:
        raise FileNotFoundError(f'offline package missing: {name}')
    approved_origin(package['url'])
    for attempt in range(1, DOWNLOAD_ATTEMPTS + 1):
        fd, temporary = tempfile.mkstemp(prefix=name + '.', suffix='.partial', dir=cache)
        temporary = Path(temporary)
        try:
            with os.fdopen(fd, 'wb') as output:
                download_package(package, output)
                output.flush()
                os.fsync(output.fileno())
            publish_package(temporary, target, package)
            break
        except RetryDownload as failure:
            if attempt == DOWNLOAD_ATTEMPTS:
                raise RuntimeError(f'package download failed for {name} after {DOWNLOAD_ATTEMPTS} attempts: {failure.cause}') from failure.cause
            print(f'transient download failure for {name} (attempt {attempt}/{DOWNLOAD_ATTEMPTS}); retrying in {attempt}s: {failure.cause}', flush=True)
        finally:
            temporary.unlink(missing_ok=True)
        time.sleep(attempt)
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


def runtime_policy_digest(policy):
    raw = json.dumps(policy, sort_keys=True, separators=(',', ':')).encode()
    return hashlib.sha256(raw).hexdigest()


def validate_private_runtime(sysroot, lock, previous=None):
    """Check the owned graph without binding it to this setup host's libraries.

    Declared platform interfaces deliberately remain unresolved here. The build
    and launch preflight validates those actual native providers and the loader
    separately. Every other missing or wrong-version dependency is a failure.
    """
    policy = lock.get('runtime')
    if policy is None:  # Existing legacy lock fixtures have no closure claim.
        return {}
    if policy.get('schema') != 1:
        raise ValueError('unsupported private runtime policy schema')
    packages = {package['package'] for package in lock['packages']}
    if (not policy.get('root_sonames') or not isinstance(policy.get('owned_sonames'), dict)
            or any(name not in policy['owned_sonames'] for name in policy['root_sonames'])
            or any(meta.get('package') not in packages for meta in policy['owned_sonames'].values())):
        raise ValueError('runtime roots/providers do not bind locked packages')
    module_path = Path(__file__).with_name('linux_runtime_graph.py')
    spec = importlib.util.spec_from_file_location('_graphics_runtime_graph', module_path)
    graph = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(graph)
    roots = [sysroot / policy['owned_sonames'][name]['path'] for name in policy['root_sonames']]
    result = graph.audit_closure(roots, sysroot, policy, {})
    errors = [item for item in result['errors'] if item['kind'] != 'host_missing']
    if errors:
        raise ValueError('private runtime dependency closure failed: ' + json.dumps(errors[:16], sort_keys=True))
    actual = {item['soname']: item for item in result['objects']}
    if set(actual) != set(policy['owned_sonames']):
        raise ValueError('private runtime policy does not describe the complete reachable owned graph')
    owned = {name: {'path': meta['path'], 'sha256': actual[name]['sha256'],
                    'bytes': actual[name]['bytes']}
             for name, meta in policy['owned_sonames'].items()}
    metadata = {'runtime_policy': policy, 'runtime_policy_sha256': runtime_policy_digest(policy),
                'owned_runtime_files': owned,
                'required_host_interfaces': sorted({edge['needed'] for edge in result['edges']
                                                     if edge['needed'] in policy['host_sonames']}),
                'runtime_scope': 'Private native DT_NEEDED/version closure verified; '
                                 'host providers and dlopen/GPU plugins require separate preflight.'}
    if previous is not None and any(previous.get(key) != value for key, value in metadata.items()):
        raise ValueError('existing private runtime metadata/files changed; preserve and investigate the sysroot')
    return metadata


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
        validate_private_runtime(target, lock, json.loads(marker.read_text()))
    else:
        with tempfile.TemporaryDirectory(prefix='extract-', dir=parent) as temporary:
            stage = Path(temporary) / 'sysroot'
            stage.mkdir()
            for package, archive in zip(lock['packages'], archives):
                inspect_archive(archive)
                subprocess.run(['dpkg-deb', '--extract', str(archive), str(stage)], check=True)
            runtime = validate_private_runtime(stage, lock)
            write_managed_text(stage / '.monado-deps.json', json.dumps(
                {'lock_sha256': identity, 'packages': lock['packages'], **runtime}, indent=2) + '\n')
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
