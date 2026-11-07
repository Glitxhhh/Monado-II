"""[XCDE] LINUX-007: validate the external boot deadline without game execution."""
import json
import hashlib
import importlib
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import textwrap
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]


@unittest.skipUnless(sys.platform == 'linux', 'Linux process ownership')
class BootDeadlineTests(unittest.TestCase):
    def run_control(self, child, seconds, callback_failure=False, read_signal=None):
        # Dedicated interpreter contains subreaping/core-limit changes and owns
        # all synthetic descendants. No game binary or selected input is used.
        script = textwrap.dedent('''
            import json, os, signal, sys
            from boot_smoke_linux import bounded_process, owned_children
            completion = {}
            before = {sig: signal.getsignal(sig) for sig in
                      (signal.SIGTERM, signal.SIGHUP, signal.SIGINT)}
            read_signal_sent = False
            original_read = os.read
            def read_at_signal_boundary(fd, size):
                global read_signal_sent
                data = original_read(fd, size)
                # Real SIGTERM/SIGHUP/SIGINT at the consume-before-append
                # boundary in CPython's communicate, not the exec error pipe.
                if READ_SIGNAL and size == 32768 and b'read-boundary' in data and not read_signal_sent:
                    read_signal_sent = True
                    os.kill(os.getpid(), READ_SIGNAL)
                return data
            if READ_SIGNAL:
                os.read = read_at_signal_boundary
            def callback(record):
                if FAIL: raise RuntimeError('injected callback failure')
            try:
                record, out, err = bounded_process([sys.executable, '-B', '-c', CHILD],
                    cwd='.', env={'PATH':'/usr/bin:/bin'}, seconds=SECONDS,
                    on_start=callback, completion=completion)
                record.update(out=out.decode(), err=err.decode(), remaining=list(owned_children()))
            except BaseException as error:
                record = completion['record']
                record.update(error=str(error), error_type=type(error).__name__, remaining=list(owned_children()),
                              out=completion['stdout'].decode(), err=completion['stderr'].decode())
            record['read_signal_sent'] = read_signal_sent
            record['handlers_restored'] = all(signal.getsignal(sig) == handler for sig, handler in before.items())
            print(json.dumps(record))
        ''')
        prefix = f'import sys\nsys.path.insert(0, {str(ROOT / "scripts")!r})\n'
        prefix += f'CHILD={child!r}\nSECONDS={seconds!r}\nFAIL={callback_failure!r}\n'
        prefix += f'READ_SIGNAL={int(read_signal) if read_signal else None!r}\n'
        run = subprocess.run([sys.executable, '-B', '-c', prefix + script], cwd=ROOT,
                             capture_output=True, text=True, timeout=12)
        self.assertEqual(run.returncode, 0, run.stderr)
        result = json.loads(run.stdout)
        self.assertEqual(result['remaining'], [])
        self.assertTrue(result['handlers_restored'])
        return result

    def test_child_exit_and_pipe_capture_preserved(self):
        result = self.run_control('import sys; print("out"); print("err",file=sys.stderr); sys.exit(37)', 2)
        self.assertEqual(result['exit'], 37)
        self.assertFalse(result['timed_out'])
        self.assertEqual(result['out'], 'out\n')
        self.assertEqual(result['err'], 'err\n')

    def test_timeout_reaps_child_and_new_session_grandchild(self):
        result = self.run_control('import os,time\nif os.fork()==0: os.setsid()\ntime.sleep(60)', 0.25)
        self.assertTrue(result['timed_out'])
        self.assertGreaterEqual(len(set(result['killed_owned_pids'])), 2)
        self.assertLess(result['seconds'], 6)

    def test_exited_adopted_helper_is_reaped_without_forced_kill(self):
        child = 'import os,time\nif os.fork()==0: os._exit(0)\ntime.sleep(.15)\nos._exit(0)'
        result = self.run_control(child, 2)
        self.assertEqual(result['exit'], 0)
        self.assertFalse(result['timed_out'])
        self.assertEqual(result['killed_owned_pids'], [])
        self.assertTrue(result['reaped_descendants'])
        self.assertTrue(all(item['wait_status'] == 0 for item in result['reaped_descendants']))

    def test_orphan_holding_pipe_is_bounded(self):
        child = 'import os,time\nif os.fork(): os._exit(0)\nos.setsid()\ntime.sleep(60)'
        result = self.run_control(child, 0.25)
        self.assertTrue(result['timed_out'])
        self.assertTrue(result['killed_owned_pids'])

    def test_callback_failure_cleans_child(self):
        result = self.run_control('import time; time.sleep(60)', 2, callback_failure=True)
        self.assertEqual(result['error'], 'injected callback failure')
        self.assertEqual(result['remaining_owned_pids'], [])
        self.assertEqual(result['exit'], -signal.SIGKILL)
        self.assertIn(result['pid'], result['killed_owned_pids'])

    def test_sigterm_retains_drained_pipes_and_cleanup_record(self):
        child = ('import os,signal,sys,time\nprint("before-term",flush=True)\n'
                 'print("term-stderr",file=sys.stderr,flush=True)\n'
                 'os.kill(os.getppid(),signal.SIGTERM)\ntime.sleep(60)')
        result = self.run_control(child, 2)
        self.assertIn('signal 15', result['error'])
        self.assertEqual(result['remaining_owned_pids'], [])
        self.assertEqual(result['exit'], -signal.SIGKILL)
        self.assertEqual(result['out'], 'before-term\n')
        self.assertEqual(result['err'], 'term-stderr\n')

    def test_signals_at_pipe_read_boundary_retain_consumed_bytes(self):
        # Probe stdout and stderr independently: losing either consumed read is
        # irreversible even if cleanup later drains all remaining pipe bytes.
        for interruption in (signal.SIGTERM, signal.SIGHUP, signal.SIGINT):
            for stream in ('stdout', 'stderr'):
                with self.subTest(interruption=interruption, stream=stream):
                    child = ('import sys,time\n'
                             f'print("read-boundary",file=sys.{stream},flush=True)\n'
                             'time.sleep(60)')
                    result = self.run_control(child, 2, read_signal=interruption)
                    self.assertTrue(result['read_signal_sent'])
                    expected_type = 'KeyboardInterrupt' if interruption == signal.SIGINT else 'BootInterrupted'
                    self.assertEqual(result['error_type'], expected_type)
                    self.assertEqual(result['out' if stream == 'stdout' else 'err'], 'read-boundary\n')
                    self.assertFalse(result['timed_out'])
                    self.assertEqual(result['remaining_owned_pids'], [])
                    self.assertEqual(result['exit'], -signal.SIGKILL)
                    self.assertIn(result['pid'], result['killed_owned_pids'])

    def test_sigint_retains_drained_pipes_and_cleanup_record(self):
        child = ('import os,signal,sys,time\nprint("before-int",flush=True)\n'
                 'print("int-stderr",file=sys.stderr,flush=True)\n'
                 'os.kill(os.getppid(),signal.SIGINT)\ntime.sleep(60)')
        result = self.run_control(child, 2)
        self.assertEqual(result['error_type'], 'KeyboardInterrupt')
        self.assertEqual(result['remaining_owned_pids'], [])
        self.assertEqual(result['exit'], -signal.SIGKILL)
        self.assertEqual(result['out'], 'before-int\n')
        self.assertEqual(result['err'], 'int-stderr\n')

    def test_signal_with_completing_child_is_not_ignored(self):
        result = self.run_control('print("read-boundary",flush=True)', 2,
                                  read_signal=signal.SIGTERM)
        self.assertEqual(result['error_type'], 'BootInterrupted')
        self.assertEqual(result['out'], 'read-boundary\n')
        self.assertFalse(result['timed_out'])

    def test_output_across_deadline_polls_is_not_duplicated(self):
        child = ('import sys,time\n'
                 'for i in range(4):\n'
                 ' print(i,flush=True)\n'
                 ' print(i,file=sys.stderr,flush=True)\n'
                 ' time.sleep(.12)')
        result = self.run_control(child, 2)
        self.assertEqual(result['exit'], 0)
        self.assertFalse(result['timed_out'])
        self.assertEqual(result['out'], '0\n1\n2\n3\n')
        self.assertEqual(result['err'], '0\n1\n2\n3\n')


class BootReceiptTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with mock.patch.object(sys, 'path', [str(ROOT / 'scripts'), *sys.path]):
            cls.boot = importlib.import_module('boot_smoke_linux')

    def run_mock_boot(self, failure=None, *, metadata_failure=False, exit_code=0):
        # Exercise main's actual evidence-writing path with synthetic files and
        # a mocked completed supervisor. Never validate or run a game binary.
        with tempfile.TemporaryDirectory(prefix='monado-boot-receipt-') as temporary:
            root = Path(temporary)
            source = root / 'input'
            source.mkdir()
            (source / 'fixture.bin').write_bytes(b'unchanged input')
            scripts = root / 'scripts'
            scripts.mkdir()
            for name in ('boot_smoke_linux.py', 'run_linux.py', 'platform_tests_linux.py'):
                (scripts / name).write_text('synthetic source\n')
            binary = root / 'build' / 'synthetic'
            binary.parent.mkdir()
            (binary.parent / 'receipt.json').write_text('{}\n')
            output = root / 'runs' / 'fixture'
            state = output / 'state'
            launch = {'argv': [str(binary)], 'input': str(source)}
            record = {'pid': 123, 'exit': -signal.SIGKILL if failure else exit_code,
                      'timed_out': False, 'seconds': 0.1, 'remaining_owned_pids': [],
                      'killed_owned_pids': [123] if failure else []}

            def completed(argv, *, completion, on_start, **kwargs):
                on_start(record)
                (state / 'logs').mkdir(parents=True)
                (state / 'logs' / 'fixture.log').write_bytes(b'final state log\n')
                completion.update(record=record, stdout=b'early out\n', stderr=b'early err\n')
                if failure is not None:
                    raise failure
                return record, completion['stdout'], completion['stderr']

            original_metadata = self.boot.input_metadata
            before = original_metadata(source)
            metadata = mock.Mock(side_effect=[before, OSError('injected metadata failure')]) if metadata_failure else original_metadata
            with (mock.patch.object(self.boot, 'ROOT', root),
                  mock.patch.object(self.boot, 'plan', return_value=launch),
                  mock.patch.object(self.boot, 'bounded_process', side_effect=completed),
                  mock.patch.object(self.boot, 'input_metadata', metadata),
                  mock.patch.object(sys, 'argv', ['boot_smoke_linux.py', '--executable', str(binary), '--output', str(output)]),
                  mock.patch('builtins.print')):
                if failure is None:
                    self.assertEqual(self.boot.main(), 0 if exit_code == 0 else 1)
                else:
                    with self.assertRaises(type(failure)) as raised:
                        self.boot.main()
                    self.assertIs(raised.exception, failure)
            receipt = json.loads((output / 'receipt.json').read_text())
            self.assertEqual(receipt['process'], record)
            self.assertEqual((output / 'early-stdout.log').read_bytes(), b'early out\n')
            self.assertEqual((output / 'early-stderr.log').read_bytes(), b'early err\n')
            self.assertEqual(receipt['state_files']['logs/fixture.log']['sha256'],
                             hashlib.sha256(b'final state log\n').hexdigest())
            self.assertFalse(receipt['expected_captures_present'])
            self.assertIn('finished_utc', receipt)
            if not metadata_failure:
                self.assertTrue(receipt['input_metadata_unchanged'])
                self.assertEqual(receipt['input_after'], before)
            return receipt

    def test_callback_error_receipt_preserves_original_failure(self):
        receipt = self.run_mock_boot(RuntimeError('injected callback failure'))
        self.assertEqual(receipt['status'], 'runner_error')
        self.assertEqual(receipt['error'], 'injected callback failure')

    def test_sigterm_receipt_keeps_input_state_and_process_evidence(self):
        receipt = self.run_mock_boot(self.boot.BootInterrupted('boot supervisor interrupted by signal 15'))
        self.assertEqual(receipt['status'], 'runner_error')
        self.assertEqual(receipt['error_type'], 'BootInterrupted')

    def test_evidence_failure_does_not_replace_interruption(self):
        receipt = self.run_mock_boot(self.boot.BootInterrupted('original interruption'), metadata_failure=True)
        self.assertEqual(receipt['error'], 'original interruption')
        self.assertEqual(receipt['evidence_errors']['input_after'], 'injected metadata failure')

    def test_nonzero_child_exit_remains_a_failure(self):
        receipt = self.run_mock_boot(exit_code=37)
        self.assertEqual(receipt['status'], 'exited')
        self.assertEqual(receipt['process']['exit'], 37)


if __name__ == '__main__':
    unittest.main()
