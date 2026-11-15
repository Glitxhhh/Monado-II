#!/usr/bin/env python3
"""Nonblocking host-owned FSR4 preparation lifetime and progress relay."""
import hashlib
import json
import os
from pathlib import Path
import signal
import stat
import subprocess
import sys
import time


class Preparation:
    def __init__(self, *, root, state, environment, descriptors, send, request, process_tree, process_identity):
        self.root, self.state = Path(root), Path(state)
        self.send, self.request = send, request
        self.cancelled = False
        self.cancel_at = None
        self.buffer = b''
        self.message = 'Preparing optional FSR4 models. See logs/launcher-fsr4.log for details.'
        self.seen = {}
        self.process_tree, self.process_identity = process_tree, process_identity
        self.worker = self.root / 'scripts/launcher_fsr4_worker.py'
        info = self.worker.lstat()
        if self.worker.resolve() != self.worker or not stat.S_ISREG(info.st_mode) or info.st_nlink != 1:
            raise ValueError('FSR4 worker must be an ordinary authored file')
        logs = self.state / 'logs'
        if logs.resolve() != logs or not stat.S_ISDIR(logs.lstat().st_mode):
            raise ValueError('FSR4 log directory must be ordinary and canonical')
        log = logs / 'launcher-fsr4.log'
        self.log = os.open(log, os.O_WRONLY | os.O_APPEND | os.O_CREAT | os.O_CLOEXEC | os.O_NOFOLLOW | os.O_NONBLOCK, 0o600)
        try:
            info = os.fstat(self.log)
            if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1 or info.st_uid != os.getuid():
                raise ValueError('FSR4 log must be an owned ordinary single-link file')
            self.process = subprocess.Popen([sys.executable, '-B', str(self.worker)], cwd=self.root,
                env=environment, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                start_new_session=True, pass_fds=tuple(descriptors))
            os.set_blocking(self.process.stdout.fileno(), False)
        except BaseException:
            os.close(self.log); raise
        self.send('fsr4', request, 'pending', 'Checking installed Proton and optional FSR4 tools.')

    def cancel(self):
        if self.cancel_at is None:
            self.cancelled = True; self.cancel_at = time.monotonic()
            # Let the worker unwind its own private Proton prefix first.
            if self.process.poll() is None:
                self.process.send_signal(signal.SIGTERM)
            self.send('fsr4', self.request, 'running', 'Cancelling optional preparation and cleaning its private processes...')

    def _kill_owned(self):
        for pid, token in list(self.seen.items()):
            if self.process_identity(pid) != token:
                self.seen.pop(pid, None); continue
            try:
                fd = os.pidfd_open(pid)
                try:
                    if self.process_identity(pid) == token:
                        signal.pidfd_send_signal(fd, signal.SIGKILL)
                finally:
                    os.close(fd)
            except ProcessLookupError:
                pass

    def poll(self):
        if self.process.poll() is None:
            for pid in self.process_tree(self.process.pid) - {self.process.pid}:
                token = self.process_identity(pid)
                if token is not None: self.seen.setdefault(pid, token)
        # Only bounded messages originating from this fixed worker are relayed.
        for _ in range(16):
            try:
                data = os.read(self.process.stdout.fileno(), 8192)
            except BlockingIOError:
                break
            if not data:
                break
            os.write(self.log, data)
            self.buffer += data
            if len(self.buffer) > 65536:
                self.buffer = self.buffer[-65536:]
            while b'\n' in self.buffer:
                line, self.buffer = self.buffer.split(b'\n', 1)
                if line.startswith(b'MONADO_FSR4_PROGRESS ') and len(line) < 2048:
                    try:
                        row = json.loads(line.split(b' ', 1)[1])
                        message = str(row['message'])[:400]
                        current, total = row['current'], row['total']
                        if type(current) is not int or type(total) is not int or not 0 <= current <= max(total, 0) <= 10000:
                            raise ValueError('invalid measured progress')
                        if total: message += f' ({current}/{total})'
                        self.message = message
                        self.send('fsr4', self.request, 'running', message)
                    except (ValueError, KeyError, TypeError):
                        pass
                elif line and not line.startswith((b'{', b' ', b'}')):
                    self.message = line.decode(errors='replace')[-512:]
        if self.cancel_at is not None and time.monotonic() - self.cancel_at > 20 and self.process.poll() is None:
            # The worker and its non-session build tools share this fresh group.
            # Nested Proton sessions are cleaned by the worker's finally path.
            self._kill_owned()
            self.process.kill()
        code = self.process.poll()
        if code is None:
            return False
        # A failed/killed worker must not leave a recorder/compiler running.
        # PID start times and pidfds prevent signalling recycled/unrelated PIDs.
        self._kill_owned()
        for pid, token in list(self.seen.items()):
            if self.process_identity(pid) != token:
                self.seen.pop(pid, None); continue
            try:
                done, _ = os.waitpid(pid, os.WNOHANG)
            except ChildProcessError:
                done = 0
            if done: self.seen.pop(pid, None)
        if self.seen:
            return False
        self.process.stdout.close(); os.close(self.log)
        if self.cancelled or code == 130:
            self.send('fsr4', self.request, 'cancelled', 'Optional FSR4 preparation cancelled. You can retry; other settings are unchanged.')
        elif code:
            self.send('fsr4', self.request, 'error', self.message or 'Preparation failed. See logs/launcher-fsr4.log.')
        else:
            self.send('fsr4', self.request, 'ready', 'FSR4 Lite models are ready. The selected upscaler has not changed.')
        return True
