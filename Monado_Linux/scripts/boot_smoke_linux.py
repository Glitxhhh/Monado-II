#!/usr/bin/env python3
"""[XCDE] LINUX-007: bounded strict Linux boot, fresh private state and local evidence.

This runs the real game. It never imports saves or a reference shader seed.
Exit status alone is not a gameplay/save pass; inspect the receipt, state log,
captures and syscall traces. An external wall bound also covers pre-frame hangs.
"""
import argparse
import ctypes
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import resource
import signal
import subprocess
import sys
import time

from platform_tests_linux import owned_children, kill_owned, reap_owned
from run_linux import plan

ROOT = Path(__file__).resolve().parent.parent


class BootInterrupted(RuntimeError):
    """Do not use InterruptedError: selectors treats that as retryable EINTR."""


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def input_metadata(root):
    # No file contents or atimes: reads may update atime, but must not change
    # content/permissions or the selected tree. Landlock supplies write denial.
    entries = []
    for path in sorted([root, *root.rglob('*')]):
        stat = path.lstat()
        entries.append([str(path.relative_to(root)), stat.st_mode, stat.st_size,
                        stat.st_mtime_ns, stat.st_ctime_ns, stat.st_nlink])
    return {'entries': len(entries), 'sha256': hashlib.sha256(json.dumps(entries).encode()).hexdigest()}


