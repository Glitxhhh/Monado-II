#!/usr/bin/env python3
"""[XCDE] LINUX-005: build/test shared host import, optionally on the real NVIDIA GPU.

No game files or windows. Real device cases apply the Linux state-write guard
before Vulkan initialization. Use actual tool approval if GPU devices are hidden.
--full-heap imports the 3.25 GiB guest extent, sampling beginning/middle/end; it
does not inspect every byte or validate game rendering. No downloads occur.
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

ROOT = Path(__file__).resolve().parent.parent


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def source_hashes():
    pending = [ROOT / path for path in ('scripts/gpu_import_tests_linux.py',
        'runtime/nvn/vulkan/host_import.c', 'runtime/platform/linux_state_guard.c',
        'tests/native/test_host_import.c', 'tests/native/test_vulkan_import_linux.c')]
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
    parser.add_argument('--real-device', action='store_true', help='run synthetic transfers on an NVIDIA discrete GPU')
    parser.add_argument('--full-heap', action='store_true', help='also import the full guest-heap extent at HEAP_BASE')
    parser.add_argument('--output', type=Path, help='fresh directory under Monado_Linux/runs/')
    args = parser.parse_args()
    if sys.platform != 'linux' or platform.machine() != 'x86_64':
        parser.error('native Linux x86-64 required')
    if args.full_heap and not args.real_device:
        parser.error('--full-heap requires --real-device')
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    os.umask(0o077)
    if args.output:
        output = args.output.absolute()
        if output != output.resolve() or not output.is_relative_to(ROOT / 'runs') or output.exists():
            parser.error('--output must be a fresh non-symlink directory under Monado_Linux/runs')
        output.mkdir(parents=True)
    else:
        if (ROOT / 'runs').is_symlink():
            parser.error('runs must not be a symlink')
        (ROOT / 'runs').mkdir(exist_ok=True)
        output = Path(tempfile.mkdtemp(prefix='linux-gpu-import.', dir=ROOT / 'runs'))
    sysroot = Path((ROOT / '.deps/graphics/active-sysroot.txt').read_text().strip())
    compiler = ROOT / '.deps/bin/clang-cc'
    vulkan = sysroot / 'usr/lib/x86_64-linux-gnu/libvulkan.so'
    environment = {'PATH': '/usr/bin:/bin', 'LANG': 'C.UTF-8', 'LC_ALL': 'C.UTF-8', 'TMPDIR': str(output)}
    receipt = {'scope': 'synthetic Vulkan import only; no game input/rendering', 'status': 'running',
        'host': list(platform.uname()), 'cwd': str(ROOT), 'argv': sys.argv, 'sources_before': source_hashes(),
        'compiler': str(compiler), 'compiler_sha256': digest(compiler), 'sysroot': str(sysroot),
        'vulkan': str(vulkan.resolve()), 'vulkan_sha256': digest(vulkan), 'commands': [], 'artifacts': {}}

    def save():
        (output / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')

    def run(name, argv, timeout=60, artifact=None):
        argv = list(map(str, argv))
        record = {'name': name, 'argv': argv, 'cwd': str(ROOT), 'env': environment, 'timeout': timeout}
        receipt['commands'].append(record); save()
        start = time.monotonic()
        child = subprocess.Popen(argv, cwd=ROOT, env=environment, stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, start_new_session=True)
        timed_out = False
        try:
            stdout, _ = child.communicate(timeout=timeout)
        except (subprocess.TimeoutExpired, KeyboardInterrupt):
            timed_out = True
            os.killpg(child.pid, signal.SIGKILL)
            stdout, _ = child.communicate(timeout=5)
        (output / (name + '.log')).write_bytes(stdout)
        record.update(exit=child.returncode, seconds=time.monotonic() - start, timed_out=timed_out,
            passed=child.returncode == 0 and not timed_out)
        if artifact and record['passed']:
            receipt['artifacts'][str(artifact.relative_to(output))] = digest(artifact)
        save(); print(name, 'PASS' if record['passed'] else 'FAIL', flush=True)
        if not record['passed']:
            raise RuntimeError(f'{name}: see {output / (name + ".log")}')

    save()
    error = None
    try:
        run('compiler-version', [compiler, '--version'])
        base = [compiler, '-O1', '-march=x86_64_v2', '-ffp-contract=off', '-fno-strict-aliasing',
            '-Wall', '-Wextra', '-Werror', '-I' + str(sysroot / 'usr/include')]
        unit = output / 'test_host_import'
        live = output / 'test_vulkan_import_linux'
        run('compile-unit', [*base, 'tests/native/test_host_import.c', 'runtime/nvn/vulkan/host_import.c', '-o', unit], artifact=unit)
        run('unit', [unit])
        run('compile-live', [*base, 'tests/native/test_vulkan_import_linux.c', 'runtime/nvn/vulkan/host_import.c',
            'runtime/platform/linux_state_guard.c', vulkan, '-Wl,-rpath,' + str(vulkan.parent), '-o', live], artifact=live)
        if args.real_device:
            state = output / 'small-state'; state.mkdir()
            run('real-small-guarded', [live, '--state-root', state])
        if args.full_heap:
            state = output / 'full-heap-state'; state.mkdir()
            run('real-full-heap-guarded', [live, '--state-root', state, '--full-heap'])
    except (RuntimeError, OSError, subprocess.SubprocessError) as failure:
        error = str(failure)
    receipt['sources_after'] = source_hashes()
    if receipt['sources_after'] != receipt['sources_before']:
        error = (error + '; ' if error else '') + 'source changed during run'
    receipt['status'] = 'failed' if error else 'passed'
    receipt['error'] = error
    save()
    print(receipt['status'], output / 'receipt.json')
    return 1 if error else 0


if __name__ == '__main__':
    sys.exit(main())
