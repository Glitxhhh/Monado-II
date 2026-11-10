"""Both native UI/summary adapters over authored resources, no game input."""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

PORT = Path(__file__).resolve().parents[1] / 'Monado_Linux'

@unittest.skipUnless(sys.platform == 'linux', 'native Linux summary/UI adapters')
class QuickstepStatsIntegrationTests(unittest.TestCase):
    def test_native_ninth_row_and_fixed_summaries(self):
        local = PORT / '.deps/bin/clang-cc'
        compiler = os.environ.get('MONADO_TEST_CC') or (str(local) if local.is_file() else shutil.which('clang'))
        if not compiler:
            self.skipTest('Clang is required for the native CPU header')
        with tempfile.TemporaryDirectory(prefix='monado-stats-integration-') as temporary:
            binary = Path(temporary) / 'native-test'
            command = [compiler, '-std=gnu11', '-O1', '-UNDEBUG', '-Wall', '-Wextra', '-Werror',
                       '-ffp-contract=off', '-fno-strict-aliasing', '-pthread', '-I', str(PORT / 'runtime'),
                       str(PORT / 'tests/native/test_quickstep_stats_integration.c'),
                       str(PORT / 'runtime/hle/quickstep_stats.c'),
                       str(PORT / 'runtime/hle/quickstep_stats_ui.c'), '-o', str(binary)]
            result = subprocess.run(command, capture_output=True, text=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=45)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('combined native ninth-row and fixed-summary contracts: PASS', result.stdout)

if __name__ == '__main__':
    unittest.main()
