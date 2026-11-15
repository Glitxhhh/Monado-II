#!/usr/bin/env python3
"""LINUX-099 host-only Play capability; GUI confinement is never inherited.

One checked native GUI may request the one receipt-bound game/profile selected
by its supervisor. No request carries commands, paths, environment or signals.
The supervisor keeps this broker, profile/runtime leases and metadata alive
until the game exits naturally, including after a GUI close or crash.
"""
import errno
import hashlib
import importlib.util
import os
from pathlib import Path
import socket
import stat
import struct
import subprocess
import sys
import time

MAX_FRAME = 2048
MAX_REQUEST = 2147483647


def digest(path):
    path = Path(path)
    info = path.lstat()
    if path.resolve() != path or not stat.S_ISREG(info.st_mode) or info.st_nlink != 1:
        raise ValueError('game broker source must be an ordinary canonical file')
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def children(pid):
    tasks = Path('/proc') / str(pid) / 'task'
    found = set()
    try:
        entries = list(tasks.iterdir())
    except FileNotFoundError:
        return found
    if len(entries) > 256:
        raise ValueError('game broker process thread limit exceeded')
    for entry in entries:
        try:
            values = (entry / 'children').read_text().split()
        except FileNotFoundError:
            continue
        for value in values:
            number = int(value)
            if number <= 0:
                raise ValueError('invalid game broker child PID')
            found.add(number)
        if len(found) > 128:
            raise ValueError('game broker child limit exceeded')
    return found


def process_tree(pid):
    seen = set()
    pending = [pid]
    while pending:
        current = pending.pop()
        if current in seen:
            continue
        seen.add(current)
        if len(seen) > 128:
            raise ValueError('game broker descendant limit exceeded')
        pending.extend(children(current) - seen)
    return seen


def process_identity(pid):
    try:
        data = (Path('/proc') / str(pid) / 'stat').read_text()
        if len(data) > 8192:
            raise ValueError('oversized game process identity')
        return data.rsplit(')', 1)[1].split()[19]  # kernel starttime, rejects PID reuse
    except FileNotFoundError:
        return None


def clean_text(value):
    return ''.join(c if 32 <= ord(c) != 127 else ' ' for c in str(value))[:1024]


