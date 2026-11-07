#!/usr/bin/env python3
"""[XCDE] LINUX-004: bounded synthetic input/state isolation tests; no game files."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import resource
import signal
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parent.parent
SOURCES = ('runtime/paths.c', 'runtime/paths.h', 'runtime/platform.h', 'runtime/gamelib.h',
           'tests/native/test_paths_linux.c', 'scripts/path_tests_linux.py', 'graphics-deps-linux.lock.json')


def sha(path):
    with path.open('rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, help='new directory below Monado_Linux/runs')
    parser.add_argument('--gcc', action='store_true', help='use installed GCC instead of pinned Zig/Clang')
    args = parser.parse_args()
    if sys.platform != 'linux' or platform.machine() != 'x86_64':
        parser.error('native Linux x86-64 required')
    os.umask(0o077)
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    runs = ROOT / 'runs'
    if runs.is_symlink():
        parser.error('runs/ must not be a symlink')
    runs.mkdir(exist_ok=True)
    if args.output:
        out = args.output.absolute()
        if out.resolve() != out or not out.is_relative_to(runs) or out.exists():
            parser.error('--output must be a new non-symlink directory under Monado_Linux/runs')
        out.mkdir(parents=True, exist_ok=False)
    else:
        out = Path(tempfile.mkdtemp(prefix='linux-paths.', dir=runs))
    (out / 'cases').mkdir()
    (out / 'tmp').mkdir()
    sysroot = Path((ROOT / '.deps/graphics/active-sysroot.txt').read_text().strip())
    lib = sysroot / 'usr/lib/x86_64-linux-gnu/libSDL3.so'
    cc = '/usr/bin/gcc' if args.gcc else str(ROOT / '.deps/bin/clang-cc')
    flags = ['-O1', '-march=x86-64-v2' if args.gcc else '-march=x86_64_v2',
             '-ffp-contract=off', '-fno-strict-aliasing', '-Wall', '-Wextra', '-Werror', '-Iruntime',
             '-I' + str(sysroot / 'usr/include')]
    env = {'PATH': '/usr/bin:/bin', 'LANG': 'C.UTF-8', 'LC_ALL': 'C.UTF-8', 'TMPDIR': str(out / 'tmp')}
    before = {name: sha(ROOT / name) for name in SOURCES}
    receipt = {'scope': 'synthetic paths only; no syscall confinement/gameplay coverage',
               'host': list(platform.uname()), 'python': sys.version, 'source_root': str(ROOT),
               'sources_before': before, 'compiler': cc, 'compiler_sha256': sha(Path(cc)),
               'SDL3': str(lib.resolve()), 'SDL3_sha256': sha(lib), 'commands': [], 'status': 'running'}
    def save():
        (out / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
    save()
    exe = out / 'test_paths'
    steps = [('version', [cc, '--version'], 20),
             ('compile', [cc, *flags, 'tests/native/test_paths_linux.c', 'runtime/paths.c', str(lib),
                          '-Wl,-rpath,' + str(lib.parent), '-o', str(exe)], 120),
             ('run', [str(exe), str(out / 'cases')], 30)]
    error = None
    for name, argv, timeout in steps:
        record = {'name': name, 'argv': argv, 'cwd': str(ROOT), 'env': env, 'timeout_seconds': timeout}
        receipt['commands'].append(record); save()
        start = time.monotonic()
        try:
            with (out / (name + '.log')).open('w') as log:
                proc = subprocess.Popen(argv, cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT,
                                        stdin=subprocess.DEVNULL, start_new_session=True)
                try:
                    proc.wait(timeout=timeout)
                except (subprocess.TimeoutExpired, KeyboardInterrupt):
                    os.killpg(proc.pid, signal.SIGKILL); proc.wait(); raise
            record.update(exit=proc.returncode, seconds=time.monotonic() - start)
            if proc.returncode:
                raise RuntimeError(f'{name} failed ({proc.returncode})')
            print(f'{name}: PASS', flush=True)
        except (OSError, subprocess.SubprocessError, RuntimeError, KeyboardInterrupt) as failure:
            error = str(failure) or type(failure).__name__; record['error'] = error; save(); break
        save()
    receipt['sources_after'] = {name: sha(ROOT / name) for name in SOURCES}
    if before != receipt['sources_after']:
        error = (error + '; ' if error else '') + 'source changed during run'
    if exe.is_file(): receipt['binary_sha256'] = sha(exe)
    receipt.update(status='failed' if error else 'passed', error=error)
    save()
    print(f"{'FAIL: ' + error if error else 'PASS: Linux paths'}\nReceipt: {out / 'receipt.json'}")
    return 1 if error else 0


if __name__ == '__main__':
    sys.exit(main())
