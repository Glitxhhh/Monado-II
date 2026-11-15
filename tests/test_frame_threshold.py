"""Threshold timing at fractional native cadence, with no GPU/game session."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

@unittest.skipUnless(os.name == 'posix', 'Linux authored native fixture')
class FrameThreshold(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='monado-frame-threshold-')
        cls.addClassCleanup(cls.temp.cleanup)
        cls.binary = Path(cls.temp.name) / 'test'
        cc = os.environ.get('MONADO_TEST_CC') or str(ROOT / 'Monado_Linux/.deps/bin/clang-cc')
        cmd = [cc, '-std=gnu11', '-O1', '-UNDEBUG', '-ffp-contract=off', '-fno-strict-aliasing',
               '-ffunction-sections', '-fdata-sections', '-I' + str(ROOT / 'Monado_Linux/runtime'),
               str(ROOT / 'tests/native/test_frame_threshold.c'), '-Wl,--gc-sections', '-pthread', '-lm', '-o', str(cls.binary)]
        if os.environ.get('MONADO_TEST_UBSAN') == '1':
            cmd[1:1] = ['-fsanitize=undefined', '-fno-sanitize-recover=all']
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
        if result.returncode: raise AssertionError(result.stdout + result.stderr)

    def test_native_rates_and_duplicate_output_pause_reset(self):
        for hz in (30, 60, 90, 120, 144, 165, 180, 240, 360, 75, 100):
            with self.subTest(hz=hz):
                result = subprocess.run([str(self.binary), str(hz)], capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

if __name__ == '__main__': unittest.main()
