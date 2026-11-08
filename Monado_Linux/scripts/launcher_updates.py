#!/usr/bin/env python3
"""[XCDE] LINUX-027: host-only update broker; native GUI has no network access.

The session supervisor owns the profile lease and all child reaping. Workers may
check or stage a release; activation is a separate, post-session operation.
"""
import importlib.util
import json
import math
import os
from pathlib import Path
import queue
import re
import socket
import stat
import sys
import threading

MAX_FRAME = 8192
IDENTIFIER = re.compile(r'[A-Za-z0-9_.-]{1,128}\Z')


def text_field(value, limit=1024):
    return ''.join(c if ord(c) >= 32 and ord(c) != 127 else ' ' for c in str(value))[:limit]


def proc_children(pid):
    """Read only the known GUI's descendants; an unreadable tree fails closed."""
    children = set()
    tasks = Path('/proc') / str(pid) / 'task'
    entries = list(tasks.iterdir())
    if len(entries) > 256:
        raise ValueError('GUI thread limit exceeded')
    for entry in entries:
        for item in (entry / 'children').read_text().split():
            child = int(item)
            if child <= 0:
                raise ValueError('invalid descendant PID')
            children.add(child)
    if len(children) > 128:
        raise ValueError('GUI descendant limit exceeded')
    return children


def proc_tree(pid):
    pending = [pid]
    seen = set()
    while pending:
        current = pending.pop()
        if current in seen:
            continue
        seen.add(current)
        if len(seen) > 128:
            raise ValueError('session descendant limit exceeded')
        pending.extend(proc_children(current) - seen)
    return seen


def load_session(workspace, state, launcher, game):
    """Load only this install's authored core after validating its opt-in marker."""
    marker = workspace / 'update-version.json'
    if not marker.exists():
        return None
    info = marker.lstat()
    if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1 or info.st_size > 16384:
        raise ValueError('update version marker must be a bounded ordinary file')
    version = json.loads(marker.read_text())
    if not isinstance(version, dict) or type(version.get('schema')) is not int or type(version.get('protocol')) is not int or version['schema'] != 1 or version['protocol'] != 1:
        raise ValueError('unsupported update protocol')
    path = workspace / 'tools/update_linux.py'
    if path.resolve() != path or not path.is_file():
        raise ValueError('update core must be an ordinary local source file')
    name = '_monado_install_update_core'
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module.Session(workspace=workspace, state=state, launcher=launcher, game=game)


