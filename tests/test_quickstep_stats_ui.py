"""Authored native row instance/lifetime fixture; no game input or renderer."""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

PORT = Path(__file__).resolve().parents[1] / 'Monado_Linux'

@unittest.skipUnless(sys.platform == 'linux', 'native Linux row adapter')
class QuickstepStatsUITests(unittest.TestCase):
    def test_component_ownership_and_cleanup(self):
        local = PORT / '.deps/bin/clang-cc'
        compiler = os.environ.get('MONADO_TEST_CC') or (str(local) if local.is_file() else shutil.which('clang'))
        if not compiler:
            self.skipTest('Clang is required for the native CPU header')
        with tempfile.TemporaryDirectory(prefix='monado-native-stats-ui-') as temporary:
            binary = Path(temporary) / 'native-test'
            result = subprocess.run([
                compiler, '-std=gnu11', '-O1', '-UNDEBUG', '-Wall', '-Wextra', '-Werror',
                '-ffp-contract=off', '-fno-strict-aliasing', '-pthread', '-I', str(PORT / 'runtime'),
                str(PORT / 'tests/native/test_quickstep_stats_ui.c'),
                str(PORT / 'runtime/hle/quickstep_stats_ui.c'), '-o', str(binary),
            ], capture_output=True, text=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=45)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('ownership contracts: PASS', result.stdout)
            self.assertNotIn('runtime error:', result.stderr)

if __name__ == '__main__':
    unittest.main()
