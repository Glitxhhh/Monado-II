"""Host/native updater capability tests; no GitHub, game, SDL or human profile."""
import importlib.util
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
SUPPORTED = sys.platform == 'linux'
broker_module = None
if SUPPORTED:
    spec = importlib.util.spec_from_file_location('monado_test_launcher_updates', ROOT / 'Monado_Linux/scripts/launcher_updates.py')
    broker_module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(broker_module)


class SessionFixture:
    def __init__(self):
        self.checked = 0
        self.installed = []
        self.activated = []
        self.wait_for_cancel = False
        self.fail_install = False
        self.closed = 0

    def check(self):
        self.checked += 1
        return {'id': 'offer-2', 'version': 'v0.2.0', 'rebuild': True, 'message': 'Authored fixture update'}

    def install(self, offer_id, progress, cancel):
        self.installed.append(offer_id)
        progress({'phase': 'building', 'message': 'Synthetic staging only', 'fraction': .5})
        if self.wait_for_cancel:
            if not cancel.wait(2):
                raise RuntimeError('fixture cancellation was not delivered')
            raise InterruptedError('cancelled')
        if self.fail_install:
            raise RuntimeError('fixture staged-build failure')
        return {'fixture_prepared': True}

    def activate(self, prepared):
        self.activated.append(prepared)
        return ['/authored/never-executed.AppImage']

    def close(self): self.closed += 1


@unittest.skipUnless(SUPPORTED, 'Linux-only inherited Unix capability')
class BrokerTests(unittest.TestCase):
    def setUp(self):
        self.session = SessionFixture()
        self.broker = broker_module.UpdateBroker(self.session, Path(sys.executable).resolve())
        self.broker.gui.setblocking(False)
        self.addCleanup(self.cleanup_broker)
        self.tree = mock.patch.object(broker_module, 'proc_tree', return_value={os.getpid()})
        self.children = mock.patch.object(broker_module, 'proc_children', return_value=set())
        self.tree.start(); self.children_mock = self.children.start()
        self.addCleanup(self.tree.stop); self.addCleanup(self.children.stop)
        self.send('hello', str(os.getpid()))
        self.send('activity', 'idle')

    def cleanup_broker(self):
        self.broker.finish(0)

    def send(self, *fields):
        self.broker.gui.send('\t'.join(['1', *fields]).encode())
        self.broker.poll(os.getpid())

    def drain(self):
        frames = []
        while True:
            try:
                frames.append(self.broker.gui.recv(8193).decode().split('\t'))
            except BlockingIOError:
                return frames

    def complete_worker(self):
        deadline = time.monotonic() + 3
        while self.broker.worker is not None and time.monotonic() < deadline:
            self.broker.poll(os.getpid()); time.sleep(.001)
        self.assertIsNone(self.broker.worker)
        return self.drain()

    def check_offer(self):
        self.send('check')
        frames = self.complete_worker()
        self.assertIn(['1', 'offer', 'offer-2', 'v0.2.0', '1', 'Authored fixture update'], frames)

    def test_check_apply_and_ready_require_observed_exit75(self):
        self.check_offer()
        self.send('apply', 'offer-2')
        frames = self.complete_worker()
        self.assertTrue(any(frame[1] == 'ready' for frame in frames))
        self.assertEqual(self.session.installed, ['offer-2'])
        self.assertFalse(self.session.activated)
        with self.assertRaises(ValueError): self.broker.activate()
        self.assertTrue(self.broker.finish(75))
        self.assertEqual(self.broker.activate(), ['/authored/never-executed.AppImage'])
        self.assertEqual(len(self.session.activated), 1)

    def test_other_gui_exit_cannot_activate_prepared_update(self):
        self.check_offer(); self.send('apply', 'offer-2'); self.complete_worker()
        self.assertFalse(self.broker.finish(0))
        with self.assertRaises(ValueError): self.broker.activate()
        self.assertFalse(self.session.activated)
        self.assertGreater(self.session.closed, 0)

    def test_clean_ready_eof_can_only_activate_with_exit75(self):
        self.check_offer(); self.send('apply', 'offer-2'); self.complete_worker()
        self.broker.gui.close(); self.broker.poll(os.getpid())
        self.assertFalse(self.broker.cancel.is_set())
        self.assertTrue(self.broker.finish(75))

    def test_busy_native_and_actual_descendant_both_reject_apply(self):
        self.check_offer()
        self.send('activity', 'busy'); self.send('apply', 'offer-2')
        self.assertTrue(any(frame[1] == 'error' for frame in self.drain()))
        self.send('activity', 'idle'); self.children_mock.return_value = {12345}
        self.send('apply', 'offer-2')
        self.assertTrue(any(frame[1] == 'error' for frame in self.drain()))
        self.assertFalse(self.session.installed)

    def test_stale_offer_and_defer_never_start_install(self):
        self.check_offer(); self.send('apply', 'stale')
        self.assertTrue(any(frame[1] == 'error' for frame in self.drain()))
        self.send('defer'); self.send('apply', 'offer-2')
        self.assertFalse(self.session.installed)

    def test_adopted_activity_outside_gui_tree_rejects_apply(self):
        self.check_offer()
        self.broker.gui_pid = 54321
        self.children_mock.side_effect = lambda pid: {os.getpid(), 12345} if pid == os.getpid() else set()
        self.send('apply', 'offer-2')
        self.assertTrue(any(frame[1] == 'error' for frame in self.drain()))
        self.assertFalse(self.session.installed)

    def test_failed_install_keeps_activation_unavailable(self):
        self.session.fail_install = True
        self.check_offer(); self.send('apply', 'offer-2')
        self.assertTrue(any(frame[1] == 'error' for frame in self.complete_worker()))
        self.assertIsNone(self.broker.prepared)
        self.assertFalse(self.session.activated)

    def test_cancel_only_stops_update_staging(self):
        self.session.wait_for_cancel = True
        self.check_offer(); self.send('apply', 'offer-2'); self.send('cancel')
        self.assertTrue(any(frame[1] == 'cancelled' for frame in self.complete_worker()))
        self.assertIsNone(self.broker.prepared)
        self.assertFalse(self.session.activated)

    def test_malformed_unknown_and_oversized_frames_close_capability(self):
        for payload in (b'2\tcheck', b'1\texec\t/bin/true', b'1\tcheck\n', b'x' * 8193):
            with self.subTest(payload=payload[:20]):
                other = broker_module.UpdateBroker(SessionFixture(), Path(sys.executable).resolve())
                try:
                    other.gui.send(payload); other.poll(os.getpid())
                    self.assertFalse(other.connected); self.assertTrue(other.cancel.is_set())
                finally: other.finish(0)

    def test_version_marker_is_required_before_loading_updater_code(self):
        with tempfile.TemporaryDirectory(prefix='monado-update-marker-') as temporary:
            root = Path(temporary)
            self.assertIsNone(broker_module.load_session(root, root / 'state', root / 'gui', root / 'game'))
            (root / 'update-version.json').write_text('{"schema":1,"protocol":2}')
            with self.assertRaises(ValueError): broker_module.load_session(root, root / 'state', root / 'gui', root / 'game')


