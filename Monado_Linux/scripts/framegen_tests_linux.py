#!/usr/bin/env python3
"""[XCDE] LINUX-020: bounded authored offscreen Vulkan/FG components; no SDL/game/human state.

Without --real-device only compile the fixture. --real-device without a native
SDK library qualifies resource uploads/readback and queried capabilities only;
it never claims that image copies are frame interpolation or WSI pacing.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import re
import resource
import shutil
import sys

from boot_smoke_linux import bounded_process
import run_linux

ROOT = Path(__file__).resolve().parent.parent


def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def sources(provider=False):
    names = ('scripts/framegen_tests_linux.py', 'tests/native/test_framegen_linux.cpp',
             'runtime/platform/linux_state_guard.c', 'runtime/platform/linux_state_guard.h',
             'scripts/boot_smoke_linux.py', 'scripts/platform_tests_linux.py', 'scripts/run_linux.py')
    result = {name: digest(ROOT / name) for name in names}
    if provider:
        for name in ('runtime/nvn/vulkan/framegen_linux.cpp', 'runtime/nvn/vulkan/framegen_linux.h'):
            result[name] = digest(ROOT / name)
        for path in sorted((ROOT / 'third_party/ffx_api').rglob('*.h')):
            result[str(path.relative_to(ROOT))] = digest(path)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--real-device', action='store_true', help='run actual authored GPU resource/readback components')
    parser.add_argument('--trace', action='store_true', help='trace real GPU process/file/network calls')
    parser.add_argument('--headless-wsi-probe', action='store_true', help='query actual selected-device headless surface support; no window or visible fallback')
    parser.add_argument('--provider', action='store_true', help='also compile the actual native FidelityFX wrapper')
    parser.add_argument('--sdk-library', type=Path, help='verified native project-local SDK module; actual interpolation requires --provider --real-device')
    parser.add_argument('--profile', action='store_true', help='also measure same-binary FG off/on/offscreen GPU stages with authored 720p/1440p images; no WSI/frame pacing claim')
    parser.add_argument('--size', action='append', choices=('720p', '1440p'), help='profile selected output sizes; default profiles both (depth/motion are two-thirds resolution)')
    args = parser.parse_args()
    if sys.platform != 'linux' or platform.machine() != 'x86_64':
        parser.error('native Linux x86-64 required')
    if args.trace and (not args.real_device or not shutil.which('strace')):
        parser.error('--trace requires --real-device and installed strace')
    if args.headless_wsi_probe and not args.real_device:
        parser.error('--headless-wsi-probe requires --real-device')
    if args.sdk_library and (not args.provider or not args.real_device):
        parser.error('--sdk-library requires --provider --real-device')
    if args.profile and not args.sdk_library:
        parser.error('--profile requires --sdk-library --provider --real-device')
    if args.size and (not args.profile or len(set(args.size)) != len(args.size)):
        parser.error('--size requires --profile and each size may be selected only once')
    if args.sdk_library:
        args.sdk_library = args.sdk_library.absolute()
        if (args.sdk_library != args.sdk_library.resolve() or not args.sdk_library.is_file()
                or not args.sdk_library.is_relative_to(ROOT / '.deps/fsr')
                or args.sdk_library.stat().st_nlink != 1):
            parser.error('--sdk-library must be an ordinary single-link module below project .deps/fsr')
    out = args.output.absolute()
    if out != out.resolve() or out.exists() or not out.is_relative_to(ROOT / 'runs'):
        parser.error('--output must be a fresh ordinary directory below Linux runs/')
    if args.real_device and not run_linux.BWRAP.is_file():
        parser.error('GPU isolation requires installed bwrap')
    os.umask(0o077)
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    out.mkdir(parents=True)
    sysroot = Path((ROOT / '.deps/graphics/active-sysroot.txt').read_text().strip())
    library = sysroot / 'usr/lib/x86_64-linux-gnu'
    cc, cxx = ROOT / '.deps/bin/clang-cc', ROOT / '.deps/bin/clang-cxx'
    env = {'PATH': '/usr/bin:/bin', 'LANG': 'C.UTF-8', 'LC_ALL': 'C.UTF-8',
           'TMPDIR': str(out), 'PYTHONDONTWRITEBYTECODE': '1', 'LD_LIBRARY_PATH': str(library)}
    receipt = {'schema': 1, 'scope': __doc__, 'started_utc': datetime.now(timezone.utc).isoformat(),
               'status': 'running', 'argv': sys.argv, 'cwd': str(ROOT), 'host': list(platform.uname()),
               'sources_before': sources(args.provider), 'dependencies': {str(p): digest(p) for p in (cc, cxx, (library / 'libvulkan.so').resolve())},
               'commands': [], 'artifacts': {}, 'real_device': args.real_device,
               'limitations': ['Resource-only mode does not perform interpolation, swapchain presentation or pacing.',
                               'No game input, SDL/window, guest, human configuration or saves are accessed.']}
    if args.sdk_library:
        receipt['scope'] = 'Actual native SDK offscreen frame interpolation on authored GPU images; no game/window/physical display pacing'
        receipt['limitations'] = ['Authored scenes do not establish game temporal/HUD quality.',
                                  'Headless support query does not establish physical WSI presentation or pacing.']
    if args.profile:
        receipt['profile'] = {'sizes': args.size or ['720p', '1440p'], 'order': ['off', 'on', 'on', 'off'],
                              'warmup_per_batch': 3, 'measured_per_batch': 8,
                              'effect_scope': 'GPU SDK recording versus a bit-exact real-image copy, not the complete game presentation path',
                              'cpu_scope': 'command recording and submit/fence wait are separate from authored pattern, cached readback snapshot and pixel-oracle costs; oracle time includes snapshot time'}
        receipt['limitations'] += ['Offscreen single-submission/fence timings do not measure physical presentation, latency, throughput or game FPS.',
                                   'Full input/output readbacks are timed separately; they are fixture correctness costs, not the normal game path.']

    def save():
        (out / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')

    def run(name, argv, *, seconds=60, state=None):
        argv = list(map(str, argv))
        command_env = dict(env)
        trace_root = None
        if state:
            if args.trace:
                trace_root = state / 'trace'
                trace_root.mkdir()
                argv = ['/usr/bin/strace', '-ff', '-qq', '-s', '256', '--kill-on-exit',
                        '-e', 'trace=%file,%process,socket,connect,bind,sendto,sendmsg',
                        '-e', 'raw=sendto,sendmsg', '-o', str(trace_root / name), *argv]
            argv, namespace = run_linux.dlss_namespace(argv, state)
            argv.insert(1, '--die-with-parent')
            run_linux.prepare_namespace_state({'state': str(state), 'namespace': namespace})
            command_env = run_linux.private_runtime_environment(command_env, state, planned_namespace=True)
        record = {'name': name, 'argv': argv, 'cwd': str(ROOT), 'environment': command_env, 'seconds_limit': seconds}
        receipt['commands'].append(record)
        save()
        completion = {}
        failure = None
        try:
            bounded_process(argv, cwd=ROOT, env=command_env, seconds=seconds, completion=completion)
        except BaseException as error:
            failure = error
        record.update(completion.get('record', {}))
        stdout, stderr = completion.get('stdout', b''), completion.get('stderr', b'')
        (out / (name + '.stdout.log')).write_bytes(stdout)
        (out / (name + '.stderr.log')).write_bytes(stderr)
        record['metrics'] = [json.loads(line.split(' ', 1)[1]) for line in stdout.decode(errors='replace').splitlines()
                             if line.startswith(('CAPABILITIES ', 'RESOURCE ', 'FRAMEGEN ', 'HEADLESS ', 'PROCS ',
                                                 'PROFILE ', 'PROFILE_SUMMARY ', 'PROFILE_CAPABILITIES '))]
        if trace_root:
            traces = sorted(trace_root.glob(name + '.*'))
            record['traces'] = {str(p.relative_to(out)): digest(p) for p in traces}
            ip = []
            external = []
            for path in traces:
                for line in path.read_text(errors='replace').splitlines():
                    if re.search(r'socket\(AF_(?:INET6?|PACKET)\b|sa_family=AF_INET6?\b', line):
                        ip.append({'file': path.name, 'line': line})
                    if re.match(r'execve\(', line) and line.rstrip().endswith('= 0') and str(out / 'test_framegen_linux') not in line:
                        external.append({'file': path.name, 'line': line})
            record['unexpected_ip_network'] = ip
            record['unexpected_successful_exec'] = external
            if ip or external:
                failure = failure or RuntimeError('unexpected network/exec observed')
        record['passed'] = failure is None and record.get('exit') == 0 and not record.get('timed_out') and not record.get('killed_owned_pids') and not record.get('remaining_owned_pids')
        save()
        if not record['passed']:
            raise RuntimeError(f'{name} failed: {failure}; see bounded command logs')
        print(name, 'PASS', flush=True)
        return stdout

    save()
    try:
        run('compiler-version', [cxx, '--version'])
        base = ['-O1', '-UNDEBUG', '-march=x86_64_v2', '-ffp-contract=off', '-fno-strict-aliasing',
                '-fvisibility=hidden', '-pthread', '-Wall', '-Wextra', '-Werror', '-I' + str(sysroot / 'usr/include')]
        guard = out / 'linux_state_guard.o'
        run('compile-state-guard', [cc, *base, '-c', ROOT / 'runtime/platform/linux_state_guard.c', '-o', guard])
        binary = out / 'test_framegen_linux'
        provider = []
        if args.provider:
            obj = out / 'framegen_linux.o'
            run('compile-provider', [cxx, *base, '-std=c++17', '-I' + str(ROOT / 'third_party'), '-c', ROOT / 'runtime/nvn/vulkan/framegen_linux.cpp', '-o', obj])
            provider = ['-DFG_LINUX_PROVIDER=1', obj]
        run('compile-fixture', [cxx, *base, '-std=c++17', ROOT / 'tests/native/test_framegen_linux.cpp', guard, *provider,
                               library / 'libvulkan.so', '-Wl,-rpath,' + str(library), '-ldl', '-o', binary])
        receipt['artifacts'][binary.name] = digest(binary)
        if args.real_device:
            state = out / 'resource-state'
            state.mkdir()
            argv = [binary, '--state-root', state, '--resource-probe']
            if args.headless_wsi_probe:
                argv.append('--headless-wsi-probe')
            if args.sdk_library:
                feature = out / 'feature'
                feature.mkdir()
                companion = feature / 'libmonado-fidelityfx.so'
                shutil.copyfile(args.sdk_library, companion)
                if digest(companion) != digest(args.sdk_library):
                    raise RuntimeError('copied SDK module hash mismatch')
                receipt['dependencies'][str(args.sdk_library)] = digest(args.sdk_library)
                receipt['artifacts'][str(companion.relative_to(out))] = digest(companion)
                argv += ['--sdk-library', companion]
            if args.profile:
                argv.append('--profile')
                for size in args.size or []:
                    argv += ['--size', size]
            stdout = run('resource-probe', argv, state=state, seconds=180 if args.profile else 60)
            if b'NOT frame interpolation or WSI/pacing qualification' not in stdout:
                raise RuntimeError('resource probe did not report its actual coverage')
            if args.sdk_library:
                if b'PASS actual SDK offscreen interpolation/static HUD/reset/gap/re-enable/recorded-resize' not in stdout:
                    raise RuntimeError('actual SDK interpolation/lifetime suite did not finish')
                generated = {m['scenario'] for m in receipt['commands'][-1]['metrics']
                             if 'scenario' in m and m.get('result') == 2}
                if generated != {'moving-hud', 'static-hud', 'moving-bgra-hud', 'reset-gap-reenable'}:
                    raise RuntimeError('actual generated-output scenario coverage missing')
            if args.profile:
                metrics = receipt['commands'][-1]['metrics']
                summaries = [m for m in metrics if 'effect_gpu_p50_ms' in m]
                rows = [m for m in metrics if 'effect_gpu_ms' in m]
                expected = {'720p': (1280, 720), '1440p': (2560, 1440)}
                receipt['profile_comparison'] = []
                for size in args.size or ['720p', '1440p']:
                    width, height = expected[size]
                    batches = sorted((m for m in summaries if (m['width'], m['height']) == (width, height)), key=lambda m: m['batch'])
                    if len(batches) != 4 or [m['enabled'] for m in batches] != [False, True, True, False] or any(m['samples'] != 8 for m in batches):
                        raise RuntimeError('profile ABBA repetition coverage missing')
                    selected = [m for m in rows if (m['width'], m['height']) == (width, height)]
                    if len(selected) != 44 or sum(m['sample'] for m in selected) != 32:
                        raise RuntimeError('profile measured/warmup coverage missing')
                    for row in selected:
                        if row['render_width'] != width * 2 // 3 or row['render_height'] != height * 2 // 3 or row.get('inputs_unchanged') is not True:
                            raise RuntimeError('profile render-input or immutable-input contract failed')
                        for key in ('upload_gpu_ms', 'effect_gpu_ms', 'readback_gpu_ms', 'total_gpu_ms', 'submit_wait_cpu_ms', 'record_cpu_ms',
                                    'pattern_cpu_ms', 'snapshot_cpu_ms', 'oracle_cpu_ms'):
                            if not isinstance(row.get(key), (int, float)) or not math.isfinite(row[key]) or row[key] < 0:
                                raise RuntimeError('invalid profile timing')
                        if row['sample'] and (row['result'] != (2 if row['enabled'] else 0) or row['hud_bad_pixels']):
                            raise RuntimeError('profile measured frame did not satisfy its output oracle')
                    off = [batches[i]['effect_gpu_p50_ms'] for i in (0, 3)]
                    on = [batches[i]['effect_gpu_p50_ms'] for i in (1, 2)]
                    receipt['profile_comparison'].append({'size': size, 'off_effect_p50_ms': off, 'on_effect_p50_ms': on,
                        'off_repeat_span_ms': abs(off[0] - off[1]), 'on_repeat_span_ms': abs(on[0] - on[1]),
                        'mean_effect_delta_ms': sum(on) / 2 - sum(off) / 2})
        receipt['sources_after'] = sources(args.provider)
        if receipt['sources_before'] != receipt['sources_after']:
            raise RuntimeError('source changed during component tests')
        receipt['status'] = 'passed'
    except BaseException as error:
        receipt.update(status='failed', error=str(error))
        raise
    finally:
        receipt['finished_utc'] = datetime.now(timezone.utc).isoformat()
        save()
    print(out / 'receipt.json')


if __name__ == '__main__':
    main()
