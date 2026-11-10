"""Authored native arena preflight; no game input or graphical runtime."""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

PORT = Path(__file__).resolve().parents[1] / 'Monado_Linux'


@unittest.skipUnless(sys.platform == 'linux', 'native Linux CPU header')
class NativeUIMemory(unittest.TestCase):
    def test_budget_identity_locking_and_cpu_preservation(self):
        local = PORT / '.deps/bin/clang-cc'
        compiler = os.environ.get('MONADO_TEST_CC') or (str(local) if local.is_file() else shutil.which('clang'))
        if not compiler:
            self.skipTest('Clang is required for the native CPU header')
        with tempfile.TemporaryDirectory(prefix='monado-ui-memory-') as temporary:
            binary = Path(temporary) / 'test'
            result = subprocess.run([compiler, '-std=gnu11', '-O1', '-UNDEBUG', '-Wall', '-Wextra', '-Werror',
                                     '-ffp-contract=off', '-fno-strict-aliasing', '-pthread', '-I', str(PORT / 'runtime'),
                                     str(PORT / 'tests/native/test_quickstep_ui_memory.c'),
                                     str(PORT / 'runtime/hle/quickstep_ui_memory.c'), '-o', str(binary)],
                                    capture_output=True, text=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('native UI memory preflight contracts: PASS', result.stdout)


if __name__ == '__main__':
    unittest.main()
