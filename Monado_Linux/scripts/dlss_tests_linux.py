#!/usr/bin/env python3
"""[XCDE] LINUX-017: isolated, bounded native NGX SR/DLAA synthetic components.

Builds the production Vulkan-only provider. Default runs only missing-runtime
contracts; --real-device enables actual offscreen GPU evaluation. No game,
display/window, downloads or human state. Host GPU access uses real tool approval.
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
import shutil
import sys

from boot_smoke_linux import bounded_process

ROOT = Path(__file__).resolve().parent.parent
SDK_DEFAULT = ROOT / '.deps/dlss/sdk/f15076a14b594f71'


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def source_hashes():
    pending = [ROOT / name for name in (
        'scripts/dlss_tests_linux.py', 'scripts/boot_smoke_linux.py',
        'scripts/platform_tests_linux.py', 'scripts/run_linux.py',
        'tests/native/test_dlss_linux.cpp', 'runtime/nvn/vulkan/dlss_linux.cpp',
        'runtime/nvn/vulkan/dlss_ngx_loader.cpp',
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
                target = (path.parent / include).resolve()
                if target.is_file():
                    pending.append(target)
                else:
                    raise RuntimeError(f'local include missing: {target}')
    return dict(sorted(found.items()))


def file_manifest(root):
    return {str(path.relative_to(root)): digest(path) for path in sorted(root.rglob('*')) if path.is_file()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True, help='fresh ordinary directory below Linux runs/')
    parser.add_argument('--sdk-root', type=Path, default=SDK_DEFAULT, help='already audited project-local SDK; never downloaded here')
    parser.add_argument('--real-device', action='store_true', help='run synthetic offscreen NVIDIA GPU evaluations')
    parser.add_argument('--capability-only', action='store_true', help='GPU init/mode/contracts only, no neural evaluation')
    parser.add_argument('--trace', action='store_true', help='record file/process/network syscalls and fail IP network attempts')
    parser.add_argument('--quoted-state', action='store_true', help='exercise a private state pathname containing spaces, quotes and UTF-8')
    parser.add_argument('--state-kind', choices=('normal', 'spaces', 'quotes', 'utf8'), default='normal',
                        help='focus one private pathname edge case')
    parser.add_argument('--ascii-data-alias', action='store_true', help='pass private /tmp/ngx-data bind alias to NGX')
    parser.add_argument('--xdg-runtime', choices=('missing', 'readonly', 'private'), default='missing',
                        help='simulate a display session runtime dir; private uses /tmp/runtime alias')
    parser.add_argument('--real-seconds', type=int, choices=range(1, 61), default=60, metavar='1..60',
                        help='outer bound for the real-device fixture, including blocked SDK startup')
    args = parser.parse_args()
    if sys.platform != 'linux' or platform.machine() != 'x86_64':
        parser.error('native Linux x86-64 required')
    if args.capability_only and not args.real_device:
        parser.error('--capability-only requires --real-device')
    if (args.quoted_state or args.state_kind != 'normal') and not args.real_device:
        parser.error('pathname edge cases require --real-device')
    if args.quoted_state and args.state_kind != 'normal':
        parser.error('--quoted-state and --state-kind are mutually exclusive')
    if args.ascii_data_alias and not args.real_device:
        parser.error('--ascii-data-alias requires --real-device')
    if args.xdg_runtime != 'missing' and not args.real_device:
        parser.error('--xdg-runtime requires --real-device')
    if args.trace and not shutil.which('strace'):
        parser.error('--trace requires installed strace')
    bwrap = Path('/usr/bin/bwrap')
    if args.real_device and not bwrap.is_file():
        parser.error('--real-device requires installed bwrap for private /tmp and updater masking')
    output = args.output.absolute()
    if output != output.resolve() or not output.is_relative_to(ROOT / 'runs') or output.exists():
        parser.error('--output must be a fresh ordinary directory below Monado_Linux/runs/')
    sdk = args.sdk_root.absolute()
    if sdk.resolve() != sdk or not sdk.is_relative_to(ROOT / '.deps/dlss'):
        parser.error('--sdk-root must be an ordinary canonical project-local .deps/dlss path')
    header = sdk / 'include/nvsdk_ngx.h'
    archive = sdk / 'lib/Linux_x86_64/libnvsdk_ngx.a'
    runtime = sdk / 'lib/Linux_x86_64/rel/libnvidia-ngx-dlss.so.310.9.1'
    for path in (header, archive, runtime):
        if not path.is_file() or path.is_symlink():
            parser.error(f'missing ordinary audited SDK component: {path}')
    os.umask(0o077)
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    output.mkdir(parents=True, exist_ok=False)
    feature = output / 'feature'
    feature.mkdir()
    runtime_copy = feature / runtime.name
    shutil.copyfile(runtime, runtime_copy)
    runtime_copy.chmod(0o444)
    if digest(runtime_copy) != digest(runtime):
        raise RuntimeError('private copied SR runtime hash mismatch')
    sysroot = Path((ROOT / '.deps/graphics/active-sysroot.txt').read_text().strip())
    if not sysroot.is_absolute():
        sysroot = ROOT / sysroot
    cc, cxx = ROOT / '.deps/bin/clang-cc', ROOT / '.deps/bin/clang-cxx'
    gnu_cxx = Path('/usr/lib/x86_64-linux-gnu/libstdc++.so.6')
    vulkan = sysroot / 'usr/lib/x86_64-linux-gnu/libvulkan.so'
    environment = {'PATH': '/usr/bin:/bin', 'LANG': 'C.UTF-8', 'LC_ALL': 'C.UTF-8',
                   'TMPDIR': str(output), 'PYTHONDONTWRITEBYTECODE': '1'}
    exports = output / 'ngx-exports.map'
    exports.write_text('{ global: monado_ngx_api; local: *; };\n')
    receipt = {
        'scope': 'synthetic production NGX SR/DLAA component; no game/window/human profile',
        'status': 'running', 'host': list(platform.uname()), 'cwd': str(ROOT), 'argv': sys.argv,
        'started_utc': datetime.now(timezone.utc).isoformat(),
        'sources_before': source_hashes(), 'sdk_root': str(sdk),
        'sdk_headers_before': file_manifest(sdk / 'include'),
        'dependencies': {str(path): digest(path) for path in (cc, cxx, archive, runtime, vulkan.resolve(), gnu_cxx.resolve())},
        'sysroot': str(sysroot), 'commands': [], 'artifacts': {str(runtime_copy.relative_to(output)): digest(runtime_copy),
                                                            exports.name: digest(exports)},
        'limits': 'compile/contracts/capability/evaluation are distinct; no gameplay temporal quality or validation-layer claim',
    }
    if args.real_device:
        receipt['bwrap'] = {'path': str(bwrap), 'sha256': digest(bwrap)}
        receipt['dependencies'][str(bwrap)] = digest(bwrap)
    if args.trace:
        receipt['dependencies']['/usr/bin/strace'] = digest(Path('/usr/bin/strace'))

    def save():
        (output / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')

    def run(name, argv, *, seconds=60, artifact=None, trace=False, namespace_state=None, extra_env=None):
        argv = list(map(str, argv))
        program = argv[0]
        trace_root = output
        if namespace_state:
            trace_root = namespace_state / 'trace'
            trace_root.mkdir()
        if trace:
            argv = ['/usr/bin/strace', '-ff', '-qq', '-s', '256', '--kill-on-exit',
                    '-e', 'trace=%file,%process,socket,socketpair,connect,bind,sendto,sendmsg,io_uring_setup',
                    '-e', 'raw=sendto,sendmsg', '-o', str(trace_root / (name + '.trace')), *argv]
        if namespace_state:
            # Keep strace inside the private PID namespace: its trace TIDs then
            # match the fixture's gettid network-policy receipt. Root remains
            # read-only; /tmp is a bind alias of the same private state inode.
            argv = [str(bwrap), '--unshare-user', '--unshare-pid', '--unshare-net',
                    '--ro-bind', '/', '/', '--dev-bind', '/dev', '/dev', '--proc', '/proc',
                    '--bind', str(namespace_state), str(namespace_state),
                    '--bind', str(namespace_state), '/tmp',
                    '--ro-bind', '/dev/null', '/usr/bin/nvidia-ngx-updater', '--', *argv]
        command_environment = {**environment, **(extra_env or {})}
        record = {'name': name, 'argv': argv, 'cwd': str(ROOT), 'environment': command_environment, 'seconds_limit': seconds}
        receipt['commands'].append(record); save()
        completion = {}

        def started(identity):
            record['started'] = dict(identity); save()

        failure = None
        try:
            bounded_process(argv, cwd=ROOT, env=command_environment, seconds=seconds, on_start=started, completion=completion)
        except BaseException as error:
            failure = error
        record.update(completion.get('record', {}))
        stdout, stderr = completion.get('stdout', b''), completion.get('stderr', b'')
        (output / (name + '.stdout.log')).write_bytes(stdout)
        (output / (name + '.stderr.log')).write_bytes(stderr)
        record['stdout_sha256'] = digest(output / (name + '.stdout.log'))
        record['stderr_sha256'] = digest(output / (name + '.stderr.log'))
        metrics = []
        probes = []
        for line in stdout.decode(errors='replace').splitlines():
            if line.startswith('METRIC '):
                metrics.append(json.loads(line[7:]))
            if line.startswith('NETWORK_PROBE '):
                probes.append(json.loads(line[14:]))
        record['metrics'] = metrics
        record['expected_denied_network_probes'] = probes
        if trace:
            traces = sorted(trace_root.glob(name + '.trace.*'))
            record['traces'] = {str(path.relative_to(output)): digest(path) for path in traces}
            network = []
            external_execs = []
            blocked_updaters = []
            for path in traces:
                for line in path.read_text(errors='replace').splitlines():
                    if re.search(r'socket\(AF_(?:INET6?|PACKET)\b|sa_family=AF_INET6?\b', line):
                        network.append({'file': path.name, 'line': line})
                    if re.match(r'execve(?:at)?\(', line) and re.search(r'= 0\s*$', line):
                        if not line.startswith('execve(' + json.dumps(program) + ','):
                            external_execs.append({'file': path.name, 'line': line})
                    if 'execve("/usr/bin/nvidia-ngx-updater"' in line and '= -1 EACCES' in line:
                        blocked_updaters.append({'file': path.name, 'line': line})
            record['ip_network_attempts'] = network
            record['unexpected_successful_execs'] = external_execs
            record['masked_updater_execs'] = blocked_updaters
            # The fixture deliberately proves five socket denials, with no sends.
            # Match those exact thread IDs/families/types and EPERM returns; any
            # additional vendor/library IP attempt is a separate test failure.
            remaining_probes = list(probes)
            unexpected = []
            family_names = {2: 'AF_INET', 10: 'AF_INET6'}
            type_names = {524289: 'SOCK_STREAM', 524290: 'SOCK_DGRAM'}
            for call in network:
                tid = int(call['file'].rsplit('.', 1)[-1])
                matched = None
                for probe in remaining_probes:
                    if (probe['tid'] == tid and 'socket(' + family_names[probe['family']] + ',' in call['line']
                            and type_names[probe['type']] in call['line'] and 'SOCK_CLOEXEC' in call['line']
                            and re.search(r'= -1 EPERM\b', call['line'])):
                        matched = probe
                        break
                if matched:
                    remaining_probes.remove(matched)
                else:
                    unexpected.append(call)
            record['unexpected_ip_network_attempts'] = unexpected
            if not traces:
                failure = failure or RuntimeError('strace produced no trace files')
            if remaining_probes:
                failure = failure or RuntimeError('expected network-policy probe missing from trace')
            if unexpected:
                failure = failure or RuntimeError('IP network attempt observed; inspect recorded trace')
            if external_execs:
                failure = failure or RuntimeError('unexpected external program executed; inspect recorded trace')
        record['passed'] = not failure and record.get('exit') == 0 and not record.get('timed_out') and not record.get('remaining_owned_pids') and not record.get('killed_owned_pids') and not record.get('cleanup_errors')
        if artifact and record['passed']:
            receipt['artifacts'][str(artifact.relative_to(output))] = digest(artifact)
        if failure:
            record['error'] = str(failure)
        save(); print(name, 'PASS' if record['passed'] else 'FAIL', flush=True)
        if isinstance(failure, (KeyboardInterrupt, SystemExit)):
            raise failure
        if not record['passed']:
            raise RuntimeError(f'{name}: see {output / (name + ".stderr.log")}')
        return record

    save()
    error = None
    interrupted = None
    states = []
    try:
        run('compiler-version', [cxx, '--version'])
        if args.real_device:
            run('bwrap-version', [bwrap, '--version'])
        base = ['-O1', '-UNDEBUG', '-march=x86_64_v2', '-ffp-contract=off', '-fno-strict-aliasing',
                '-fvisibility=hidden', '-pthread', '-Wall', '-Wextra', '-Werror',
                '-I' + str(sysroot / 'usr/include'), '-I' + str(sdk / 'include')]
        guard = output / 'linux_state_guard.o'
        provider = output / 'dlss_linux.o'
        bridge = feature / 'libmonado-ngx.so'
        binary = output / 'test_dlss_linux'
        run('compile-state-guard', [cc, *base, '-c', 'runtime/platform/linux_state_guard.c', '-o', guard], artifact=guard)
        # The vendor headers are third-party and carry compiler-specific constructs;
        # production provider warnings are still enabled, without imposing -Werror.
        provider_flags = [flag for flag in base if flag != '-Werror']
        # No STL object crosses the function table. Compile the loader without
        # bundled libc++ and resolve the archive's GNU ABI against installed libstdc++.
        run('compile-vendor-bridge', [cc, *provider_flags, '-x', 'c++', '-std=c++17', '-shared', '-fPIC', '-nostdlib++',
                                     'runtime/nvn/vulkan/dlss_ngx_loader.cpp', '-x', 'none', archive, gnu_cxx, '-ldl',
                                     '-Wl,--version-script,' + str(exports), '-Wl,--no-undefined', '-o', bridge], artifact=bridge)
        run('module-exports', ['/usr/bin/nm', '-D', '--defined-only', bridge])
        symbols = (output / 'module-exports.stdout.log').read_text().splitlines()
        if len(symbols) != 1 or symbols[0].split()[-1] != 'monado_ngx_api':
            raise RuntimeError('unexpected NGX module public symbols')
        run('module-needed', ['/usr/bin/readelf', '-d', bridge])
        needed = (output / 'module-needed.stdout.log').read_text()
        if 'libstdc++.so.6' not in needed or 'libc++.so' in needed:
            raise RuntimeError('NGX module does not retain isolated GNU C++ runtime ABI')
        run('compile-provider', [cxx, *provider_flags, '-std=c++17', '-c', 'runtime/nvn/vulkan/dlss_linux.cpp', '-o', provider], artifact=provider)
        run('compile-test', [cxx, *base, '-std=c++17', 'tests/native/test_dlss_linux.cpp', provider, guard,
                            vulkan, '-Wl,-rpath,' + str(vulkan.parent), '-ldl', '-o', binary], artifact=binary)
        absent_state = output / 'absent-state'; absent_state.mkdir()
        states.append(absent_state)
        run('absent-runtime', [binary, '--state-root', absent_state, '--feature-dir', absent_state / 'missing-runtime',
                               '--absent-runtime'], trace=args.trace)
        if args.real_device:
            names = {'normal': 'device-state', 'spaces': 'device state', 'quotes': 'device-state-"quote"', 'utf8': 'device-state-雪'}
            live_state = output / ('device-state- "雪"' if args.quoted_state else names[args.state_kind]); live_state.mkdir()
            states.append(live_state)
            argv = [binary, '--state-root', live_state, '--feature-dir', feature]
            if args.ascii_data_alias:
                (live_state / 'ngx-data').mkdir()
                argv.append('--ascii-data-alias')
            if args.capability_only:
                argv.append('--capability-only')
            runtime_environment = {}
            if args.xdg_runtime == 'readonly':
                runtime_dir = output / 'xdg-runtime-readonly'; runtime_dir.mkdir()
                runtime_environment['XDG_RUNTIME_DIR'] = str(runtime_dir)
            elif args.xdg_runtime == 'private':
                (live_state / 'runtime').mkdir()
                runtime_environment['XDG_RUNTIME_DIR'] = '/tmp/runtime'
            record = run('capability' if args.capability_only else 'evaluate', argv, seconds=args.real_seconds,
                         trace=args.trace, namespace_state=live_state, extra_env=runtime_environment)
            if not args.capability_only and not record['metrics']:
                raise RuntimeError('evaluation returned no pixel metrics')
        receipt['state_artifacts'] = {str(path.relative_to(output)): digest(path)
                                     for path in sorted(output.rglob('*.rgba')) if path.is_file()}
        receipt['private_state_files'] = {state.name: file_manifest(state) for state in states}
    except BaseException as failure:
        error = str(failure) or type(failure).__name__
        if isinstance(failure, (KeyboardInterrupt, SystemExit)):
            interrupted = failure
    finally:
        receipt['sources_after'] = source_hashes()
        receipt['sdk_headers_after'] = file_manifest(sdk / 'include')
        receipt['dependencies_after'] = {path: digest(Path(path)) for path in receipt['dependencies']}
        if receipt['sources_after'] != receipt['sources_before']:
            error = (error + '; ' if error else '') + 'authored source changed during run'
        if receipt['sdk_headers_before'] != receipt['sdk_headers_after'] or receipt['dependencies'] != receipt['dependencies_after']:
            error = (error + '; ' if error else '') + 'dependency changed during run'
        receipt['status'] = 'failed' if error else 'passed'
        receipt['error'] = error
        receipt['finished_utc'] = datetime.now(timezone.utc).isoformat()
        save()
    print(receipt['status'], output / 'receipt.json')
    if interrupted:
        raise interrupted
    return 1 if error else 0


if __name__ == '__main__':
    sys.exit(main())
