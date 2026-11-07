#!/usr/bin/env python3
"""Synthetic pctl client initialization IPC regression; no game input or graphics."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import resource
import signal
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
SOURCES = ['tests/native/test_pctl_linux.c', 'runtime/platform/linux.c', 'runtime/paths.c',
           'runtime/hle/fibers.c', 'runtime/hle/fiber_context_linux.c',
           'runtime/diag/diag.c', 'runtime/diag/sampler.c']


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def snapshot():
    pending = [ROOT / p for p in [*SOURCES, 'scripts/pctl_tests_linux.py']]
    found = {}
    while pending:
        path = pending.pop().resolve()
        name = str(path.relative_to(ROOT))
        if name in found:
            continue
        found[name] = digest(path)
        if path.suffix in ('.c', '.h'):
            for include in re.findall(r'^\s*#\s*include\s+"([^"]+)"', path.read_text(), re.M):
                target = (path.parent / include).resolve()
                if not target.is_file():
                    raise RuntimeError(f'local include missing: {target}')
                pending.append(target)
    return dict(sorted(found.items()))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, help='fresh directory under Monado_Linux/runs')
    args = parser.parse_args()
    if sys.platform != 'linux' or platform.machine() != 'x86_64':
        parser.error('native Linux x86-64 required')
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    os.umask(0o077)
    runs = ROOT / 'runs'
    if runs.is_symlink():
        parser.error('runs must not be a symlink')
    runs.mkdir(exist_ok=True)
    if args.output:
        output = args.output.absolute()
        if output != output.resolve() or not output.is_relative_to(runs) or output.exists():
            parser.error('--output must be fresh and non-symlink under Monado_Linux/runs')
        output.mkdir(parents=True)
    else:
        output = Path(tempfile.mkdtemp(prefix='linux-pctl.', dir=runs))
    env = {'PATH': '/usr/bin:/bin', 'LANG': 'C.UTF-8', 'LC_ALL': 'C.UTF-8',
           'TMPDIR': str(output), 'XCDE_STRICT': '1'}
    compiler = ROOT / '.deps/bin/clang-cc'
    sysroot = Path((ROOT / '.deps/graphics/active-sysroot.txt').read_text().strip())
    libdir = sysroot / 'usr/lib/x86_64-linux-gnu'
    sdl = libdir / 'libSDL3.so'
    receipt = {'scope': 'synthetic actual pctl/IPC/kernel client lifecycle; no game/GPU/saves',
               'status': 'running', 'host': list(platform.uname()), 'cwd': str(ROOT),
               'argv': sys.argv, 'sources_before': snapshot(), 'commands': [], 'artifacts': {},
               'compiler': str(compiler), 'compiler_sha256': digest(compiler),
               'sysroot': str(sysroot), 'sdl_sha256': digest(sdl),
               'limitations': ['Native parental-policy provider unavailable; initialization is client lifecycle only',
                               'No policy queries or enforcement validated; inherited query stubs remain unaudited',
                               'Repeated initialization is idempotent native behavior, not verified Horizon behavior',
                               'Normal IPC payloads retain word padding; domain logical lengths are exact',
                               'Kernel reset retains Service/KSession objects; pctl state cleanup tested in both reset orders after stop/join',
                               'Raw kernel CloseHandle does not release Service state; retained pctl state is reclaimed at reset',
                               'Unrelated guest dispatch/graphics callbacks are fixture adapters']}

    def save():
        (output / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')

    def run(name, argv, *, mode='0', expected=0, contains=(), artifact=None):
        argv = list(map(str, argv))
        child_env = dict(env, XCDE_DETERMINISTIC=mode)
        row = {'name': name, 'argv': argv, 'cwd': str(ROOT), 'env': child_env,
               'timeout_seconds': 60, 'expected_exit': expected}
        receipt['commands'].append(row); save()
        begin = time.monotonic()
        child = subprocess.Popen(argv, cwd=ROOT, env=child_env, stdin=subprocess.DEVNULL,
                                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT, start_new_session=True)
        timed_out = False
        try:
            stdout, _ = child.communicate(timeout=60)
        except (subprocess.TimeoutExpired, KeyboardInterrupt):
            timed_out = True
            os.killpg(child.pid, signal.SIGKILL)
            stdout, _ = child.communicate(timeout=5)
        (output / (name + '.log')).write_bytes(stdout)
        row.update(exit=child.returncode, seconds=time.monotonic() - begin, timed_out=timed_out,
                   required_output=list(contains), passed=not timed_out and child.returncode == expected
                   and all(text.encode() in stdout for text in contains))
        if artifact and row['passed']:
            receipt['artifacts'][artifact.name] = digest(artifact)
        save()
        print(name, 'PASS' if row['passed'] else 'FAIL', flush=True)
        if not row['passed']:
            raise RuntimeError(f'{name} failed; see {output / (name + ".log")}')

    save()
    try:
        run('compiler-version', [compiler, '--version'])
        binary = output / 'test_pctl_linux'
        flags = ['-O1', '-UNDEBUG', '-march=x86_64_v2', '-ffp-contract=off', '-fno-strict-aliasing',
                 '-pthread', '-Iruntime', '-I' + str(sysroot / 'usr/include')]
        # The fixture includes the registry's sources in one translation unit;
        # retain its used ELF sections under the Zig driver's default GC.
        run('compile', [compiler, *flags, *SOURCES, '-Wl,--no-gc-sections', '-rdynamic', str(sdl), '-Wl,-rpath,' + str(libdir),
                        '-ldl', '-lm', '-o', binary], artifact=binary)
        for mode in ('0', '1'):
            run('lifecycle-' + mode, [binary], mode=mode,
                contains=('8 reset cycles OK', 'parental-policy provider unavailable; enforcement unsupported'))
            for scenario, diagnostic in (
                    ('strict-factory', 'STRICT MODE: stopping at ipc [pctl] IParentalControlServiceFactory cmd 2147483647'),
                    ('strict-child', 'STRICT MODE: stopping at ipc [pctl] IParentalControlService cmd 2147483647'),
                    ('preinit-query', 'query before client initialization'),
                    ('stale-init', 'closed or reset client'),
                    ('stale-query', 'closed or reset client'),
                    ('factory-no-pid', 'expected 8 payload bytes, send-PID=1'),
                    ('factory-short', 'expected 8 payload bytes, send-PID=1'),
                    ('factory-domain-short', 'expected 8 payload bytes, send-PID=1'),
                    ('factory-extra', 'expected 8 payload bytes, send-PID=1'),
                    ('factory-reserved', 'nonzero factory reserved input'),
                    ('factory-handle', 'expected 8 payload bytes, send-PID=1'),
                    ('factory-buffer', 'expected 8 payload bytes, send-PID=1'),
                    ('factory-domain-object', 'expected 8 payload bytes, send-PID=1'),
                    ('factory-domain-bounds', 'invalid domain payload bounds'),
                    ('init-data', 'expected 0 payload bytes, send-PID=0'),
                    ('init-domain-data', 'expected 0 payload bytes, send-PID=0'),
                    ('init-pid', 'expected 0 payload bytes, send-PID=0'),
                    ('init-handle', 'expected 0 payload bytes, send-PID=0'),
                    ('init-buffer', 'expected 0 payload bytes, send-PID=0'),
                    ('init-domain-object', 'expected 0 payload bytes, send-PID=0')):
                run(scenario + '-' + mode, [binary, scenario], mode=mode,
                    expected=-signal.SIGABRT, contains=(diagnostic,))
        receipt['sources_after'] = snapshot()
        if receipt['sources_after'] != receipt['sources_before']:
            raise RuntimeError('source changed during tests; repeat with stable snapshot')
        receipt['status'] = 'passed'
    except Exception as exc:
        receipt['status'] = 'failed'
        receipt['error'] = str(exc)
    finally:
        save()
    print(receipt['status'], output / 'receipt.json')
    return 0 if receipt['status'] == 'passed' else 1


if __name__ == '__main__':
    raise SystemExit(main())
