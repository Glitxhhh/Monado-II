#!/usr/bin/env python3
"""[XCDE] LINUX-018: guarded real controller/supervisor/native compiler cancellation.

No launcher SDL initialization, visible window, pointer events or guest game boot.
The native precompile entry alone reads selected immutable input and creates its
hidden Vulkan window. Every output belongs to fresh runs/ state.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import resource
import sys

from boot_smoke_linux import bounded_process
import launcher_linux
import run_linux

ROOT = Path(__file__).resolve().parent.parent
FIXTURE = 'tests/native/test_launcher_precompile_integration_linux.cpp'


def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def sources():
    pending = [ROOT / name for name in (FIXTURE, 'scripts/launcher_precompile_integration_linux.py',
        'scripts/boot_smoke_linux.py', 'scripts/precompile_linux.py', 'scripts/run_linux.py',
        'scripts/launcher_linux.py', 'runtime/paths.c', 'runtime/platform/linux.c',
        'runtime/platform/linux_state_guard.c')]
    result = {}
    while pending:
        path = pending.pop().resolve(); name = str(path.relative_to(ROOT))
        if name in result:
            continue
        result[name] = digest(path)
        if path.suffix in ('.c', '.h', '.cpp', '.hpp'):
            for include in re.findall(r'^\s*#\s*include\s+"([^"]+)"', path.read_text(), re.M):
                candidate = (path.parent / include).resolve()
                if candidate.is_file():
                    pending.append(candidate)
    return dict(sorted(result.items()))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--executable', type=Path)
    parser.add_argument('--compile-only', action='store_true')
    parser.add_argument('--trace', action='store_true')
    parser.add_argument('--cancel-after-seconds', type=int, choices=range(1, 31), default=8)
    parser.add_argument('--native-timeout', type=int, choices=range(10, 51), default=25)
    parser.add_argument('--timeout', type=int, choices=range(20, 66), default=35)
    parser.add_argument('--require-built', action='store_true', help='require actual compiled shader work before cancellation')
    args = parser.parse_args()
    if args.native_timeout < args.cancel_after_seconds + 5 or args.timeout < args.native_timeout + 10:
        parser.error('timeouts must leave >=5s native cancellation grace and >=10s outer cleanup grace')
    if not args.compile_only and args.executable is None:
        parser.error('--executable is required unless --compile-only')
    out = args.output.absolute()
    if out.resolve() != out or not out.is_relative_to(ROOT / 'runs') or out.exists():
        parser.error('--output must be a fresh canonical directory under Linux runs/')
    os.umask(0o077); resource.setrlimit(resource.RLIMIT_CORE, (0, 0)); out.mkdir(parents=True)
    sysroot = Path((ROOT / '.deps/graphics/active-sysroot.txt').read_text().strip())
    cc, cxx = ROOT / '.deps/bin/clang-cc', ROOT / '.deps/bin/clang-cxx'
    compile_env = {'PATH': '/usr/bin:/bin', 'LANG': 'C.UTF-8', 'LC_ALL': 'C.UTF-8', 'TMPDIR': str(out), 'PYTHONDONTWRITEBYTECODE': '1'}
    receipt = {'scope': __doc__, 'status': 'running', 'argv': sys.argv, 'cwd': str(ROOT),
               'started_utc': datetime.now(timezone.utc).isoformat(), 'sources_before': sources(),
               'dependencies': {str(path): digest(path) for path in (cc, cxx, Path('/usr/bin/bwrap'))},
               'sysroot': str(sysroot), 'commands': [], 'artifacts': {}}

    def save():
        (out / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')

    def run(name, argv, environment, seconds=15):
        argv = list(map(str, argv))
        row = {'name': name, 'argv': argv, 'cwd': str(ROOT), 'environment': environment, 'seconds_limit': seconds}
        receipt['commands'].append(row); save(); completion = {}; failure = None
        try:
            bounded_process(argv, cwd=ROOT, env=environment, seconds=seconds, completion=completion)
        except BaseException as error:
            failure = error
        row.update(completion.get('record', {}))
        for stream in ('stdout', 'stderr'):
            path = out / (name + '.' + stream + '.log'); path.write_bytes(completion.get(stream, b'')); row[stream + '_sha256'] = digest(path)
        row['passed'] = not failure and row.get('exit') == 0 and not row.get('timed_out') and not row.get('killed_owned_pids') and not row.get('remaining_owned_pids') and not row.get('cleanup_errors')
        if failure:
            row['error'] = str(failure)
        save(); print(name, 'PASS' if row['passed'] else 'FAIL', flush=True)
        if not row['passed']:
            raise RuntimeError(name + ': inspect output logs')

    error = None
    try:
        flags = ['-O1', '-UNDEBUG', '-march=x86_64_v2', '-ffp-contract=off', '-fno-strict-aliasing',
                 '-ffunction-sections', '-fdata-sections', '-pthread', '-I' + str(sysroot / 'usr/include')]
        objects = []
        for name in ('runtime/platform/linux_state_guard.c', 'runtime/paths.c', 'runtime/platform/linux.c'):
            target = out / (Path(name).stem + '.o'); objects.append(target)
            run('compile-' + target.stem, [cc, *flags, '-c', ROOT / name, '-o', target], compile_env)
        binary = out / 'test_launcher_precompile_integration_linux'
        run('link-controller', [cxx, *flags, '-std=c++17', ROOT / FIXTURE, *objects, '-Wl,--gc-sections', '-o', binary], compile_env)
        receipt['artifacts'][binary.name] = digest(binary)
        if not args.compile_only:
            state = out / 'state'
            launch = run_linux.plan(args.executable, state, precompile=True)
            build = json.loads((Path(launch['argv'][0]).parent / 'receipt.json').read_text())
            if build.get('status') != 'passed' or build.get('precompile_protocol') != 1 or not build.get('generated'):
                raise RuntimeError('native compiler requires a passed generated build with precompile protocol 1')
            receipt['game_build_receipt_sha256'] = digest(Path(launch['argv'][0]).parent / 'receipt.json')
            receipt['game_before'] = {'path': launch['argv'][0], 'sha256': launch['binary_sha256']}
            argv = [str(binary), str(ROOT), launch['input'], launch['argv'][0], str(state),
                    str(args.cancel_after_seconds), str(args.native_timeout)]
            if args.trace:
                trace = state / 'trace'; trace.mkdir(parents=True)
                argv = ['/usr/bin/strace', '-ff', '-qq', '-s', '512', '--kill-on-exit',
                        '-e', 'trace=%process,socket,connect', '-o', str(trace / 'controller'), *argv]
                receipt['dependencies']['/usr/bin/strace'] = digest('/usr/bin/strace')
            argv, namespace = run_linux.dlss_namespace(argv, state, pid_namespace=False)
            argv.insert(1, '--die-with-parent')  # component lifetime only
            run_linux.prepare_namespace_state({'state': str(state), 'namespace': namespace})
            environment = run_linux.private_runtime_environment(launcher_linux.clean_environment(launch['library_path']), state, planned_namespace=True)
            receipt['namespace'] = namespace
            run('real-controller-native', argv, environment, seconds=args.timeout)
            if args.trace:
                allowed = {str(binary), '/usr/bin/python3', str(ROOT / '.venv/bin/python'), launch['argv'][0]}
                bad_exec, bad_network = [], []
                traces = sorted((state / 'trace').glob('controller.*'))
                for path in traces:
                    for line in path.read_text(errors='replace').splitlines():
                        if re.search(r'\b(?:socket\(AF_(?:INET6?|PACKET)\b|connect\([^\n]*sa_family=AF_INET6?\b)', line) and re.search(r'= (?:0|[1-9][0-9]*)\s*$', line):
                            bad_network.append(line)
                        if re.match(r'execve(?:at)?\(', line) and re.search(r'= 0\s*$', line):
                            match = re.match(r'execve\(("(?:[^"\\]|\\.)*")', line)
                            executable = json.loads(match[1]) if match else None
                            if executable not in allowed or (executable == launch['argv'][0] and '"--precompile"' not in line):
                                bad_exec.append(line)
                receipt['trace'] = {'files': {str(path.relative_to(out)): digest(path) for path in traces},
                                    'unexpected_successful_exec': bad_exec, 'unexpected_successful_network': bad_network}
                if not traces or bad_exec or bad_network:
                    raise RuntimeError('missing trace or unexpected successful exec/network')
            receipt['game_after_sha256'] = digest(launch['argv'][0])
            if receipt['game_after_sha256'] != receipt['game_before']['sha256']:
                raise RuntimeError('native binary changed during test')
            fields = dict(line.split(' ', 1) for line in (state / 'cache/precompile_status.txt').read_text().splitlines()[1:])
            receipt['final_status'] = fields
            if args.require_built and (int(fields['built']) <= 0 or int(fields['programs_done']) <= 0):
                raise RuntimeError('bounded cancellation occurred before actual shader compilation')
            for directory in ('save',):
                if any((state / directory).iterdir()):
                    raise RuntimeError('precompile unexpectedly created a save')
            receipt['state_artifacts'] = {str(path.relative_to(state)): {'bytes': path.stat().st_size, 'sha256': digest(path)}
                for path in state.rglob('*') if path.is_file() and not path.is_symlink()}
    except BaseException as failure:
        error = str(failure) or type(failure).__name__
    finally:
        receipt['sources_after'] = sources()
        if receipt['sources_after'] != receipt['sources_before']:
            error = (error + '; ' if error else '') + 'test source changed'
        receipt['dependencies_after'] = {path: digest(path) for path in receipt['dependencies']}
        if receipt['dependencies_after'] != receipt['dependencies']:
            error = (error + '; ' if error else '') + 'dependency changed'
        receipt.update(status='failed' if error else 'passed', error=error, finished_utc=datetime.now(timezone.utc).isoformat()); save()
    print(receipt['status'], out / 'receipt.json')
    return 1 if error else 0


if __name__ == '__main__':
    sys.exit(main())