class UpdateBroker:
    """One private socket capability and at most one bounded worker at a time."""
    def __init__(self, session, launcher):
        self.session = session
        self.launcher = Path(launcher)
        self.host, self.gui = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        self.host.setblocking(False)
        self.cancel = threading.Event()
        self.events = queue.Queue(maxsize=32)
        self.worker = None
        self.kind = None
        self.offer = None
        self.prepared = None
        self.gui_pid = None
        self.native_idle = False
        self.connected = True
        self.outgoing = []
        self._last_progress = None
        self.activation_ready = False

    @classmethod
    def from_workspace(cls, workspace, state, launcher, game):
        session = load_session(Path(workspace), Path(state), Path(launcher), Path(game))
        return cls(session, launcher) if session is not None else None

    def child_fd(self):
        return self.gui.fileno()

    def child_started(self):
        self.gui.close()

    def _send(self, *fields):
        if not self.connected:
            return
        data = '\t'.join(['1', *(text_field(field) for field in fields)]).encode('utf-8')
        if len(data) > MAX_FRAME:
            self.disconnect()
            return
        # Progress is replaceable; completion and offers must keep their order.
        if fields[0] == 'progress':
            self.outgoing = [frame for frame in self.outgoing if not frame.startswith(b'1\tprogress\t')]
        if len(self.outgoing) >= 32:
            self.disconnect()
            return
        self.outgoing.append(data)
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

    def _progress(self, event):
        # Never block the updater's compiler/stdout reader on a slow GUI.
        if not isinstance(event, dict):
            return
        phase = text_field(event.get('phase', 'working'), 128)
        if not IDENTIFIER.fullmatch(phase):
            phase = 'working'
        fraction = event.get('fraction', -1)
        if isinstance(fraction, bool) or not isinstance(fraction, (int, float)) or not math.isfinite(fraction) or not (-1 == fraction or 0 <= fraction <= 1):
            fraction = -1
        self._last_progress = (phase, fraction, text_field(event.get('message', 'Working...')))

    def _start(self, kind):
        if self.worker is not None:
            self._send('error', 'Another update operation is already running.')
            return
        self.cancel.clear()
        self.kind = kind

        def work():
            try:
                value = self.session.check() if kind == 'check' else self.session.install(self.offer['id'], self._progress, self.cancel)
                self.events.put(('finished', kind, value))
            except BaseException as error:
                self.events.put(('failed', kind, text_field(error)))
        self.worker = threading.Thread(target=work, name='monado-update-' + kind)
        self.worker.start()

    def _hello(self, fields, root_pid):
        if self.gui_pid is not None or len(fields) != 3 or not fields[2].isdigit():
            raise ValueError('invalid update greeting')
        pid = int(fields[2])
        if pid <= 0 or pid not in proc_tree(root_pid) or (Path('/proc') / str(pid) / 'exe').resolve(strict=True) != self.launcher:
            raise ValueError('update greeting is not from the verified native GUI')
        self.gui_pid = pid

    def _request(self, fields, root_pid):
        if len(fields) < 2 or fields[0] != '1':
            raise ValueError('invalid update protocol')
        command = fields[1]
        if command == 'hello':
            self._hello(fields, root_pid)
            return
        if self.gui_pid is None:
            raise ValueError('update greeting required')
        if command == 'activity' and len(fields) == 3 and fields[2] in ('idle', 'busy'):
            self.native_idle = fields[2] == 'idle'
        elif command == 'check' and len(fields) == 2:
            if self.worker is None and self.prepared is None:
                self._send('state', 'checking', 'Checking for a newer stable release...')
                self._start('check')
        elif command == 'apply' and len(fields) == 3:
            if self.worker is not None or self.prepared is not None or not self.offer or fields[2] != self.offer['id']:
                self._send('error', 'That update offer is no longer available.')
            elif (not self.native_idle or proc_children(self.gui_pid) or
                  proc_children(os.getpid()) - {root_pid}):
                # An intermediate game/helper can die before its child. Such
                # activity is adopted by the outer subreaper, no longer by GUI.
                # No update worker exists at admission, so any extra direct
                # supervisor child is outstanding activity, never updater work.
                self._send('error', 'Close the game or shader preparation before updating.')
            else:
                self._start('install')
        elif command == 'defer' and len(fields) == 2 and self.worker is None:
            self.offer = None
        elif command == 'cancel' and len(fields) == 2 and (self.kind == 'install' or self.prepared is not None):
            self.cancel.set()
            if self.worker is None:
                self.prepared = None
                self._close_staging()
                self._send('cancelled', 'Update cancelled. The current version is still usable.')
        else:
            raise ValueError('unknown or unavailable update request')

    def poll(self, root_pid):
        for _ in range(16):
            if not self.connected:
                break
            try:
                frame = self.host.recv(MAX_FRAME + 1)
            except BlockingIOError:
                break
            except OSError:
                self.disconnect()
                break
            if not frame or len(frame) > MAX_FRAME:
                self.disconnect(clean_eof=not frame)
                break
            try:
                text = frame.decode('utf-8')
                if any((ord(c) < 32 and c != '\t') or ord(c) == 127 for c in text):
                    raise ValueError('invalid update text')
                self._request(text.split('\t'), root_pid)
            except (ValueError, OSError):
                self.disconnect()
        progress, self._last_progress = self._last_progress, None
        if progress is not None and self.kind == 'install' and self.prepared is None:
            self._send('progress', *progress)
        while True:
            try:
                status, kind, value = self.events.get_nowait()
            except queue.Empty:
                break
            self.worker.join()
            self.worker = None
            self.kind = None
            self._last_progress = None
            if self.cancel.is_set() or not self.connected:
                self.prepared = None
                self._close_staging()
                self._send('cancelled', 'Update cancelled. The current version is still usable.')
            elif status == 'failed':
                self._close_staging()
                self._send('error', 'Update failed: ' + value)
            elif kind == 'check':
                if value is None:
                    self.offer = None
                    self._send('uptodate', 'This is the latest compatible stable release.')
                elif not isinstance(value, dict) or not IDENTIFIER.fullmatch(str(value.get('id', ''))) or not IDENTIFIER.fullmatch(str(value.get('version', ''))) or not isinstance(value.get('rebuild'), bool):
                    self._send('error', 'The update service returned an invalid release offer.')
                else:
                    self.offer = value
                    self._send('offer', value['id'], value['version'], int(value['rebuild']), value.get('message', 'A newer stable release is available.'))
            elif isinstance(value, dict):
                self.prepared = value
                self._send('ready', 'Update ready. Restarting the launcher...')
            else:
                self._close_staging()
                self._send('error', 'The update did not produce a verified prepared installation.')
        self._flush()

    def disconnect(self, *, clean_eof=False):
        self.connected = False
        # A ready GUI intentionally closes this FD when exiting 75. The final
        # observed exit code, not EOF alone, authorizes its restart transaction.
        if not clean_eof or self.prepared is None:
            self.cancel.set()
        self.outgoing.clear()
        try:
            self.host.close()
        except OSError:
            pass

    def finish(self, gui_code, root_pid=None):
        """Join workers before the supervisor's generic adopted-child reap loop."""
        if gui_code != 75 or self.prepared is None:
            self.cancel.set()
        if self.worker is not None:
            self.worker.join()
            self.worker = None
        self.gui.close()
        self.host.close()
        ready = gui_code == 75 and self.prepared is not None and not self.cancel.is_set()
        self.activation_ready = ready
        if not ready:
            self._close_staging()
        return ready

    def _close_staging(self):
        close = getattr(self.session, 'close', None)
        if close is not None:
            close()

    def activate(self):
        if not self.activation_ready or self.prepared is None or self.cancel.is_set():
            raise ValueError('no completed update is ready for activation')
        argv = self.session.activate(self.prepared)
        if not isinstance(argv, list) or not argv or any(not isinstance(value, str) or '\0' in value for value in argv) or not Path(argv[0]).is_absolute():
            raise ValueError('update activation did not return an absolute launcher command')
        self.activation_ready = False
        self.prepared = None
        return argv