class GameBroker:
    def __init__(self, *, state, launcher, executable, executable_sha256,
                 policy, environment, profile_lock_fd, runtime_fd=None, log_fd=None,
                 controls=None, activity=None):
        self.state = Path(state)
        self.launcher = Path(launcher)
        self.executable = Path(executable)
        self.executable_sha256 = executable_sha256
        self.policy = policy
        self.helper = policy.ROOT / 'scripts/run_linux.py'
        self.helper_sha256 = digest(self.helper)
        self.environment = dict(environment)
        self.profile_lock_fd = profile_lock_fd
        self.runtime_fd = runtime_fd
        self.log_fd = log_fd
        self.controls = dict(controls or {})
        self.activity = activity
        self.host, self.gui = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        try:
            self.host.setsockopt(socket.SOL_SOCKET, socket.SO_PASSCRED, 1)
            self.host.setblocking(False)
        except OSError:
            self.host.close()
            self.gui.close()
            raise
        self.connected = True
        self.gui_pid = None
        self.native_idle = False
        self.last_request = 0
        self.request = None
        self.child = None
        self.seen = False
        self.descendants = {}
        self.outgoing = []
        self.observation_errors = 0
        self.input = None
        self.preparation = None
        self.preparation_helper = policy.ROOT / 'scripts/launcher_fsr4_worker.py'
        self.preparation_sha256 = digest(self.preparation_helper) if self.preparation_helper.is_file() else None
        try:
            self._verify_selection()
        except (ValueError, OSError):
            self.host.close()
            self.gui.close()
            raise

    def _verify_selection(self):
        if digest(self.helper) != self.helper_sha256:
            raise ValueError('game broker helper changed since admission')
        plan = self.policy.plan(self.executable, self.state)
        if (Path(plan['argv'][0]) != self.executable or
                Path(plan['state']) != self.state or
                plan['binary_sha256'] != self.executable_sha256):
            raise ValueError('game broker selection changed since admission')
        source = plan.get('input')
        if self.input is not None and source != self.input:
            raise ValueError('game broker input changed since admission')
        self.input = source
        return plan

    def child_fd(self):
        return self.gui.fileno()

    def child_started(self):
        self.gui.close()

    def _log(self, message):
        if self.log_fd is not None:
            try:
                os.write(self.log_fd, ('[game-broker] ' + clean_text(message) + '\n').encode())
            except OSError:
                pass

    def _send(self, *fields):
        if not self.connected:
            return
        frame = '\t'.join(('1', *(clean_text(value) for value in fields))).encode()
        if len(frame) > MAX_FRAME or len(self.outgoing) >= 16:
            self.disconnect()
            return
        self.outgoing.append(frame)
        self._flush()

    def _flush(self):
        while self.connected and self.outgoing:
            try:
                count = self.host.send(self.outgoing[0])
            except BlockingIOError:
                return
            except OSError:
                self.disconnect()
                return
            if count != len(self.outgoing[0]):
                self.disconnect()
                return
            self.outgoing.pop(0)

    def disconnect(self):
        if self.connected:
            self.connected = False
            self.host.close()
        if self.preparation is not None:
            self.preparation.cancel()
        # A running game retains its own lifetime; optional preparation cancels.

    def _request(self, fields, credential, root_pid):
        if len(fields) < 2 or fields[0] != '1':
            raise ValueError('invalid Play protocol')
        pid, uid, _gid = credential
        if uid != os.getuid():
            raise ValueError('Play peer user mismatch')
        command = fields[1]
        if command == 'hello':
            if (self.gui_pid is not None or len(fields) != 3 or not fields[2].isdigit() or
                    str(int(fields[2])) != fields[2] or pid != int(fields[2]) or
                    pid not in process_tree(root_pid) or
                    (Path('/proc') / str(pid) / 'exe').resolve(strict=True) != self.launcher):
                raise ValueError('Play greeting is not from the verified native GUI')
            self.gui_pid = pid
            self._send('ready', 'Host game launch is available.')
            return
        if self.gui_pid is None or pid != self.gui_pid:
            raise ValueError('verified Play greeting required')
        if command == 'activity' and len(fields) == 3 and fields[2] in ('busy', 'idle'):
            self.native_idle = fields[2] == 'idle'
            return
        if command == 'cancel-fsr4' and len(fields) == 3:
            if self.preparation is None or fields[2] != str(self.preparation.request):
                raise ValueError('invalid optional preparation cancellation')
            self.preparation.cancel()
            return
        if command not in ('play', 'prepare-fsr4') or len(fields) != 3 or not fields[2].isdigit():
            raise ValueError('unsupported Play command')
        request = int(fields[2])
        if str(request) != fields[2] or not self.last_request < request <= MAX_REQUEST:
            raise ValueError('replayed or invalid Play request')
        self.last_request = request
        if (self.child is not None or self.preparation is not None or not self.native_idle or children(self.gui_pid) or
                children(os.getpid()) - {root_pid} or
                (self.activity is not None and self.activity())):
            if command == 'prepare-fsr4':
                self._send('fsr4', request, 'error', 'Close the game or shader preparation and finish updating before preparing models.')
            else:
                self._send('error', request, 'Close the game or shader preparation and finish updating before Play.')
            return
        try:
            self._verify_selection()
            if command == 'prepare-fsr4':
                if not self.preparation_sha256 or digest(self.preparation_helper) != self.preparation_sha256:
                    raise ValueError('optional preparation worker changed since admission')
                receipt = __import__('json').loads((self.executable.parent / 'receipt.json').read_text())
                if receipt.get('fsr4_enabled') is not True:
                    raise ValueError('This build does not contain the optional FSR4 native component.')
                spec = importlib.util.spec_from_file_location('_launcher_fsr4_broker', self.policy.ROOT / 'scripts/launcher_fsr4_broker.py')
                module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
                descriptors = (self.profile_lock_fd,) + ((self.runtime_fd,) if self.runtime_fd is not None else ())
                self.preparation = module.Preparation(root=self.policy.ROOT, state=self.state,
                    environment=self.environment, descriptors=descriptors, send=self._send, request=request,
                    process_tree=process_tree, process_identity=process_identity)
                return
            # Host-selected argv only. No child-specific value comes from IPC.
            argv = [sys.executable, '-B', str(self.helper), '--executable', str(self.executable),
                    '--state-root', str(self.state), '--env', 'XCDE_STRICT=1',
                    '--env', 'XCDE_NO_ERROR_DIALOG=1', '--env', 'XCDE_INI=' + str(self.state / 'xcde.ini')]
            for key, value in sorted(self.controls.items()):
                argv += ['--env', key + '=' + value]
            logs = self.state / 'logs'
            info = logs.lstat()
            if logs.resolve() != logs or not stat.S_ISDIR(info.st_mode):
                raise ValueError('game log directory must be ordinary and canonical')
            log = os.open(logs / 'launcher-game.log', os.O_WRONLY | os.O_APPEND | os.O_CREAT |
                          os.O_CLOEXEC | os.O_NOFOLLOW | os.O_NONBLOCK, 0o600)
            try:
                info = os.fstat(log)
                if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1 or info.st_uid != os.getuid():
                    raise ValueError('game log must be an owned ordinary single-link file')
                descriptors = (self.profile_lock_fd,) + ((self.runtime_fd,) if self.runtime_fd is not None else ())
                self.child = subprocess.Popen(argv, cwd=self.policy.ROOT, env=self.environment,
                    stdin=subprocess.DEVNULL, stdout=log, stderr=log, start_new_session=True,
                    pass_fds=descriptors)
            finally:
                os.close(log)
            self.request = request
            self.seen = False
            self.descendants = {}
            self._send('pending', request, 'Starting game from the checked host supervisor.')
        except (ValueError, OSError, subprocess.SubprocessError) as error:
            if command == 'prepare-fsr4':
                self._send('fsr4', request, 'error', str(error))
            else:
                self._send('error', request, str(error))

    def _poll_child(self):
        if self.child is None:
            return
        code = self.child.poll()
        try:
            if code is None:
                known = process_tree(self.child.pid)
                for pid in known - {self.child.pid}:
                    token = process_identity(pid)
                    if token is not None:
                        self.descendants.setdefault(pid, token)
                if not self.seen:
                    for pid in known:
                        try:
                            match = (Path('/proc') / str(pid) / 'exe').resolve(strict=True) == self.executable
                        except FileNotFoundError:
                            continue
                        if match:
                            self.seen = True
                            self._send('running', self.request, 'Game running. Close its window to return to settings.')
                            break
                return
            # A supervisor may fail between spawn and the first tree sample.
            # The outer subreaper then adopts the native game directly. Recover
            # only the exact fixed ELF, never an updater or arbitrary orphan.
            for pid in children(os.getpid()):
                try:
                    match = (Path('/proc') / str(pid) / 'exe').resolve(strict=True) == self.executable
                except FileNotFoundError:
                    continue
                if match:
                    token = process_identity(pid)
                    if token is not None:
                        self.descendants.setdefault(pid, token)
                        if not self.seen:
                            self.seen = True
                            self._send('running', self.request, 'Game running. Close its window to return to settings.')
            # The outer session is a subreaper. Reap only known game descendants,
            # never the GUI, updater worker or an unrelated child status.
            for pid, token in list(self.descendants.items()):
                if process_identity(pid) != token:
                    self.descendants.pop(pid, None)
                    continue
                try:
                    done, _ = os.waitpid(pid, os.WNOHANG)
                except ChildProcessError:
                    done = 0
                if done or not (Path('/proc') / str(pid)).exists():
                    self.descendants.pop(pid, None)
            if self.descendants:
                return
        except (ValueError, OSError) as error:
            if self.observation_errors < 8:
                self._log('Could not observe game descendants: ' + str(error))
                self.observation_errors += 1
            if code is None:
                return
        request, seen = self.request, self.seen
        self.child = None
        self.request = None
        result = code if code is not None and code >= 0 else 128 - code if code is not None else 1
        if result == 0 and not seen:
            result = 1
        self._send('exit', request, result, 'Game closed. Settings are available.' if result == 0 else
                   'Game stopped. See logs/launcher-game.log and logs/monado.log.')

    def poll(self, root_pid):
        self._poll_child()
        if self.preparation is not None and self.preparation.poll():
            self.preparation = None
        if not self.connected:
            return
        for _ in range(16):
            try:
                frame, ancillary, flags, _ = self.host.recvmsg(MAX_FRAME + 1, socket.CMSG_SPACE(struct.calcsize('3i')))
            except BlockingIOError:
                break
            except OSError:
                self.disconnect()
                break
            try:
                if not frame or len(frame) > MAX_FRAME or flags & (socket.MSG_TRUNC | socket.MSG_CTRUNC):
                    raise ValueError('closed or oversized Play frame')
                credentials = [struct.unpack('3i', data) for level, kind, data in ancillary
                               if level == socket.SOL_SOCKET and kind == socket.SCM_CREDENTIALS and len(data) == struct.calcsize('3i')]
                if len(credentials) != 1:
                    raise ValueError('Play peer credentials required')
                text = frame.decode('utf-8')
                if any((ord(c) < 32 and c != '\t') or ord(c) == 127 for c in text):
                    raise ValueError('invalid Play text')
                self._request(text.split('\t'), credentials[0], root_pid)
            except (ValueError, OSError, UnicodeError) as error:
                self._log('Play connection rejected: ' + str(error))
                self.disconnect()
                break
        self._flush()

    def finish(self):
        self.disconnect()
        self.gui.close()
        while self.child is not None or self.preparation is not None:
            self._poll_child()
            if self.preparation is not None and self.preparation.poll():
                self.preparation = None
            if self.child is not None or self.preparation is not None:
                time.sleep(.03)
