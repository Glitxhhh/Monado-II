"""[XCDE] LINUX-007: supervisor interruption/parent-death containment, synthetic only."""
import ctypes
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import textwrap
import time
import unittest

from scripts.platform_tests_linux import owned_children, kill_owned, reap_owned

ROOT = Path(__file__).resolve().parents[2]


@unittest.skipUnless(sys.platform == 'linux', 'Linux process lifetime controls')
class BootSignalTests(unittest.TestCase):
    def run_lifetime_control(self, interruption, *, grandchild, traced=False):
        # This independent controller adopts/reaps workers even if the supervisor
        # under test is killed or defective. Never signal an unrelated process.
        libc = ctypes.CDLL(None, use_errno=True)
        previous = ctypes.c_int()
        self.assertEqual(libc.prctl(37, ctypes.byref(previous), 0, 0, 0), 0)
        self.assertEqual(owned_children(), set())
        self.assertEqual(libc.prctl(36, 1, 0, 0, 0), 0)
        supervisor = None
        try:
            with tempfile.TemporaryDirectory(prefix='monado-boot-lifetime-') as temporary:
                directory = Path(temporary)
                worker_source = textwrap.dedent('''
                    import os, pathlib, sys, time
                    directory = pathlib.Path(sys.argv[1])
                    if sys.argv[2] == 'yes':
                        grandchild = os.fork()
                        if grandchild == 0:
                            os.setsid()
                            (directory / 'grandchild.pid').write_text(str(os.getpid()))
                            time.sleep(60)
                            os._exit(0)
                    (directory / 'child.pid').write_text(str(os.getpid()))
                    time.sleep(60)
                ''')
                worker = directory / 'worker.py'
                worker.write_text(worker_source)
                command = [sys.executable, '-B', str(worker), str(directory), 'yes' if grandchild else 'no']
                if traced:
                    command = [shutil.which('strace'), '--kill-on-exit', '-ff', '-e', 'trace=%process',
                               '-o', str(directory / 'trace'), *command]
                supervisor_source = textwrap.dedent('''
                    import json, pathlib, signal, sys
                    sys.path.insert(0, sys.argv[1])
                    from boot_smoke_linux import bounded_process, owned_children
                    directory = pathlib.Path(sys.argv[2])
                    command = json.loads(sys.argv[3])
                    before = {sig: signal.getsignal(sig) for sig in (signal.SIGTERM, signal.SIGHUP, signal.SIGINT)}
                    outcome = {}
                    try:
                        process, stdout, stderr = bounded_process(command, cwd=str(directory), env={'PATH':'/usr/bin:/bin'}, seconds=30,
                            on_start=lambda record: (directory / 'started.json').write_text(json.dumps(record)))
                        outcome.update(process=process, stdout=stdout.decode(), stderr=stderr.decode())
                    except BaseException as error:
                        outcome['error'] = str(error)
                        outcome['error_type'] = type(error).__name__
                    outcome['handlers_restored'] = all(signal.getsignal(sig) == handler for sig, handler in before.items())
                    outcome['remaining'] = sorted(owned_children())
                    (directory / 'finished.json').write_text(json.dumps(outcome))
                ''')
                supervisor = subprocess.Popen([sys.executable, '-B', '-c', supervisor_source,
                                               str(ROOT / 'scripts'), str(directory), json.dumps(command)],
                                              stdout=subprocess.PIPE, stderr=subprocess.PIPE, start_new_session=True)
                required = ['started.json', 'child.pid'] + (['grandchild.pid'] if grandchild else [])
                deadline = time.monotonic() + 5
                while not all((directory / name).exists() for name in required) and time.monotonic() < deadline:
                    if supervisor.poll() is not None:
                        break
                    time.sleep(0.01)
                missing = [name for name in required if not (directory / name).exists()]
                finished = directory / 'finished.json'
                diagnosis = finished.read_text() if finished.exists() else 'supervisor still running'
                self.assertFalse(missing, f'workers not ready: {missing}; {diagnosis}')
                tracked = [int((directory / name).read_text()) for name in required if name.endswith('.pid')]
                os.kill(supervisor.pid, interruption)
                stdout, stderr = supervisor.communicate(timeout=7)
                if interruption == signal.SIGKILL:
                    self.assertEqual(supervisor.returncode, -signal.SIGKILL, stderr.decode())
                else:
                    self.assertEqual(supervisor.returncode, 0, stderr.decode())
                    outcome = json.loads((directory / 'finished.json').read_text())
                    if interruption == signal.SIGINT:
                        self.assertEqual(outcome['error_type'], 'KeyboardInterrupt')
                    else:
                        self.assertEqual(outcome['error_type'], 'BootInterrupted')
                        self.assertIn(f'signal {interruption}', outcome['error'])
                    self.assertTrue(outcome['handlers_restored'])
                    self.assertEqual(outcome['remaining'], [])
                # Observe death/reaping before the independent cleanup runs. A
                # positive result cannot be supplied by this controller's kill.
                deadline = time.monotonic() + 3
                while owned_children() and time.monotonic() < deadline:
                    reap_owned()
                    time.sleep(0.01)
                remaining = owned_children()
                self.assertEqual(remaining, set(), f'workers survived {signal.Signals(interruption).name}: {remaining}')
                for pid in tracked:
                    self.assertFalse(Path(f'/proc/{pid}').exists(), f'worker {pid} still exists')
                self.assertEqual(stdout, b'')
        finally:
            kill_owned()
            if supervisor is not None:
                supervisor.communicate(timeout=3)
            deadline = time.monotonic() + 3
            while owned_children() and time.monotonic() < deadline:
                kill_owned()
                reap_owned()
                time.sleep(0.01)
            reap_owned()
            remaining = owned_children()
            libc.prctl(36, previous.value, 0, 0, 0)
            self.assertEqual(remaining, set(), f'independent test cleanup failed: {remaining}')

    def test_sigterm_reaps_child_and_new_session_grandchild(self):
        self.run_lifetime_control(signal.SIGTERM, grandchild=True)

    def test_sighup_reaps_child_and_new_session_grandchild(self):
        self.run_lifetime_control(signal.SIGHUP, grandchild=True)

    def test_sigint_reaps_child_and_new_session_grandchild(self):
        self.run_lifetime_control(signal.SIGINT, grandchild=True)

    def test_parent_sigkill_kills_direct_child(self):
        self.run_lifetime_control(signal.SIGKILL, grandchild=False)

    @unittest.skipUnless(shutil.which('strace'), 'strace needed for traced lifetime control')
    def test_parent_sigkill_kills_tracer_and_new_session_tracees(self):
        self.run_lifetime_control(signal.SIGKILL, grandchild=True, traced=True)


if __name__ == '__main__':
    unittest.main()
