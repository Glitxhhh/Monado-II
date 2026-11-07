#!/usr/bin/env python3
"""[XCDE] LINUX-007: synthetic FPCR results and host-thread environment checks.

Clang compiles the actual SIMD helpers with the production Linux flags. GCC
independently checks libc/pthread FP state, since these helpers use Clang-only
elementwise builtins. Polluted host FP entry is always tested. --gap-probes also
requires architectural FPSR accumulation; known failures remain failures, with
separate logs. No game input, graphics or downloads are used.
"""
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
SOURCES = ('scripts/fpcr_tests_linux.py', 'tests/native/test_fpcr_linux.c',
           'runtime/platform/linux.c')


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def snapshot():
    pending = [ROOT / name for name in SOURCES]
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
    parser.add_argument('--gap-probes', action='store_true')
    args = parser.parse_args()
    if sys.platform != 'linux' or platform.machine() != 'x86_64':
        parser.error('native Linux x86-64 required')
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    os.umask(0o077)
    parent = ROOT / 'runs'
    if parent.is_symlink():
        parser.error('runs must not be a symlink')
    parent.mkdir(exist_ok=True)
    if args.output:
        output = args.output.absolute()
        if output != output.resolve() or not output.is_relative_to(parent) or output.exists():
            parser.error('--output must be fresh and non-symlink under Monado_Linux/runs')
        output.mkdir(parents=True)
    else:
        output = Path(tempfile.mkdtemp(prefix='linux-fpcr.', dir=parent))
    env = {'PATH': '/usr/bin:/bin', 'LANG': 'C.UTF-8', 'LC_ALL': 'C.UTF-8', 'TMPDIR': str(output)}
    clang = ROOT / '.deps/bin/clang-cc'
    gcc = Path('/usr/bin/gcc')
    sysroot = Path((ROOT / '.deps/graphics/active-sysroot.txt').read_text().strip())
    receipt = {'scope': 'synthetic FPCR results/thread state; no game execution',
        'status': 'running', 'host': list(platform.uname()), 'cwd': str(ROOT),
        'argv': sys.argv, 'sources_before': snapshot(), 'commands': [], 'artifacts': {},
        'compilers': {str(c): digest(c) for c in (clang, gcc)},
        'sysroot': str(sysroot),
        'limitations': ['GCC checks host FP environment only, not production Clang SIMD builtins',
                       'No full instruction differential coverage or game/timing coverage'],
        'gap_probes_requested': args.gap_probes}

    def save():
        (output / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')

    def run(name, argv, artifact=None, input_data=None):
        argv = list(map(str, argv))
        row = {'name': name, 'argv': argv, 'cwd': str(ROOT), 'env': env, 'timeout_seconds': 30}
        receipt['commands'].append(row); save()
        start = time.monotonic()
        child = subprocess.Popen(argv, cwd=ROOT, env=env, stdin=subprocess.PIPE,
                                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT, start_new_session=True)
        timed_out = False
        try:
            stdout, _ = child.communicate(input=input_data, timeout=30)
        except (subprocess.TimeoutExpired, KeyboardInterrupt):
            timed_out = True
            os.killpg(child.pid, signal.SIGKILL)
            stdout, _ = child.communicate(timeout=5)
        (output / (name + '.log')).write_bytes(stdout)
        row.update(exit=child.returncode, seconds=time.monotonic() - start, timed_out=timed_out,
                   passed=child.returncode == 0 and not timed_out)
        if artifact and row['passed']:
            receipt['artifacts'][artifact.name] = digest(artifact)
        save()
        print(name, 'PASS' if row['passed'] else 'FAIL', flush=True)
        return row['passed'], stdout

    save()
    try:
        for name, compiler in (('clang', clang), ('gcc', gcc)):
            if not run(name + '-version', [compiler, '--version'])[0]:
                raise RuntimeError(name + ' compiler unavailable')
        common = ['-O1', '-UNDEBUG', '-ffp-contract=off', '-fno-strict-aliasing', '-Wall', '-Wextra', '-Werror',
                  '-ffunction-sections', '-fdata-sections', '-I' + str(sysroot / 'usr/include')]
        clang_flags = [clang, '-march=x86_64_v2', *common]
        ok, macros = run('clang-macros', [*clang_flags, '-dM', '-E', '-x', 'c', '-'], input_data=b'')
        if not ok or re.search(rb'^#define (?:NDEBUG|__FAST_MATH__)(?:\s|$)', macros, re.M):
            raise RuntimeError('compiler assertions/FP macro check failed')
        binary = output / 'test_fpcr_linux'
        source = ROOT / 'tests/native/test_fpcr_linux.c'
        link = [ROOT / 'runtime/platform/linux.c', '-Wl,--gc-sections', '-lm', '-pthread']
        if not run('compile-clang', [*clang_flags, source, *link, '-o', binary], binary)[0]:
            raise RuntimeError('Clang helper fixture failed to compile')
        run('fpcr-results-and-threads', [binary])
        if args.gap_probes:
            run('architectural-fpsr', [binary, '--fpsr'])
        else:
            receipt['limitations'].append('Architectural FPSR accumulation gap probe was not requested')
        binary_gcc = output / 'test_fp_environment_gcc'
        if not run('compile-gcc-host', [gcc, '-march=x86-64-v2', *common, '-frounding-math', '-DHOST_ONLY',
                                      source, *link, '-o', binary_gcc], binary_gcc)[0]:
            raise RuntimeError('GCC host fixture failed to compile')
        run('gcc-host-environment', [binary_gcc])
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        receipt['error'] = str(error)
    receipt['sources_after'] = snapshot()
    receipt['sources_stable'] = receipt['sources_before'] == receipt['sources_after']
    receipt['status'] = 'passed' if (receipt['sources_stable'] and not receipt.get('error') and
        all(row.get('passed') for row in receipt['commands'])) else 'failed'
    save()
    print(receipt['status'], output / 'receipt.json')
    return 0 if receipt['status'] == 'passed' else 1


if __name__ == '__main__':
    sys.exit(main())
