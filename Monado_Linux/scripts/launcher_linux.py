#!/usr/bin/env python3
"""[XCDE] LINUX-009: verified native graphical launcher with private state.

Normal invocation has no automatic stop or scripted input. --dry-run performs
read-only identity/input/path checks. Test controls require explicit arguments.
"""
import argparse
import ctypes
import fcntl
import json
import math
import os
from pathlib import Path
import re
import signal
import stat
import subprocess
import sys

import run_linux

ROOT = Path(__file__).resolve().parents[1]
TEST_KEYS = frozenset(('XCDE_SHOTS', 'XCDE_EXIT_AFTER_SHOTS', 'LARECOMP_NO_USER_INPUT',
                      'LARECOMP_AUDIO_GAIN', 'XCDE_USAGE', 'XCDE_HLE_COVERAGE',
                      'XCDE_STALL', 'XCDE_PRESENT_STATS', 'XCDE_TRACE_PRESENT', 'XCDE_DUP_CHECK'))


def ordinary_path(path):
    """Reject symlink components and lexical parent traversal, even if dangling."""
    path = path.absolute()
    if '..' in path.parts or path.resolve() != path:
        raise ValueError('path must be canonical without symlinks or parent traversal: ' + str(path))
    current = Path(path.anchor)
    for part in path.parts[1:]:
        current /= part
        try:
            info = current.lstat()
        except FileNotFoundError:
            continue
        if stat.S_ISLNK(info.st_mode):
            raise ValueError('symlink path is not allowed: ' + str(current))
        if current != path and not stat.S_ISDIR(info.st_mode):
            raise ValueError('path parent is not a directory: ' + str(current))
    return path


def validate_tree(directory, depth=0):
    """Inspect without following links, before any state/log/config writes."""
    if depth > 128:
        raise ValueError('state directory nesting exceeds 128 levels')
    with os.scandir(directory) as entries:
        for entry in entries:
            info = entry.stat(follow_symlinks=False)
            if stat.S_ISDIR(info.st_mode):
                fd = os.open(entry.name, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC,
                             dir_fd=directory)
                try:
                    validate_tree(fd, depth + 1)
                finally:
                    os.close(fd)
            elif not stat.S_ISREG(info.st_mode) or info.st_nlink != 1:
                raise ValueError('state contains a symlink, special or multiply-linked file: ' + entry.name)


def validate_state(state, source):
    state = ordinary_path(state)
    if len(os.fsencode(state)) >= 1024:
        raise ValueError('state root exceeds native path limit')
    if not any(state != ROOT / name and state.is_relative_to(ROOT / name) for name in ('play', 'runs')):
        raise ValueError('state must be a child of Monado_Linux/play or runs')
    if state == source or state.is_relative_to(source) or source.is_relative_to(state):
        raise ValueError('input and state roots must be disjoint')
    if state.exists():
        fd = os.open(state, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC)
        try:
            validate_tree(fd)
        finally:
            os.close(fd)
    return state


def state_file(path, state, *, required=False):
    path = ordinary_path(path if path.is_absolute() else state / path)
    if path == state or not path.is_relative_to(state):
        raise ValueError('test files and outputs must stay inside state: ' + str(path))
    if path.exists():
        info = path.lstat()
        if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1:
            raise ValueError('test path must be an ordinary single-link file: ' + str(path))
    elif required:
        raise ValueError('test input file is missing: ' + str(path))
    return path


def test_controls(entries, state):
    controls = {}
    for entry in entries:
        key, separator, value = entry.partition('=')
        if not separator or key not in TEST_KEYS or key in controls or not value:
            raise ValueError('invalid or duplicate test child control: ' + key)
        if key in ('XCDE_USAGE', 'XCDE_HLE_COVERAGE'):
            state_file(Path(value), state)
        elif key == 'XCDE_SHOTS':
            shots = value.split(',')
            if len(shots) > 16:
                raise ValueError('at most 16 test captures are supported')
            for shot in shots:
                frame, colon, filename = shot.partition(':')
                if not colon or not frame.isdecimal() or not 1 <= int(frame) <= 1000000 or not filename:
                    raise ValueError('XCDE_SHOTS requires frame:path pairs')
                state_file(Path(filename), state)
        elif key == 'LARECOMP_AUDIO_GAIN':
            number = float(value)
            if not math.isfinite(number) or not 0 <= number <= 2:
                raise ValueError('test audio gain must be finite and in 0..2')
        elif key == 'XCDE_STALL':
            if not value.isdecimal() or not 1 <= int(value) <= 3600:
                raise ValueError('test stall interval must be in 1..3600')
        elif key == 'XCDE_TRACE_PRESENT':
            match = re.fullmatch(r'(\d+):(\d+)', value)
            if not match or not 0 <= int(match[1]) <= int(match[2]) <= 1000000:
                raise ValueError('present trace requires an ordered bounded frame range')
        elif value != '1':
            raise ValueError('test boolean controls require exactly 1')
        controls[key] = value
    return controls


