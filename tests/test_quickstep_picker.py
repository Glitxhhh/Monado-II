"""Authored native picker bridge fixture; no game input or generated code."""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

PORT = Path(__file__).resolve().parents[1] / 'Monado_Linux'


@unittest.skipUnless(sys.platform == 'linux' and PORT.is_dir(), 'native Linux picker adapter')
class QuickstepPickerTests(unittest.TestCase):
    def test_owned_native_rows_and_mutation_boundaries(self):
        local = PORT / '.deps/bin/clang-cc'
        compiler = os.environ.get('MONADO_TEST_CC') or (str(local) if local.is_file() else shutil.which('clang'))
        if not compiler:
            self.skipTest('Clang is required for the native CPU header')
        with tempfile.TemporaryDirectory(prefix='monado-quickstep-picker-') as temporary:
            binary = Path(temporary) / 'picker-test'
            command = [compiler, '-std=gnu11', '-O1', '-UNDEBUG', '-ffp-contract=off',
                       '-fno-strict-aliasing', '-Wall', '-Wextra', '-Werror',
                       '-pthread', '-Wl,--no-gc-sections', '-I', str(PORT / 'runtime'),
                       str(PORT / 'tests/native/test_quickstep_picker.c'),
                       str(PORT / 'runtime/hle/quickstep_picker.c'), '-o', str(binary)]
            if os.environ.get('MONADO_TEST_UBSAN') == '1':
                command[1:1] = ['-fsanitize=undefined', '-fno-sanitize-recover=all']
            result = subprocess.run(command, capture_output=True, text=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(binary)], cwd=temporary,
                                    capture_output=True, text=True, timeout=45)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('state contracts: PASS', result.stdout)
            self.assertEqual({p.name for p in Path(temporary).iterdir()}, {binary.name})


if __name__ == '__main__':
    unittest.main()