@unittest.skipUnless(SUPPORTED, 'native Linux IPC and state guard')
class NativeIpcTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # CI may use system compilers; developer installs keep Clang local.
        local = ROOT / 'Monado_Linux/.deps/bin'
        cc = os.environ.get('MONADO_TEST_CC') or (str(local / 'clang-cc') if (local / 'clang-cc').is_file() else shutil.which('cc'))
        cxx = os.environ.get('MONADO_TEST_CXX') or (str(local / 'clang-cxx') if (local / 'clang-cxx').is_file() else shutil.which('c++'))
        if not cc or not cxx: raise unittest.SkipTest('installed C/C++ compiler unavailable')
        runs = ROOT / 'Monado_Linux/runs'
        runs_existed = runs.exists()
        runs.mkdir(exist_ok=True)
        if not runs_existed:
            # Class cleanups run LIFO: remove our temporary directory first,
            # then only the empty parent that this test itself created.
            cls.addClassCleanup(runs.rmdir)
        cls.temporary = tempfile.TemporaryDirectory(prefix='linux027-ipc-', dir=runs)
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.output = Path(cls.temporary.name)
        cls.binary = cls.output / 'authored-update-ipc'
        guard = cls.output / 'guard.o'
        for command in ([cc, '-std=gnu11', '-c', str(ROOT / 'Monado_Linux/runtime/platform/linux_state_guard.c'), '-o', str(guard)],
                        [cxx, '-std=c++17', '-O1', '-UNDEBUG', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', str(ROOT / 'tests/native/test_launcher_updates_ipc.cpp'), str(guard), '-o', str(cls.binary)]):
            result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=60)
            if result.returncode: raise AssertionError(result.stderr.decode(errors='replace'))

    def test_native_protocol_busy_cancel_ready_and_exec_fd_isolation(self):
        result = subprocess.run([str(self.binary)], stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr.decode(errors='replace'))
        self.assertIn(b'PASS bounded IPC', result.stdout)

    def run_peer(self, namespace):
        state = self.output / ('namespace-state' if namespace else 'guard-state'); state.mkdir()
        session = SessionFixture(); broker = broker_module.UpdateBroker(session, self.binary)
        command = [str(self.binary), '--peer', str(broker.child_fd()), str(state)]
        if namespace:
            if not Path('/usr/bin/bwrap').is_file():
                broker.finish(0); self.skipTest('bwrap is unavailable')
            command = ['/usr/bin/bwrap', '--unshare-user', '--unshare-net', '--ro-bind', '/', '/', '--dev-bind', '/dev', '/dev',
                       '--ro-bind', '/proc', '/proc', '--bind', str(state), str(state), '--bind', str(state), '/tmp', '--', *command]
        child = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                 pass_fds=(broker.child_fd(),), env={'PATH':'/usr/bin:/bin','LANG':'C.UTF-8'})
        broker.child_started()
        try:
            deadline = time.monotonic() + 10
            while child.poll() is None and time.monotonic() < deadline:
                broker.poll(child.pid); time.sleep(.005)
            if child.poll() is None:
                child.kill(); child.wait(timeout=3); self.fail('authored IPC fixture exceeded its bound')
            stdout, stderr = child.communicate(timeout=3)
            if namespace and child.returncode and b'No permissions to create a new namespace' in stderr:
                self.skipTest('host sandbox denies unprivileged bwrap/user namespaces')
            if child.returncode == 77 and b'Landlock ABI >=3 required' in stderr:
                self.skipTest('host lacks required Landlock ABI >=3')
            self.assertEqual(child.returncode, 75, stderr.decode(errors='replace'))
            self.assertTrue(broker.finish(75))
            self.assertEqual(session.installed, ['offer-2'])
            self.assertFalse(session.activated, 'fixture must not activate or execute any update')
        finally:
            broker.finish(0)
            if child.poll() is None: child.kill(); child.wait(timeout=3)

    def test_native_socket_capability_survives_landlock(self): self.run_peer(False)

    def test_native_socket_capability_survives_private_tmp_network_namespace(self): self.run_peer(True)

    def test_session_supervisor_retains_lease_and_worker_reaps_its_own_child(self):
        # A dedicated authored helper owns subreaper state, not unittest.
        import fcntl
        state = self.output / 'supervisor-state'; state.mkdir()
        started = state / 'worker-started'
        helper = state / 'supervisor-fixture.py'
        helper.write_text(
            'import json,os,subprocess,sys,time\nfrom pathlib import Path\n'
            'sys.path.insert(0,' + repr(str(ROOT / 'Monado_Linux/scripts')) + ')\n'
            'sys.path.insert(0,' + repr(str(ROOT / 'tests')) + ')\n'
            'import launcher_linux as launcher\nfrom launcher_updates import UpdateBroker\n'
            'from test_launcher_updates import SessionFixture\n'
            'class Fixture(SessionFixture):\n'
            ' def install(self, offer_id, progress, cancel):\n'
            '  Path(' + repr(str(started)) + ').write_text("started")\n'
            '  child=subprocess.run([sys.executable,"-B","-c","import time;time.sleep(.2)"],timeout=3)\n'
            '  assert child.returncode==0,"worker status was stolen"\n'
            '  return super().install(offer_id,progress,cancel)\n'
            'state=Path(' + repr(str(state)) + ')\n'
            'session=Fixture()\nbroker=UpdateBroker(session,Path(' + repr(str(self.binary)) + '))\n'
            'fd=launcher.acquire_lock(state)\n'
            'log=os.open(state/"console.log",os.O_WRONLY|os.O_CREAT,0o600)\n'
            'try:\n'
            ' code=launcher.wait_session([' + repr(str(self.binary)) + ',"--peer",str(broker.child_fd()),str(state)],'
            '{"PATH":"/usr/bin:/bin","LANG":"C.UTF-8"},fd,log,updates=broker)\n'
            ' assert code==75 and broker.activation_ready\n'
            ' assert not session.activated,"fixture must not activate"\n'
            ' print("PASS supervisor lease, worker child reaping, and ready handoff")\n'
            'finally:\n broker.finish(0)\n os.close(log)\n os.close(fd)\n')
        process = subprocess.Popen([sys.executable, '-B', str(helper)], stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            deadline = time.monotonic() + 5
            while not started.exists() and process.poll() is None and time.monotonic() < deadline: time.sleep(.005)
            if not started.exists():
                stdout, stderr = process.communicate(timeout=3)
                self.fail((stdout + stderr).decode(errors='replace'))
            lock = os.open(state / '.session.lock', os.O_RDWR)
            try:
                with self.assertRaises(BlockingIOError): fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                stdout, stderr = process.communicate(timeout=5)
                self.assertEqual(process.returncode, 0, stderr.decode(errors='replace'))
                self.assertIn(b'PASS supervisor lease', stdout)
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            finally: os.close(lock)
        finally:
            if process.poll() is None: process.kill(); process.wait(timeout=3)


if __name__ == '__main__': unittest.main()