def bounded_process(argv, *, cwd, env, seconds, on_start=None, completion=None):
    """Own/reap descendants; optionally retain the final result even on error."""
    if ctypes.CDLL(None, use_errno=True).prctl(36, 1, 0, 0, 0):  # PR_SET_CHILD_SUBREAPER
        raise OSError(ctypes.get_errno(), 'cannot enable child subreaper')
    if owned_children():
        raise RuntimeError('bounded runner must not own unrelated child processes')
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    started = time.monotonic()
    parent_pid = os.getpid()
    def child_lifetime():
        # If this supervisor is killed before cleanup, kill the direct child.
        # The traced boot additionally uses strace --kill-on-exit for all tracees.
        if ctypes.CDLL(None, use_errno=True).prctl(1, signal.SIGKILL, 0, 0, 0):
            os._exit(126)  # PR_SET_PDEATHSIG
        if os.getppid() != parent_pid:
            os._exit(126)  # parent died before prctl was installed
    pending_signal = None
    def interrupted(number, frame):
        nonlocal pending_signal
        # [XCDE] LINUX-007: communicate can be between os.read and retaining
        # those bytes. Raising here loses that read even after a cleanup drain.
        if pending_signal is None:
            pending_signal = number
    def check_interruption():
        if pending_signal == signal.SIGINT:
            raise KeyboardInterrupt
        if pending_signal is not None:
            raise BootInterrupted(f'boot supervisor interrupted by signal {pending_signal}')
    old_handlers = {number: signal.signal(number, interrupted) for number in
                    (signal.SIGTERM, signal.SIGHUP, signal.SIGINT)}
    child = None
    stdout = stderr = b''
    failure = None
    record = {'wall_limit_seconds': seconds, 'timed_out': False, 'killed_owned_pids': []}
    try:
        child = subprocess.Popen(argv, cwd=cwd, env=env, stdin=subprocess.DEVNULL,
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE, start_new_session=True,
                                 preexec_fn=child_lifetime)
        record['pid'] = child.pid
        if on_start:
            on_start(record)
        deadline = started + seconds
        while True:
            check_interruption()
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                record['timed_out'] = True
                break  # the common cleanup kills, drains and reaps the child
            try:
                # Timeout retries retain cumulative pipe output internally;
                # never concatenate TimeoutExpired.output between these polls.
                stdout, stderr = child.communicate(timeout=min(0.1, remaining))
            except subprocess.TimeoutExpired:
                continue
            check_interruption()
            break
    except BaseException as error:
        failure = error
    finally:
        for number in old_handlers:
            signal.signal(number, signal.SIG_IGN)
        # Also cover delivery between the last poll checkpoint and cleanup.
        # Once handlers are ignored, draining cannot lose bytes to an exception.
        if failure is None and pending_signal is not None:
            try:
                check_interruption()
            except BaseException as error:
                failure = error
                record['timed_out'] = False
        try:
            # [XCDE] LINUX-017: namespace helpers may already have exited when
            # adopted. Reap those before deciding that cleanup must kill them.
            # Never steal Popen's direct-child wait status (even in an exit race).
            for pid in sorted(owned_children() - ({child.pid} if child else set())):
                try:
                    done, status = os.waitpid(pid, os.WNOHANG)
                except (ChildProcessError, ProcessLookupError):
                    continue
                if done:
                    record.setdefault('reaped_descendants', []).append({'pid': done, 'wait_status': status})
            if owned_children():
                record['killed_owned_pids'] += kill_owned()
            if child:
                try:
                    # Also drain the pipes after callback errors or interruption.
                    stdout, stderr = child.communicate(timeout=5)
                except BaseException as error:
                    if isinstance(error, subprocess.TimeoutExpired):
                        stdout, stderr = error.output or stdout, error.stderr or stderr
                    record.setdefault('cleanup_errors', []).append(str(error))
                    if failure is None:
                        failure = error
            deadline = time.monotonic() + 5
            while owned_children() and time.monotonic() < deadline:
                record.setdefault('reaped_descendants', []).extend(reap_owned())
                if owned_children():
                    record['killed_owned_pids'] += kill_owned()
                    time.sleep(0.01)
            record.setdefault('reaped_descendants', []).extend(reap_owned())
        except BaseException as error:
            record.setdefault('cleanup_errors', []).append(str(error))
            if failure is None:
                failure = error
        finally:
            record.update(exit=child.returncode if child else None, seconds=time.monotonic() - started,
                          remaining_owned_pids=sorted(owned_children()))
            if record['remaining_owned_pids']:
                error = RuntimeError('owned descendants remain after cleanup')
                record.setdefault('cleanup_errors', []).append(str(error))
                if failure is None:
                    failure = error
            if completion is not None:
                completion.update(record=record, stdout=stdout, stderr=stderr)
            for number, handler in old_handlers.items():
                signal.signal(number, handler)
    if failure is not None:
        raise failure.with_traceback(failure.__traceback__)
    return record, stdout, stderr


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--executable', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True, help='absent directory beneath Linux runs/')
    parser.add_argument('--seconds', type=int, choices=range(1, 181), metavar='1..180', default=90)
    parser.add_argument('--trace', action='store_true', help='use installed strace, including child threads/processes')
    args = parser.parse_args()
    output = args.output.absolute()
    if output.resolve() != output or not output.is_relative_to(ROOT / 'runs') or output.exists():
        parser.error('output must be a new ordinary directory under Monado_Linux/runs')
    state = output / 'state'
    launch = plan(args.executable, state)
    output.mkdir(parents=True, exist_ok=False)
    controls = {
        'XCDE_STRICT': '1', 'XCDE_LIST_IMPORTS': '1', 'XCDE_HLE_COVERAGE': 'logs/hle.tsv',
        'XCDE_USAGE': 'logs/usage.tsv', 'XCDE_STALL': '10', 'LARECOMP_NO_USER_INPUT': '1',
        'LARECOMP_WINDOW': '960x540', 'LARECOMP_AUDIO_GAIN': '0',
        'XCDE_SHOTS': '30:build/boot-030.png,120:build/boot-120.png',
        'XCDE_EXIT_AFTER_SHOTS': '1',
        'XCDE_NO_ERROR_DIALOG': '1',
    }
    argv = [sys.executable, '-B', str(ROOT / 'scripts/run_linux.py'), '--executable', launch['argv'][0],
            '--state-root', str(state)]
    for key, value in controls.items():
        argv += ['--env', key + '=' + value]
    if args.trace:
        argv = ['/usr/bin/strace', '--kill-on-exit', '-ff', '-ttt', '-s', '256', '-yy',
                '-e', 'trace=%file,%process,socket,connect,bind,sendto,sendmsg',
                '-e', 'raw=sendto,sendmsg', '-o', str(output / 'syscalls'), *argv]
    # Keep only desktop/device session settings, with no inherited test controls,
    # loader injection, credentials or network proxy configuration.
    environment = {'PATH': '/usr/bin:/bin', 'LANG': 'C.UTF-8', 'LC_ALL': 'C.UTF-8'}
    for key in ('HOME', 'USER', 'LOGNAME', 'DISPLAY', 'WAYLAND_DISPLAY', 'XAUTHORITY',
                'XDG_RUNTIME_DIR', 'XDG_SESSION_TYPE', 'DBUS_SESSION_BUS_ADDRESS',
                'PULSE_SERVER', 'PULSE_COOKIE', 'PIPEWIRE_REMOTE', 'TZ'):
        if key in os.environ:
            environment[key] = os.environ[key]
    receipt = {'scope': 'bounded neutral real-time strict boot; no save import or gameplay claim',
               'status': 'prepared', 'started_utc': datetime.now(timezone.utc).isoformat(),
               'argv': argv, 'cwd': str(ROOT), 'env': environment, 'controls': controls, 'launch': launch,
               'sources': {name: digest(ROOT / name) for name in
                           ('scripts/boot_smoke_linux.py', 'scripts/run_linux.py', 'scripts/platform_tests_linux.py')},
               'input_before': input_metadata(Path(launch['input'])),
               'build_receipt_sha256': digest(Path(launch['argv'][0]).parent / 'receipt.json')}

    def save():
        temporary = output / 'receipt.json.tmp'
        temporary.write_text(json.dumps(receipt, indent=2) + '\n')
        temporary.replace(output / 'receipt.json')

    def started(record):
        receipt.update(status='running', process=record)
        save()
        print(f"Bounded boot pid {record['pid']}; state {state}", flush=True)

    save()
    completion = {}
    failure = None
    try:
        record, _, _ = bounded_process(argv, cwd=ROOT, env=environment, seconds=args.seconds,
                                       on_start=started, completion=completion)
        receipt['process'] = record
        receipt['status'] = 'timed_out' if record['timed_out'] else 'exited'
    except BaseException as error:
        failure = error
        receipt.update(status='runner_error', error=str(error), error_type=type(error).__name__)

    # The child has been cleaned up before evidence collection, on either path.
    # A secondary evidence failure must not replace the original interruption.
    def capture(name, operation):
        nonlocal failure
        try:
            operation()
        except BaseException as error:
            receipt.setdefault('evidence_errors', {})[name] = str(error)
            if failure is None:
                failure = error
                receipt.update(status='runner_error', error=str(error), error_type=type(error).__name__)

    if 'record' in completion:
        receipt['process'] = completion['record']
    for stream in ('stdout', 'stderr'):
        capture('early-' + stream, lambda stream=stream:
                (output / ('early-' + stream + '.log')).write_bytes(completion.get(stream, b'')))

    def capture_input():
        receipt['input_after'] = input_metadata(Path(launch['input']))
        receipt['input_metadata_unchanged'] = receipt['input_before'] == receipt['input_after']
        if not receipt['input_metadata_unchanged']:
            raise RuntimeError('shared input metadata changed during boot; stop and investigate')

    def capture_state():
        receipt['state_files'] = {str(p.relative_to(state)): {'bytes': p.stat().st_size, 'sha256': digest(p)}
                                  for p in sorted(state.rglob('*')) if p.is_file() and not p.is_symlink()}
        receipt['expected_captures_present'] = all((state / 'build' / name).is_file()
                                                    for name in ('boot-030.png', 'boot-120.png'))

    capture('input_after', capture_input)
    capture('state_files', capture_state)
    receipt['note'] = 'Inspect logs/captures: process exit and capture existence alone are not a gameplay pass.'
    receipt['finished_utc'] = datetime.now(timezone.utc).isoformat()
    capture('receipt', save)
    if failure is not None:
        raise failure.with_traceback(failure.__traceback__)
    print(f"Boot {receipt['status']}, exit {record['exit']}; receipt {output / 'receipt.json'}", flush=True)
    return 0 if record['exit'] == 0 and not record['timed_out'] else 1


if __name__ == '__main__':
    sys.exit(main())
