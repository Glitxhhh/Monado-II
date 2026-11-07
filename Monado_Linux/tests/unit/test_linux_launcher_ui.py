"""[XCDE] LINUX-009: synthetic UI-runner cleanup/evidence failure regressions.

All processes and UI input are mocked. These exercise the real runner control
flow and receipt writes without opening a window or executing a game.
"""
import importlib.util
import json
from pathlib import Path
import struct
import sys
import tempfile
import types
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
try:
    spec = importlib.util.spec_from_file_location('launcher_ui_tests_linux', ROOT / 'scripts/launcher_ui_tests_linux.py')
    runner = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(runner)
finally:
    sys.path.pop(0)


@unittest.skipUnless(sys.platform == 'linux', 'Linux UI supervisor tests')
class LauncherUiReceipt(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory(prefix='monado-launcher-ui-')
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)
        (self.root / 'runs').mkdir()
        (self.root / 'scripts').mkdir()
        for name in ('launcher_ui_tests_linux.py', 'launcher_linux.py', 'run_linux.py',
                     'boot_smoke_linux.py', 'platform_tests_linux.py'):
            (self.root / 'scripts' / name).write_text('# synthetic identity fixture\n')
        self.inspector = self.root / 'inspector'
        self.inspector.write_bytes(b'synthetic inspector; never executed')
        self.calls = 0

    def execute(self, name, *, process_update=None, interrupt=None, metadata_error=None):
        output = self.root / 'runs' / name
        args = ['launcher_ui_tests_linux.py', '--launcher-executable', str(self.root / 'launcher'),
                '--output', str(output), '--settings-inspector', str(self.inspector)]
        def plan(binary, state):
            return {'argv': [str(binary)], 'input': str(self.root / 'input'), 'library_path': '/synthetic/lib'}
        def process(argv, **kwargs):
            self.calls += 1
            record = {'wall_limit_seconds': kwargs['seconds'], 'timed_out': False,
                      'killed_owned_pids': [], 'pid': 700 + self.calls, 'reaped_descendants': [],
                      'exit': 0, 'seconds': 0.01, 'remaining_owned_pids': []}
            if self.calls == 1:
                record.update(process_update or {})
            kwargs['on_start'](record)
            kwargs['completion'].update(record=record, stdout=b'preserved output', stderr=b'')
            if interrupt is not None:
                raise interrupt
            # Sufficient synthetic output for every otherwise-passing GUI phase.
            bmp = bytearray(54)
            bmp[:2] = b'BM'
            struct.pack_into('<ii', bmp, 18, 980, 680)
            Path(argv[argv.index('--shot') + 1]).write_bytes(bmp)
            state = Path(argv[argv.index('--state-root') + 1])
            (state / 'xcde.ini').write_text('window = 1600x900\nshow_fps = 1\n')
            return record, b'preserved output', b''
        metadata = {'entries': 1, 'sha256': 'synthetic-input'}
        inspector = types.SimpleNamespace(returncode=0, stdout='{"window":[1600,900],"show_fps":1}', stderr='')
        with mock.patch.object(runner, 'ROOT', self.root), \
             mock.patch.object(sys, 'argv', args), \
             mock.patch.object(runner, 'plan', side_effect=plan), \
             mock.patch.object(runner, 'clean_environment', return_value={}), \
             mock.patch.object(runner, 'input_metadata', side_effect=[metadata, metadata_error or metadata]), \
             mock.patch.object(runner, 'bounded_process', side_effect=process), \
             mock.patch.object(runner.subprocess, 'run', return_value=inspector):
            result = runner.main()
        return result, json.loads((output / 'receipt.json').read_text())

    def test_successful_exit_with_forced_cleanup_is_not_a_pass(self):
        for name, update in (('killed', {'killed_owned_pids': [999]}),
                             ('cleanup-error', {'cleanup_errors': ['synthetic cleanup failure']})):
            with self.subTest(name=name):
                self.calls = 0
                code, receipt = self.execute(name, process_update=update)
                self.assertEqual(code, 1)
                self.assertEqual(receipt['status'], 'failed')
                self.assertIn('did not exit cleanly', receipt['error'])
                self.assertEqual(len(receipt['phases']), 1)

    def test_keyboard_interrupt_records_completion_despite_final_evidence_error(self):
        original = KeyboardInterrupt('synthetic user interruption')
        with self.assertRaises(KeyboardInterrupt) as caught:
            self.execute('interrupted', interrupt=original, metadata_error=OSError('metadata unavailable'))
        self.assertIs(caught.exception, original)
        receipt = json.loads((self.root / 'runs/interrupted/receipt.json').read_text())
        self.assertEqual(receipt['status'], 'failed')
        self.assertEqual(receipt['error_type'], 'KeyboardInterrupt')
        self.assertEqual(receipt['error'], str(original))
        self.assertEqual(receipt['evidence_errors']['input_after']['error_type'], 'OSError')
        self.assertEqual(receipt['phases'][0]['process']['remaining_owned_pids'], [])
        self.assertEqual((self.root / 'runs/interrupted/open/stdout.log').read_bytes(), b'preserved output')

    def test_phase_log_failure_does_not_replace_keyboard_interrupt(self):
        original = KeyboardInterrupt('primary interruption')
        write_bytes = Path.write_bytes
        def fail_log(path, data):
            if path.name == 'stdout.log':
                raise OSError('secondary log write failure')
            return write_bytes(path, data)
        with mock.patch.object(Path, 'write_bytes', fail_log), self.assertRaises(KeyboardInterrupt) as caught:
            self.execute('phase-log-failure', interrupt=original)
        self.assertIs(caught.exception, original)
        receipt = json.loads((self.root / 'runs/phase-log-failure/receipt.json').read_text())
        self.assertEqual(receipt['error_type'], 'KeyboardInterrupt')
        self.assertEqual(receipt['phases'][0]['evidence_errors']['stdout']['error'], 'secondary log write failure')
        self.assertTrue(receipt['input_metadata_unchanged'])

    def test_receipts_use_atomic_publication(self):
        calls = []
        replace = Path.replace
        def check_replace(path, target):
            if Path(target).name == 'receipt.json':
                self.assertEqual(path.name, 'receipt.json.tmp')
                json.loads(path.read_text())
                calls.append(str(target))
            return replace(path, target)
        with mock.patch.object(Path, 'replace', check_replace):
            code, receipt = self.execute('atomic')
        self.assertEqual(code, 0)
        self.assertEqual(receipt['status'], 'passed')
        self.assertGreaterEqual(len(calls), 4)


if __name__ == '__main__':
    unittest.main()
