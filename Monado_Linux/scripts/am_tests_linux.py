#!/usr/bin/env python3
"""Synthetic AM foreground/event IPC regression; no game input or graphics."""
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
SOURCES = ['tests/native/test_am_linux.c', 'runtime/platform/linux.c', 'runtime/paths.c',
           'runtime/hle/fibers.c', 'runtime/hle/fiber_context_linux.c',
           'runtime/diag/diag.c', 'runtime/diag/sampler.c']


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def snapshot():
    pending = [ROOT / p for p in [*SOURCES, 'scripts/am_tests_linux.py']]
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
    parser.add_argument('--idle-baseline-only', action='store_true', help='preserve cmd62 failure before implementing idle policy')
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
        output = Path(tempfile.mkdtemp(prefix='linux-am.', dir=runs))
    env = {'PATH': '/usr/bin:/bin', 'LANG': 'C.UTF-8', 'LC_ALL': 'C.UTF-8',
           'TMPDIR': str(output), 'XCDE_STRICT': '1'}
    compiler = ROOT / '.deps/bin/clang-cc'
    sysroot = Path((ROOT / '.deps/graphics/active-sysroot.txt').read_text().strip())
    libdir = sysroot / 'usr/lib/x86_64-linux-gnu'
    sdl = libdir / 'libSDL3.so'
    receipt = {'scope': 'synthetic actual AM/IPC/kernel foreground/event contract; no game/GPU/saves',
               'status': 'running', 'host': list(platform.uname()), 'cwd': str(ROOT),
               'argv': sys.argv, 'sources_before': snapshot(), 'commands': [], 'artifacts': {},
               'compiler': str(compiler), 'compiler_sha256': digest(compiler),
               'sysroot': str(sysroot), 'sdl_sha256': digest(sdl),
               'limitations': ['No process-suspension operation exists; count remains zero',
                               'Policies/notification flags are retained for the active-only model; no fabricated HOME/sleep/mode/resume transitions',
                               'Idle62/63 retain per-boot enum state; existing SDL lifetime inhibition request is unchanged, not verified desktop/OS suspend prevention',
                               'SDL events adapter uses SDL_INIT_EVENTS only, with no window/GPU/input hardware',
                               'Test-originated kernel signal is not evidence of applet suspension',
                               'Reset hooks/kernel reset tested after actual stop/join in both orders; current host has no live reboot dispatcher',
                               'Unrelated guest dispatch/settings/graphics callbacks are synthetic fixture adapters']}

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
        binary = output / 'test_am_linux'
        flags = ['-O1', '-UNDEBUG', '-march=x86_64_v2', '-ffp-contract=off', '-fno-strict-aliasing',
                 '-pthread', '-Iruntime', '-I' + str(sysroot / 'usr/include')]
        # The fixture includes the registry's sources in one translation unit;
        # retain its used ELF sections under the Zig driver's default GC.
        run('compile', [compiler, *flags, *SOURCES, '-Wl,--no-gc-sections', '-rdynamic', str(sdl), '-Wl,-rpath,' + str(libdir),
                        '-ldl', '-lm', '-o', binary], artifact=binary)
        run('idle-set-baseline', [binary, '--idle-set'], contains=('idle setter baseline request returned',))
        if args.idle_baseline_only:
            receipt['sources_after'] = snapshot()
            if receipt['sources_after'] != receipt['sources_before']:
                raise RuntimeError('source changed during tests')
            receipt['status'] = 'passed'
            return 0
        run('domain-payload-size', [binary, '--domain-size-probe'], contains=('domain payload size OK',))
        run('timer-signal', [binary, '--timer-signal'], mode='1',
            contains=('timer signal grants baton after callback OK',))
        for mode in ('0', '1'):
            run('blocked-close-' + mode, [binary, '--blocked-close'], mode=mode,
                contains=('guest blocked in WaitSynchronization; host close signal now',
                          'blocked ExitRequest wake/cleanup OK'))
            run('shutdown-close-' + mode, [binary, '--shutdown-close'], mode=mode,
                contains=('renderer drain follows guest stop/join',
                          'direct GPU/cache/process finish follows renderer drain',
                          'AM-ready live guest stop/join -> render drain -> direct finish OK'))
            run('shutdown-renderer-wait-' + mode, [binary, '--shutdown-renderer-wait'], mode=mode,
                contains=('render worker completed queued work while UI joined guest',
                          'renderer completion drained/joined after guest safe-point exit',
                          'AM-ready renderer-wait guest stop/join -> real completion drain -> direct finish OK'))
            run('shutdown-exit-old-order-' + mode, [binary, '--shutdown-exit-old-order'], mode=mode,
                expected=-signal.SIGABRT, contains=('shutdown_stage == 2',))
            run('early-close-' + mode, [binary, '--early-close'], mode=mode,
                contains=('early native close before IPC initialization OK',))
        for mode in ('0', '1'):
            run('events-' + mode, [binary], mode=mode,
                contains=('concurrent idle policy set/query OK', 'foreground acquisition/focus/queue invariants', '8 reset cycles OK'))
            for argument in ('--idle-range', '--idle-domain-range', '--idle-negative'):
                run(argument[2:] + '-' + mode, [binary, argument], mode=mode,
                    expected=-signal.SIGABRT, contains=('invalid idle-time extension',))
            malformed_idle = ['--idle-short', '--idle-domain-short', '--idle-extra-data', '--idle-get-extra-data']
            for extra in range(1, 9):
                domain = '-domain' if extra == 8 else ''
                malformed_idle += [f'--idle{domain}-extra-{extra}', f'--idle{domain}-get-extra-{extra}']
            for argument in malformed_idle:
                run(argument[2:] + '-' + mode, [binary, argument], mode=mode,
                    expected=-signal.SIGABRT, contains=('malformed idle-time cmd',))
            for kind, argument, diagnostic in (
                    ('self', '--strict-negative', 'ISelfController cmd 2147483647'),
                    ('window', '--strict-window-negative', 'IWindowController cmd 2147483647')):
                run('strict-' + kind + '-' + mode, [binary, argument], mode=mode,
                    expected=-signal.SIGABRT,
                    contains=('STRICT MODE: stopping at ipc [am] ' + diagnostic,))
            for argument, diagnostic in (
                    ('--malformed-boolean', 'flag 2 is not boolean'),
                    ('--malformed-short', 'payload bytes, need 3'),
                    ('--malformed-domain-short', 'payload bytes, need 3'),
                    ('--malformed-domain-bounds', 'invalid domain payload bounds')):
                run(argument[2:] + '-' + mode, [binary, argument], mode=mode,
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
