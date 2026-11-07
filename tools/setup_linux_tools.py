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
import subprocess
import sys
import tempfile


PACKAGES = {'pip': '25.3', 'capstone': '5.0.7', 'unicorn': '2.1.4',
            'lz4': '4.4.5', 'ziglang': '0.14.1'}
LOCK_SHA256 = 'f7ba58c8332d5956f87ec187e932771a05fe85abcc3d27cdb45afe1037ccfcc6'
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
    return port


def plan(port):
    return dict(scope=__doc__, mode='plan', status='not installed or verified', port=str(port),
                python='CPython 3.14 / Linux x86-64', packages=PACKAGES,
                lock_sha256=LOCK_SHA256,
                creates_only=['.venv', '.deps/bin/clang-cc', '.deps/bin/clang-cxx',
                              '.deps/setup-tmp', '.deps/metadata/linux-tools.json'],
                network='none in plan/check; --install explicitly uses PyPI hash-locked binary wheels',
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
        raise RuntimeError('tool setup command failed: ' + str(argv[0]) + '\n' + result.stderr[-4000:])
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


def install(port, *, runner=None):
    # checked_port rejects unknown existing wrappers before any installation.
    port = checked_port(port)
    venv = port / '.venv'
    if venv.exists():
        probe_venv(port, require_packages=False, runner=runner)
    for relative in ('.deps', '.deps/bin', '.deps/metadata', '.deps/setup-tmp'):
        (port / relative).mkdir(mode=0o700, parents=True, exist_ok=True)
    if not venv.exists():
        run([sys.executable, '-I', '-B', '-m', 'venv', venv], port, install=True, runner=runner)
        probe_venv(port, require_packages=False, runner=runner)
    python = venv / 'bin/python'
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
