#!/usr/bin/env python3
"""[XCDE] LINUX-018: bounded authored precompile/cache components, no game boot.

Default CPU cases use real shaderc and production cache source, guarded fresh
state and synthetic I/O faults. --before-probes preserves known failing baseline
reproducers. No selected RomFS, human profile, Windows seed or GPU is used here.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import resource
import shlex
import shutil
import sys

from boot_smoke_linux import bounded_process
import run_linux

ROOT = Path(__file__).resolve().parent.parent
FIXTURE = 'tests/native/test_spirv_cache_linux.c'
GPU_FIXTURE = 'tests/native/test_precompile_gpu_linux.c'


def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def sources():
    pending = [ROOT / name for name in (FIXTURE, GPU_FIXTURE, 'scripts/precompile_tests_linux.py',
        'scripts/boot_smoke_linux.py', 'scripts/platform_tests_linux.py', 'scripts/run_linux.py',
        'runtime/platform/linux_state_guard.c')]
    found = {}
    while pending:
        path = pending.pop().resolve()
        name = str(path.relative_to(ROOT))
        if name in found:
            continue
        found[name] = digest(path)
        if path.suffix in ('.c', '.h', '.cpp', '.hpp'):
            for include in re.findall(r'^\s*#\s*include\s+"([^"]+)"', path.read_text(), re.M):
                included = (path.parent / include).resolve()
                if not included.is_file():
                    raise RuntimeError(f'local source include missing: {included}')
                pending.append(included)
    return dict(sorted(found.items()))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True, help='new ordinary directory under Linux runs/')
    parser.add_argument('--before-probes', action='store_true', help='expect the recorded baseline cache failures, not a feature pass')
    parser.add_argument('--gpu', action='store_true', help='also run actual offscreen authored GPU pipeline/cache checks')
    parser.add_argument('--gpu-build-only', action='store_true', help='compile the authored GPU fixture without running it')
    parser.add_argument('--trace', action='store_true', help='trace GPU file/process/network calls; requires --gpu')
    args = parser.parse_args()
    if args.before_probes and (args.gpu or args.gpu_build_only):
        parser.error('baseline failure probes do not run the GPU component')
    if args.trace and not args.gpu:
        parser.error('--trace requires --gpu')
    if args.gpu and not Path('/usr/bin/bwrap').is_file():
        parser.error('--gpu requires installed bwrap for private runtime/cache namespace')
    if args.trace and not shutil.which('strace'):
        parser.error('--trace requires installed strace')
    out = args.output.absolute()
    if out != out.resolve() or not out.is_relative_to(ROOT / 'runs') or out.exists():
        parser.error('--output must be a new ordinary directory under Monado_Linux/runs/')
    os.umask(0o077)
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    out.mkdir(parents=True)
    sysroot = Path((ROOT / '.deps/graphics/active-sysroot.txt').read_text().strip())
    library = sysroot / 'usr/lib/x86_64-linux-gnu'
    cc = ROOT / '.deps/bin/clang-cc'
    env = {'PATH': '/usr/bin:/bin', 'LANG': 'C.UTF-8', 'LC_ALL': 'C.UTF-8',
           'TMPDIR': str(out), 'LD_LIBRARY_PATH': str(library), 'PYTHONDONTWRITEBYTECODE': '1'}
    receipt = {'scope': 'actual shaderc/SPIR-V cache; authored data and synthetic faults only; no Vulkan/game/UI/human state',
               'status': 'running', 'started_utc': datetime.now(timezone.utc).isoformat(),
               'argv': sys.argv, 'cwd': str(ROOT), 'host': list(platform.uname()), 'sources_before': sources(),
               'dependencies': {str(path.resolve()): digest(path) for path in (cc, library / 'libshaderc.so')},
               'sysroot': str(sysroot), 'commands': [], 'artifacts': {},
               'before_probes': args.before_probes}
    if args.gpu or args.gpu_build_only:
        receipt['scope'] = 'actual shaderc/cache and authored offscreen Vulkan pipeline; no game/window/human state'
        receipt['dependencies'][str((library / 'libvulkan.so').resolve())] = digest(library / 'libvulkan.so')
    if args.gpu:
        receipt['dependencies']['/usr/bin/bwrap'] = digest('/usr/bin/bwrap')
    if args.trace:
        receipt['dependencies']['/usr/bin/strace'] = digest('/usr/bin/strace')
    # Retain exact authored reproducers before production fixes change them.
    for name in receipt['sources_before']:
        target = out / 'source-snapshot' / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes((ROOT / name).read_bytes())

    def save():
        (out / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')

    def run(name, argv, *, seconds=30, expected=0, expect_timeout=False, artifact=None, gpu_state=None):
        argv = list(map(str, argv))
        program = argv[0]
        trace_root = None
        if gpu_state:
            if args.trace:
                trace_root = gpu_state / 'trace'; trace_root.mkdir(exist_ok=True)
                argv = ['/usr/bin/strace', '-ff', '-qq', '-s', '256', '--kill-on-exit',
                        '-e', 'trace=%file,%process,socket,connect,bind,sendto,sendmsg', '-e', 'raw=sendto,sendmsg',
                        '-o', str(trace_root / name), *argv]
            argv, namespace = run_linux.dlss_namespace(argv, gpu_state)
            argv.insert(1, '--die-with-parent')  # owned component only, never gameplay
            run_linux.prepare_namespace_state({'state': str(gpu_state), 'namespace': namespace})
        item = {'name': name, 'argv': argv, 'cwd': str(ROOT), 'environment': env,
                'expected_exit': expected, 'expected_timeout': expect_timeout, 'seconds_limit': seconds}
        receipt['commands'].append(item); save()
        completion = {}
        failure = None
        try:
            bounded_process(argv, cwd=ROOT, env=env, seconds=seconds, completion=completion)
        except BaseException as error:
            failure = error
        item.update(completion.get('record', {}))
        for stream in ('stdout', 'stderr'):
            path = out / (name + '.' + stream + '.log')
            path.write_bytes(completion.get(stream, b''))
            item[stream + '_sha256'] = digest(path)
        if trace_root:
            traces = sorted(trace_root.glob(name + '.*'))
            item['traces'] = {str(path.relative_to(out)): digest(path) for path in traces}
            bad_network, bad_exec = [], []
            for path in traces:
                for line in path.read_text(errors='replace').splitlines():
                    if re.search(r'socket\(AF_(?:INET6?|PACKET)\b|sa_family=AF_INET6?\b', line):
                        bad_network.append({'file': path.name, 'line': line})
                    if re.match(r'execve(?:at)?\(', line) and re.search(r'= 0\s*$', line):
                        if not line.startswith('execve(' + json.dumps(program) + ','):
                            bad_exec.append({'file': path.name, 'line': line})
            item['unexpected_network'] = bad_network
            item['unexpected_successful_exec'] = bad_exec
            if not traces or bad_network or bad_exec:
                failure = failure or RuntimeError('missing trace or unexpected GPU network/external exec')
        clean = not item.get('remaining_owned_pids') and not item.get('cleanup_errors')
        if expect_timeout:
            okay = not failure and clean and item.get('timed_out')
        else:
            okay = not failure and clean and not item.get('timed_out') and not item.get('killed_owned_pids') and item.get('exit') == expected
        item['expectation_met'] = okay
        if failure:
            item['error'] = str(failure)
        if artifact and okay:
            receipt['artifacts'][str(artifact.relative_to(out))] = digest(artifact)
        save(); print(name, 'EXPECTED' if okay and (expected or expect_timeout) else 'PASS' if okay else 'FAIL', flush=True)
        if isinstance(failure, (KeyboardInterrupt, SystemExit)):
            raise failure
        if not okay:
            raise RuntimeError(f'{name}: inspect {out / (name + ".stderr.log")}')
        return completion.get('stdout', b'').decode(errors='replace')

    save()
    error = None
    interrupted = None
    try:
        run('compiler-version', [cc, '--version'])
        pkg = run('shaderc-flags', [ROOT / '.deps/bin/pkg-config-graphics', '--cflags', '--libs', 'shaderc'])
        binary = out / 'test_spirv_cache_linux'
        flags = ['-O1', '-UNDEBUG', '-march=x86_64_v2', '-ffp-contract=off', '-fno-strict-aliasing',
                 '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections', '-pthread', '-Wall', '-Wextra',
                 '-I' + str(sysroot / 'usr/include')]
        if args.before_probes:
            flags.append('-DCACHE_BEFORE_PROBE=1')
        run('compile-cache', [cc, *flags, ROOT / FIXTURE, ROOT / 'runtime/platform/linux_state_guard.c',
                             *shlex.split(pkg), '-Wl,-rpath,' + str(library), '-o', binary], artifact=binary)
        cases = ['cold-warm', 'corrupt-repair', 'short-read', 'short-write', 'lock-failure', 'loose-buffered-failure']
        if not args.before_probes:
            cases += ['structure-repair', 'write-failure-render', 'flush-failure', 'eintr', 'concurrency']
        for case in cases:
            state = out / ('state-' + case); state.mkdir()
            timeout = args.before_probes and case == 'short-read'
            expected = 1 if args.before_probes and case != 'cold-warm' and not timeout else 0
            run(case, [binary, case, state], seconds=1 if timeout else 30, expected=expected, expect_timeout=timeout)
            if case == 'cold-warm':
                run('cross-process-hit', [binary, 'reopen-hit', state])
        if args.gpu or args.gpu_build_only:
            gpu_binary = out / 'test_precompile_gpu_linux'
            run('compile-gpu', [cc, *flags, ROOT / GPU_FIXTURE, ROOT / 'runtime/platform/linux_state_guard.c',
                                *shlex.split(pkg), library / 'libvulkan.so', '-Wl,-rpath,' + str(library),
                                '-o', gpu_binary], artifact=gpu_binary)
            if args.gpu:
                state = out / 'gpu-state'; state.mkdir()
                for mode in ('cold', 'warm'):
                    run('gpu-' + mode, [gpu_binary, mode, state], seconds=30, gpu_state=state)
                receipt['gpu_cache_files'] = {str(path.relative_to(out)): digest(path)
                                               for path in sorted((state / 'cache').rglob('*')) if path.is_file()}
    except BaseException as failure:
        error = str(failure) or type(failure).__name__
        if isinstance(failure, (KeyboardInterrupt, SystemExit)):
            interrupted = failure
    finally:
        receipt['sources_after'] = sources()
        receipt['dependencies_after'] = {name: digest(name) for name in receipt['dependencies']}
        if receipt['sources_before'] != receipt['sources_after'] or receipt['dependencies'] != receipt['dependencies_after']:
            error = (error + '; ' if error else '') + 'source/dependency changed during test'
        receipt['status'] = 'failed' if error else 'expected-baseline-failures-observed' if args.before_probes else 'passed'
        receipt['error'] = error
        receipt['finished_utc'] = datetime.now(timezone.utc).isoformat()
        save()
    print(receipt['status'], out / 'receipt.json')
    if interrupted:
        raise interrupted
    return 1 if error else 0


if __name__ == '__main__':
    sys.exit(main())
