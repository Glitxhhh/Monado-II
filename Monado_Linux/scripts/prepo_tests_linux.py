#!/usr/bin/env python3
"""Synthetic prepo offline-result IPC regression; no game input or graphics."""
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
SOURCES = ['tests/native/test_prepo_linux.c', 'runtime/platform/linux.c', 'runtime/paths.c',
           'runtime/hle/fibers.c', 'runtime/hle/fiber_context_linux.c',
           'runtime/diag/diag.c', 'runtime/diag/sampler.c']


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def snapshot():
    pending = [ROOT / p for p in [*SOURCES, 'scripts/prepo_tests_linux.py']]
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
    parser.add_argument('--expected-result', type=lambda value: int(value, 0), required=True, help='audited nonzero service result')
    parser.add_argument('--baseline-only', action='store_true', help='retain the supported-request failure before implementation')
    args = parser.parse_args()
    if not 0 < args.expected_result <= 0xffffffff:
        parser.error('--expected-result must be a nonzero uint32')
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
        output = Path(tempfile.mkdtemp(prefix='linux-prepo.', dir=runs))
    env = {'PATH': '/usr/bin:/bin', 'LANG': 'C.UTF-8', 'LC_ALL': 'C.UTF-8',
           'TMPDIR': str(output), 'XCDE_STRICT': '1'}
    compiler = ROOT / '.deps/bin/clang-cc'
    sysroot = Path((ROOT / '.deps/graphics/active-sysroot.txt').read_text().strip())
    libdir = sysroot / 'usr/lib/x86_64-linux-gnu'
    sdl = libdir / 'libSDL3.so'
    receipt = {'scope': 'synthetic actual prepo/IPC/kernel offline-result contract; no game/GPU/saves/reports',
               'status': 'running', 'host': list(platform.uname()), 'cwd': str(ROOT),
               'argv': sys.argv, 'sources_before': snapshot(), 'commands': [], 'artifacts': {},
               'compiler': str(compiler), 'compiler_sha256': digest(compiler),
               'sysroot': str(sysroot), 'sdl_sha256': digest(sdl),
               'limitations': ['Explicit native offline refusal is not console report storage/transmission parity',
                               'IPC envelope is validated; refusal precedes reading UID/reserved or either input buffer',
                               'Normal IPC sizes retain word padding; domain lengths are exact',
                               'Unrelated guest dispatch/graphics callbacks are fixture adapters'],
               'expected_result': args.expected_result}


    def save():
        temporary = output / 'receipt.json.tmp'
        temporary.write_text(json.dumps(receipt, indent=2) + '\n')
        temporary.replace(output / 'receipt.json')

    def run(name, argv, *, mode='0', strict='1', expected=0, contains=(), artifact=None):
        argv = list(map(str, argv))
        child_env = dict(env, XCDE_DETERMINISTIC=mode, XCDE_STRICT=strict)
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
        binary = output / 'test_prepo_linux'
        flags = ['-O1', '-UNDEBUG', '-march=x86_64_v2', '-ffp-contract=off', '-fno-strict-aliasing',
                 '-pthread', '-Iruntime', '-I' + str(sysroot / 'usr/include')]
        # The fixture includes the registry's sources in one translation unit;
        # retain its used ELF sections under the Zig driver's default GC.
        run('compile', [compiler, *flags, *SOURCES, '-Wl,--no-gc-sections', '-rdynamic', str(sdl), '-Wl,-rpath,' + str(libdir),
                        '-ldl', '-lm', '-o', binary], artifact=binary)
        for mode in ('0', '1'):
            run('offline-' + mode, [binary, hex(args.expected_result)], mode=mode,
                contains=('explicit refusal', '4 reset cycles OK'))
            if args.baseline_only:
                continue
            run('unknown-nonstrict-' + mode, [binary, hex(args.expected_result), 'unknown-nonstrict'],
                mode=mode, strict='0', contains=('explicit refusal', '4 reset cycles OK'))
            for scenario in ('null-buffers', 'oversized-buffers'):
                run(scenario + '-' + mode, [binary, hex(args.expected_result), scenario], mode=mode,
                    contains=('explicit refusal', '4 reset cycles OK'))
            for scenario in ('short', 'domain-short', 'extra-data', 'empty-data', 'missing-pid',
                             'missing-buffers', 'missing-event', 'missing-report', 'copy-handle',
                             'output-buffer', 'domain-object'):
                run(scenario + '-' + mode, [binary, hex(args.expected_result), scenario], mode=mode,
                    expected=-signal.SIGABRT,
                    contains=('[prepo:u] malformed SaveReportWithUserOld2 envelope',))
            for scenario, diagnostic in (
                    ('unknown', 'STRICT MODE: stopping at ipc'),
                    ('unknown-domain', 'STRICT MODE: stopping at ipc'),
                    ('domain-bounds', 'invalid domain payload bounds')):
                run(scenario + '-' + mode, [binary, hex(args.expected_result), scenario], mode=mode,
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