def clean_environment(library_path):
    environment = {'PATH': '/usr/bin:/bin', 'LANG': 'C.UTF-8', 'LC_ALL': 'C.UTF-8',
                   'LD_LIBRARY_PATH': library_path,
                   'SDL_APP_ID': 'monado-linux-port'}
    for key in ('HOME', 'USER', 'LOGNAME', 'DISPLAY', 'WAYLAND_DISPLAY', 'XAUTHORITY',
                'XDG_RUNTIME_DIR', 'XDG_SESSION_TYPE', 'DBUS_SESSION_BUS_ADDRESS',
                'PULSE_SERVER', 'PULSE_COOKIE', 'PULSE_RUNTIME_PATH', 'PIPEWIRE_REMOTE',
                'PIPEWIRE_RUNTIME_DIR', 'TZ'):
        if key in os.environ:
            environment[key] = os.environ[key]
    return environment


def prepare(args):
    launcher = run_linux.plan(args.launcher_executable, args.state_root)
    game = run_linux.plan(args.game_executable, args.state_root)
    gui_receipt = json.loads((Path(launcher['argv'][0]).parent / 'receipt.json').read_text())
    game_receipt = json.loads((Path(game['argv'][0]).parent / 'receipt.json').read_text())
    if gui_receipt.get('artifact_kind') != 'linux-launcher':
        raise ValueError('launcher executable must have a linux-launcher build receipt')
    if game_receipt.get('artifact_kind') == 'linux-launcher' or not game_receipt.get('generated'):
        raise ValueError('game executable must have a complete generated-game build receipt')
    if launcher['input'] != game['input']:
        raise ValueError('launcher and game must select the same immutable input')
    state = validate_state(Path(game['state']), Path(game['input']))
    if (args.test_child_env or args.test_exit_after_child) and not args.ui_script:
        raise ValueError('test child controls require --ui-script')
    if (args.shot_tab is not None or args.shot_frame is not None) and not args.shot:
        raise ValueError('--shot-tab and --shot-frame require --shot')
    controls = test_controls(args.test_child_env, state)
    argv = [launcher['argv'][0], '--project-root', str(ROOT), '--input-root', game['input'],
            '--game-executable', game['argv'][0], '--state-root', str(state)]
    if args.shot:
        argv += ['--shot', str(state_file(args.shot, state)),
                 '--shot-tab', str(args.shot_tab if args.shot_tab is not None else 0),
                 '--shot-frame', str(args.shot_frame if args.shot_frame is not None else 5)]
    if args.ui_script:
        argv += ['--ui-script', str(state_file(args.ui_script, state, required=True))]
    if args.test_exit_after_child:
        argv.append('--test-exit-after-child')
    for key, value in controls.items():
        argv += ['--test-child-env', key + '=' + value]
    launch = {'argv': argv, 'cwd': str(ROOT), 'state': str(state), 'input': game['input'],
            'launcher_sha256': launcher['binary_sha256'], 'game_sha256': game['binary_sha256'],
            'library_path': launcher['library_path'], 'game_library_path': game['library_path'],
            'test_controls': controls}
    # [XCDE] LINUX-017: bwrap must run BEFORE native GUI Landlock restricts
    # /proc uid-map writes. Preserve the inherited lock OFD into the GUI; an
    # unused pass_fds descriptor is retained by the tested installed bwrap.
    # No --sync-fd/--die-with-parent/PID namespace: existing GUI and Python
    # holders retain the lease and a GUI crash must not kill a running game.
    if game_receipt.get('dlss_enabled', False):
        if run_linux.private_tmp_alias(state):
            launch['namespace'] = {'kind': 'inherited-private-dlss', 'private_tmp_alias': True}
        else:
            launch['execution_argv'], launch['namespace'] = run_linux.dlss_namespace(argv, state, pid_namespace=False)
        launch['runtime_environment'] = run_linux.private_runtime_environment(
            clean_environment(launcher['library_path']), state,
            planned_namespace='execution_argv' in launch)
    return launch


def check_input(source):
    check = subprocess.run([str(ROOT / '.venv/bin/python'), '-B', str(ROOT / 'tools/extract/check_dump.py'), source],
                           cwd=ROOT, capture_output=True, text=True, timeout=120,
                           env={'PATH': '/usr/bin:/bin', 'LANG': 'C.UTF-8', 'LC_ALL': 'C.UTF-8'})
    if check.returncode:
        raise ValueError('selected game input check failed: ' + check.stdout + check.stderr)
    return check.stdout.strip()


def mkdir_private(path):
    """Create/open directories through nofollow dirfds; never replace anything."""
    fd = os.open('/', os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC)
    try:
        for part in path.parts[1:]:
            try:
                os.mkdir(part, 0o700, dir_fd=fd)
            except FileExistsError:
                pass
            next_fd = os.open(part, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC, dir_fd=fd)
            os.close(fd); fd = next_fd
        return fd
    except BaseException:
        os.close(fd)
        raise


def regular_open(directory, name, flags):
    fd = os.open(name, flags | os.O_CREAT | os.O_CLOEXEC | os.O_NOFOLLOW | os.O_NONBLOCK, 0o600, dir_fd=directory)
    info = os.fstat(fd)
    if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1:
        os.close(fd)
        raise ValueError(name + ' must be an ordinary single-link file')
    return fd


