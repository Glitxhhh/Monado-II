#!/usr/bin/env python3
"""Prepare Monado II's port-local Linux compiler tools; never build or run a game.

The checked wheel baseline requires CPython 3.14 on Linux x86-64. Default mode
prints a read-only plan. --check verifies an existing environment without writes.
--install explicitly creates/updates only PORT/.venv and PORT/.deps, using the
hash-locked binary wheels. It needs access to PyPI; it makes no system install.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import stat
import subprocess
import sys
import tempfile
import urllib.parse
import urllib.request


PACKAGES = {'pip': '25.3', 'capstone': '5.0.7', 'unicorn': '2.1.4',
            'lz4': '4.4.5', 'ziglang': '0.14.1'}
LOCK_SHA256 = 'f7ba58c8332d5956f87ec187e932771a05fe85abcc3d27cdb45afe1037ccfcc6'
# Exact official PyPI25.3 metadata; SHA also appears in requirements-linux.lock.
PIP_WHEEL = {'filename': 'pip-25.3-py3-none-any.whl', 'bytes': 1778622,
             'sha256': '9655943313a94722b7774661c21049070f6bbb0a1516bf02f7c8d5d9201514cd',
             'url': 'https://files.pythonhosted.org/packages/44/3c/d717024885424591d5376220b5e836c2d5293ce2011523c9de23ff7bf068/pip-25.3-py3-none-any.whl'}
BOOTSTRAP_PIP = '''import sys
sys.path.insert(0, sys.argv[1])
from pip._internal.cli.main import main
raise SystemExit(main(['--isolated', 'install', '--no-index', '--no-deps', '--no-cache-dir',
    '--no-input', '--disable-pip-version-check', '--ignore-installed', sys.argv[1]]))
'''
WRAPPER = '''#!/usr/bin/env bash
# [XCDE] LINUX-002: isolated Zig/Clang driver for the Linux host ABI.
set -euo pipefail
task_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)
export ZIG_GLOBAL_CACHE_DIR="$task_root/.deps/cache/zig-global"
export ZIG_LOCAL_CACHE_DIR="$task_root/.deps/cache/zig-local"
exec "$task_root/.venv/bin/python" -B -m ziglang @DRIVER@ -target x86_64-linux-gnu "$@"
'''
PROBE = '''import importlib.metadata,json,platform,sys
names=('pip','capstone','unicorn','lz4','ziglang')
versions={}
for name in names:
    try: versions[name]=importlib.metadata.version(name)
    except importlib.metadata.PackageNotFoundError: versions[name]=None
print(json.dumps(dict(implementation=platform.python_implementation(),version=list(sys.version_info[:3]),
    platform=sys.platform,machine=platform.machine(),prefix=sys.prefix,base_prefix=sys.base_prefix,packages=versions)))
'''


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def host_info():
    return dict(implementation=platform.python_implementation(), version=list(sys.version_info[:3]),
                platform=sys.platform, machine=platform.machine())


def require_host(info=None):
    info = info or host_info()
    if (info['implementation'] != 'CPython' or info['version'][:2] != [3, 14] or
            info['platform'] != 'linux' or info['machine'] != 'x86_64'):
        raise ValueError('the pinned wheel baseline requires CPython 3.14 on Linux x86-64; run python3.14')


def wrapper_text(driver):
    return WRAPPER.replace('@DRIVER@', driver)


def checked_port(value):
    port = Path(value).absolute()
    if port != port.resolve() or not port.is_dir():
        raise ValueError('--port must name an existing canonical ordinary source directory')
    lock = port / 'requirements-linux.lock'
    if lock.is_symlink() or not lock.is_file() or digest(lock) != LOCK_SHA256:
        raise ValueError('requirements-linux.lock differs from the reviewed CPython 3.14 wheel baseline')
    for relative in ('.venv', '.deps', '.deps/bin', '.deps/metadata', '.deps/setup-tmp',
                     '.deps/cache', '.deps/cache/zig-local', '.deps/cache/zig-global'):
        path = port / relative
        if path.is_symlink() or (path.exists() and not path.is_dir()):
            raise ValueError('local setup directory must be ordinary: ' + relative)
    for name, driver in (('clang-cc', 'cc'), ('clang-cxx', 'c++')):
        path = port / '.deps/bin' / name
        if path.is_symlink() or (path.exists() and
                (not path.is_file() or path.read_text() != wrapper_text(driver))):
            raise ValueError('refusing to replace a different existing compiler wrapper: ' + name)
    receipt = port / '.deps/metadata/linux-tools.json'
    if receipt.is_symlink() or (receipt.exists() and not receipt.is_file()):
        raise ValueError('local setup receipt must be an ordinary file')
    cached = port / '.deps/setup-tmp' / PIP_WHEEL['filename']
    if cached.is_symlink() or (cached.exists() and
            (not stat.S_ISREG(cached.stat().st_mode) or cached.stat().st_nlink != 1)):
        raise ValueError('cached pip bootstrap wheel must be an ordinary single-link file')
    return port


def plan(port):
    return dict(scope=__doc__, mode='plan', status='not installed or verified', port=str(port),
                python='CPython 3.14 / Linux x86-64', packages=PACKAGES,
                lock_sha256=LOCK_SHA256,
                creates_only=['.venv', '.deps/bin/clang-cc', '.deps/bin/clang-cxx',
                              '.deps/setup-tmp', '.deps/metadata/linux-tools.json'],
                pip_bootstrap=dict(PIP_WHEEL, cache='.deps/setup-tmp/' + PIP_WHEEL['filename'],
                                   mode='venv --without-pip; bootstrap only when pip is absent'),
                network='none in plan/check; --install uses exact official pip wheel if needed, then PyPI hash-locked binary wheels',
                compiler_target='x86_64-linux-gnu',
                compiler_caches=['.deps/cache/zig-local', '.deps/cache/zig-global'])


def run(argv, port, *, install=False, runner=None):
    environment = {'PATH': '/usr/bin:/bin', 'LANG': 'C.UTF-8', 'LC_ALL': 'C.UTF-8',
                   'PYTHONNOUSERSITE': '1', 'PYTHONDONTWRITEBYTECODE': '1'}
    if install:
        environment['TMPDIR'] = str(port / '.deps/setup-tmp')
    operation = runner or subprocess.run
    result = operation(list(map(str, argv)), cwd=str(port), env=environment,
                       capture_output=True, text=True, timeout=600 if install else 60)
    if result.returncode:
        raise RuntimeError('tool setup command failed: ' + str(argv[0]) +
                           '\nstdout:\n' + (result.stdout or '')[-4000:] +
                           '\nstderr:\n' + (result.stderr or '')[-4000:])
    return result.stdout


def probe_venv(port, *, require_packages=True, runner=None):
    venv, python = port / '.venv', port / '.venv/bin/python'
    if not venv.is_dir() or venv.is_symlink() or not (venv / 'pyvenv.cfg').is_file() or not python.is_file():
        raise ValueError('port-local .venv is missing/incomplete; use --install to create it')
    info = json.loads(run([python, '-I', '-B', '-c', PROBE], port, runner=runner))
    require_host(info)
    if Path(info['prefix']).resolve() != venv or info['prefix'] == info['base_prefix']:
        raise ValueError('Python executable is not using this port-local virtual environment')
    if require_packages:
        if info['packages'] != PACKAGES:
            raise ValueError('port-local package versions differ from the pinned wheel baseline; use --install')
        zig = run([python, '-I', '-B', '-m', 'ziglang', 'version'], port, runner=runner).strip()
        if zig != PACKAGES['ziglang']:
            raise ValueError('Zig executable version differs from its pinned package')
    return info


def check(port, *, runner=None):
    info = probe_venv(port, runner=runner)
    wrappers = {}
    for name, driver in (('clang-cc', 'cc'), ('clang-cxx', 'c++')):
        path = port / '.deps/bin' / name
        if (path.is_symlink() or not path.is_file() or path.read_text() != wrapper_text(driver) or
                not os.access(path, os.X_OK)):
            raise ValueError('missing or altered GNU-target compiler wrapper: ' + name + '; use --install')
        wrappers[name] = digest(path)
    return dict(mode='check', status='passed', port=str(port), host=info,
                lock_sha256=LOCK_SHA256, compiler_target='x86_64-linux-gnu', wrappers=wrappers,
                scope='read-only environment/version/wrapper verification; no compile, game or network')


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, fp, code, message, headers, newurl):
        raise ValueError('pip bootstrap URL must not redirect')


def verified_pip_cache(path):
    """Read only an unchanged ordinary cache file, with Linux nofollow opens."""
    before = path.lstat()
    if not stat.S_ISREG(before.st_mode) or before.st_nlink != 1:
        raise ValueError('cached pip bootstrap wheel must be an ordinary single-link file')
    flags = os.O_RDONLY | getattr(os, 'O_NOFOLLOW', 0) | getattr(os, 'O_CLOEXEC', 0) | getattr(os, 'O_BINARY', 0)
    descriptor = os.open(path, flags)
    with os.fdopen(descriptor, 'rb') as stream:
        opened = os.fstat(stream.fileno())
        if not stat.S_ISREG(opened.st_mode) or opened.st_nlink != 1:
            raise ValueError('cached pip bootstrap descriptor must be an ordinary single-link file')
        data = stream.read(PIP_WHEEL['bytes'] + 1)
        finished = os.fstat(stream.fileno())
    after = path.lstat()
    snapshot = lambda info: (info.st_dev, info.st_ino, info.st_mode, info.st_nlink,
                             info.st_size, info.st_mtime_ns, info.st_ctime_ns)
    identity = lambda info: (info.st_dev, info.st_ino, stat.S_IFMT(info.st_mode), info.st_size)
    if snapshot(before) != snapshot(after) or snapshot(opened) != snapshot(finished) or identity(before) != identity(opened):
        raise ValueError('cached pip bootstrap wheel changed while reading')
    if len(data) != PIP_WHEEL['bytes'] or hashlib.sha256(data).hexdigest() != PIP_WHEEL['sha256']:
        raise ValueError('cached pip bootstrap wheel hash/size mismatch; preserve and inspect it')
    return path


def pip_wheel(port, *, opener=None):
    """Fetch bounded exact bytes; never execute an unchecked download or a link."""
    checked_port(port)
    target = port / '.deps/setup-tmp' / PIP_WHEEL['filename']
    if target.exists():
        return verified_pip_cache(target)
    url = urllib.parse.urlsplit(PIP_WHEEL['url'])
    if (url.scheme != 'https' or url.hostname != 'files.pythonhosted.org' or url.port is not None
            or url.username is not None or url.password is not None or url.query or url.fragment
            or not url.path.startswith('/packages/') or url.path.rsplit('/', 1)[-1] != PIP_WHEEL['filename']):
        raise ValueError('unexpected pip bootstrap origin')
    open_url = opener or urllib.request.build_opener(NoRedirect()).open
    request = urllib.request.Request(PIP_WHEEL['url'], headers={'User-Agent': 'Monado-II-local-tools'})
    with open_url(request, timeout=30) as response:
        if response.geturl() != PIP_WHEEL['url']:
            raise ValueError('pip bootstrap response changed URL')
        data = response.read(PIP_WHEEL['bytes'] + 1)
    if len(data) != PIP_WHEEL['bytes'] or hashlib.sha256(data).hexdigest() != PIP_WHEEL['sha256']:
        raise ValueError('downloaded pip bootstrap wheel hash/size mismatch')
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=target.parent, prefix='.pip-bootstrap-', suffix='.partial', delete=False) as stream:
            temporary = Path(stream.name)
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        checked_port(port)
        if target.exists():
            raise ValueError('pip bootstrap cache appeared during download; preserve and inspect it')
        # Creating a hard link refuses an existing destination atomically. The
        # private temporary is then removed before the cache is consumed.
        os.link(temporary, target)
        temporary.unlink()
        verified_pip_cache(target)
    finally:
        if temporary is not None and temporary.exists():
            temporary.unlink()
    return target


def install(port, *, runner=None):
    # checked_port rejects unknown existing wrappers before any installation.
    port = checked_port(port)
    venv = port / '.venv'
    info = None
    if venv.exists():
        info = probe_venv(port, require_packages=False, runner=runner)
    for relative in ('.deps', '.deps/bin', '.deps/metadata', '.deps/setup-tmp'):
        (port / relative).mkdir(mode=0o700, parents=True, exist_ok=True)
    if not venv.exists():
        run([sys.executable, '-I', '-B', '-m', 'venv', '--without-pip', venv], port, install=True, runner=runner)
        info = probe_venv(port, require_packages=False, runner=runner)
    python = venv / 'bin/python'
    if info['packages'].get('pip') is None:
        wheel = pip_wheel(port)
        run([python, '-I', '-B', '-c', BOOTSTRAP_PIP, wheel], port, install=True, runner=runner)
        info = probe_venv(port, require_packages=False, runner=runner)
        if info['packages'].get('pip') != PACKAGES['pip']:
            raise ValueError('verified pip bootstrap did not install pip into the port-local virtual environment')
    run([python, '-I', '-B', '-m', 'pip', '--isolated', 'install', '--require-hashes',
         '--only-binary=:all:', '--no-deps', '--no-cache-dir', '--no-input', '--disable-pip-version-check',
         '--index-url', 'https://pypi.org/simple', '-r', port / 'requirements-linux.lock'],
        port, install=True, runner=runner)
    probe_venv(port, runner=runner)
    for name, driver in (('clang-cc', 'cc'), ('clang-cxx', 'c++')):
        path = port / '.deps/bin' / name
        if not path.exists():
            with path.open('x') as stream:
                stream.write(wrapper_text(driver))
        path.chmod(path.stat().st_mode | 0o111)
    result = check(port, runner=runner)
    result.update(mode='install', installed_utc=datetime.now(timezone.utc).isoformat())
    destination = port / '.deps/metadata/linux-tools.json'
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(mode='w', dir=destination.parent,
                                         prefix='.linux-tools-', delete=False) as stream:
            temporary = Path(stream.name)
            stream.write(json.dumps(result, indent=2) + '\n')
            stream.flush()
            os.fsync(stream.fileno())
        temporary.replace(destination)
    finally:
        if temporary is not None and temporary.exists():
            temporary.unlink()
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', type=Path, default=Path(__file__).resolve().parents[1] / 'Monado_Linux')
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--check', action='store_true', help='read-only installed version and wrapper verification')
    mode.add_argument('--install', action='store_true', help='explicit port-local venv/wrapper installation using PyPI')
    args = parser.parse_args(argv)
    try:
        require_host()
        port = checked_port(args.port)
        result = install(port) if args.install else check(port) if args.check else plan(port)
    except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as error:
        parser.exit(1, str(error) + '\n')
    print(json.dumps(result, indent=2))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
