"""LINUX-103: real bounded/native-budget DRS and native-AA sizing."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class DynamicResolutionOutput(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = os.environ.get('MONADO_TEST_CC') or shutil.which('clang') or shutil.which('cc')
        if not cc:
            raise unittest.SkipTest('C11 compiler required')
        cls.directory = tempfile.TemporaryDirectory(prefix='monado-dynamic-output-')
        cls.addClassCleanup(cls.directory.cleanup)
        cls.binary = Path(cls.directory.name) / 'controller'
        args = [cc, '-std=gnu11', '-O1', '-UNDEBUG', '-ffp-contract=off', '-fno-strict-aliasing',
                str(ROOT / 'tests/native/test_dynamic_resolution_output.c'),
                str(ROOT / 'Monado_Linux/runtime/nvn/output_scale.c'),
                str(ROOT / 'Monado_Linux/runtime/host/xcde_aspect.c'), '-lm', '-o', str(cls.binary)]
        if os.environ.get('MONADO_TEST_UBSAN') == '1':
            args[1:1] = ['-fsanitize=undefined', '-fno-sanitize-recover=all']
        p = subprocess.run(args, capture_output=True, text=True, timeout=90)
        if p.returncode:
            raise AssertionError(p.stdout + p.stderr)

    def scenario(self, name):
        p = subprocess.run([str(self.binary), name], capture_output=True, text=True, timeout=15,
                           env={'PATH': '/usr/bin:/bin', 'LANG': 'C.UTF-8'})
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertIn('Dynamic resolution output integration: PASS', p.stdout)

    def test_explicit_pixel_height_endpoints_and_recovery(self):
        self.scenario('bounds')

    def test_configured_native_budget_and_gpu_headroom(self):
        self.scenario('budget')

    def test_effective_native_cap_is_not_generated_refresh(self):
        self.scenario('cap')

    def test_resize_bounds_fit_canvas_and_reset_old_history(self):
        self.scenario('resize')

    def test_existing_automatic_controller_preserves_default_behavior(self):
        self.scenario('legacy')

    def test_dlaa_and_fsr3_native_aa_override_dynamic_and_round_to_output(self):
        self.scenario('native-aa')


if __name__ == '__main__':
    unittest.main()