def acquire_lock(state):
    state_fd = mkdir_private(state)
    lock_fd = None
    try:
        # Check again after opening/creating, before adding the lock/log files.
        validate_tree(state_fd)
        lock_fd = regular_open(state_fd, '.session.lock', os.O_RDWR)
        try:
            fcntl.flock(lock_fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise ValueError('Monado launcher or game is already using this state directory') from None
        return lock_fd
    except BaseException:
        if lock_fd is not None:
            os.close(lock_fd)
        raise
    finally:
        os.close(state_fd)


def wait_session(argv, environment, lock_fd, log_fd):
    """Keep the lock until the GUI and adopted descendants exit naturally.

    The GUI retains an inherited lock as protection if this wrapper dies. The
    wrapper is a subreaper so unexpected GUI death does not release the lock
    while its game remains. This does not kill the game on a launcher close.
    """
    children = Path('/proc/self/task') / str(os.getpid()) / 'children'
    if children.read_text().strip():
        raise RuntimeError('launcher wrapper must not own unrelated children')
    libc = ctypes.CDLL(None, use_errno=True)
    previous = ctypes.c_int()
    if libc.prctl(37, ctypes.byref(previous), 0, 0, 0) or libc.prctl(36, 1, 0, 0, 0):
        raise OSError(ctypes.get_errno(), 'cannot enable launcher child subreaper')
    interrupted = []
    def keep_game(number, frame):
        interrupted.append(number)
        # No exception: keep the lock and wait/reap the owned session naturally.
        try:
            os.write(2, b'Monado session remains active; close its window to finish safely.\n')
        except OSError:
            pass
    handlers = {number: signal.signal(number, keep_game) for number in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP)}
    child = None
    try:
        child = subprocess.Popen([*argv, '--lock-fd', str(lock_fd)], cwd=ROOT, env=environment,
                                 stdin=subprocess.DEVNULL, stdout=log_fd, stderr=log_fd,
                                 start_new_session=True, pass_fds=(lock_fd,))
        code = child.wait()
        while True:
            try:
                os.waitpid(-1, 0)
            except InterruptedError:
                continue
            except ChildProcessError:
                break
        return code if code else 128 + interrupted[0] if interrupted else 0
    finally:
        # There is deliberately no kill/terminate path. The child inherits the
        # lock if this wrapper is forcibly terminated or cannot continue.
        for number, handler in handlers.items():
            signal.signal(number, handler)
        if libc.prctl(36, previous.value, 0, 0, 0):
            raise OSError(ctypes.get_errno(), 'cannot restore child subreaper')


def arguments(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--launcher-executable', type=Path, default=ROOT / 'build/linux-20261107-023-experimental-gui/monado-launcher')
    parser.add_argument('--game-executable', type=Path, default=ROOT / 'build/linux-20261107-023-experimental/xcde-linux')
    parser.add_argument('--state-root', type=Path, default=ROOT / 'play/launcher')
    parser.add_argument('--dry-run', action='store_true')
    parser.add_argument('--shot', type=Path)
    parser.add_argument('--shot-tab', type=int, choices=range(4))
    parser.add_argument('--shot-frame', type=int, choices=range(1, 10001), metavar='1..10000')
    parser.add_argument('--ui-script', type=Path)
    parser.add_argument('--test-exit-after-child', action='store_true')
    parser.add_argument('--test-child-env', action='append', default=[], metavar='NAME=VALUE')
    return parser, parser.parse_args(argv)


def main(argv=None):
    parser, args = arguments(argv)
    lock_fd = log_fd = None
    try:
        launch = prepare(args)
        launch['input_check'] = check_input(launch['input'])
        if args.dry_run:
            print(json.dumps(launch, indent=2))
            return 0
        os.umask(0o077)
        state = Path(launch['state'])
        lock_fd = acquire_lock(state)
        logs_fd = mkdir_private(state / 'logs')
        try:
            log_fd = regular_open(logs_fd, 'launcher-console.log', os.O_WRONLY | os.O_APPEND)
        finally:
            os.close(logs_fd)
        # [XCDE] LINUX-013: retain the verified selection for manual-run evidence,
        # including when the GUI has already closed before logs are inspected.
        selection = {'game_executable': launch['argv'][launch['argv'].index('--game-executable') + 1],
                     'game_sha256': launch['game_sha256']}
        with os.fdopen(os.dup(log_fd), 'a', encoding='utf-8') as log:
            log.write('[launcher-selection] ' + json.dumps(selection, sort_keys=True) + '\n')
        if 'namespace' in launch:
            run_linux.prepare_namespace_state(launch)
        return wait_session(launch.get('execution_argv', launch['argv']),
                            launch.get('runtime_environment', clean_environment(launch['library_path'])), lock_fd, log_fd)
    except (ValueError, OSError, RuntimeError, subprocess.SubprocessError) as error:
        parser.exit(1, str(error) + '\n')
    finally:
        if log_fd is not None:
            os.close(log_fd)
        if lock_fd is not None:
            os.close(lock_fd)


if __name__ == '__main__':
    sys.exit(main())
