"""Authored optional-metadata/actual-filesystem tests; no game files or user saves."""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
PORT = ROOT / 'Monado_Linux'


@unittest.skipUnless(sys.platform == 'linux' and PORT.is_dir(), 'native Linux save pairing')
class QuickstepSaveTests(unittest.TestCase):
    def test_real_slot_save_filesystem_roundtrip(self):
        local = PORT / '.deps/bin/clang-cc'
        compiler = os.environ.get('MONADO_TEST_CC') or (str(local) if local.is_file() else shutil.which('clang'))
        if not compiler:
            self.skipTest('Clang is required for the authored native fixture')
        with tempfile.TemporaryDirectory(prefix='monado-quickstep-roundtrip-') as temporary:
            binary = Path(temporary) / 'test-quickstep-roundtrip'
            command = [compiler, '-std=gnu11', '-O1', '-UNDEBUG', '-DQUICKSTEP_TRANSITION_TEST', '-DQUICKSTEP_SPLIT_READ_TEST', '-ffp-contract=off',
                       '-fno-strict-aliasing', '-ffunction-sections', '-fdata-sections',
                       '-Wall', '-Wextra', '-Werror', '-pthread', '-Wl,--gc-sections',
                       '-I', str(PORT / 'runtime'),
                       str(PORT / 'tests/native/test_quickstep_roundtrip.c'),
                       str(PORT / 'runtime/hle/quickstep_slot.c'), '-o', str(binary)]
            built = subprocess.run(command, capture_output=True, text=True, timeout=120)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            result = subprocess.run([str(binary)], cwd=temporary, capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('PASS: real Quickstep slot/save/filesystem roundtrip', result.stdout)

    def test_codec_and_real_save_transaction_failures(self):
        local = PORT / '.deps/bin/clang-cc'
        compiler = os.environ.get('MONADO_TEST_CC') or (str(local) if local.is_file() else shutil.which('clang'))
        if not compiler:
            self.skipTest('Clang is required for the authored native fixture')
        with tempfile.TemporaryDirectory(prefix='monado-quickstep-save-') as temporary:
            binary = Path(temporary) / 'test-quickstep-save'
            command = [compiler, '-std=gnu11', '-O1', '-UNDEBUG', '-ffp-contract=off',
                       '-fno-strict-aliasing', '-ffunction-sections', '-fdata-sections',
                       '-Wall', '-Wextra', '-Werror', '-pthread', '-Wl,--gc-sections',
                       '-I', str(PORT / 'runtime'),
                       str(PORT / 'tests/native/test_quickstep_save.c'), '-o', str(binary)]
            built = subprocess.run(command, capture_output=True, text=True, timeout=120)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            result = subprocess.run([str(binary)], cwd=temporary, capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('PASS: Quickstep metadata codec', result.stdout)


if __name__ == '__main__':
    unittest.main()
