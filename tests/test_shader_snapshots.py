"""LINUX-046: registration lifetime, compiler constants and former registry cap."""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

PORT = Path(__file__).resolve().parents[1] / 'Monado_Linux'


@unittest.skipUnless(sys.platform == 'linux', 'native Linux NVN fixture')
class ShaderSnapshotTests(unittest.TestCase):
    def test_immutable_registration_and_constants(self):
        local = PORT / '.deps/bin/clang-cc'
        compiler = os.environ.get('MONADO_TEST_CC') or (str(local) if local.is_file() else shutil.which('clang'))
        if not compiler:
            self.skipTest('Clang is required for the native CPU header')
        with tempfile.TemporaryDirectory(prefix='monado-shader-snapshots-') as temporary:
            binary = Path(temporary) / 'snapshots'
            command = [compiler, '-std=gnu11', '-O1', '-UNDEBUG', '-ffp-contract=off',
                       '-fno-strict-aliasing', '-ffunction-sections', '-fdata-sections',
                       '-Wall', '-Wextra', '-Werror', '-Wno-format', '-Wno-unused-parameter', '-pthread', '-Wl,--gc-sections',
                       '-I', str(PORT / 'runtime'), str(PORT / 'tests/native/test_shader_snapshots.c'),
                       '-o', str(binary)]
            result = subprocess.run(command, capture_output=True, text=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(binary)], cwd=temporary, capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('immutable shader snapshots: PASS (8 contract groups)', result.stdout)


if __name__ == '__main__':
    unittest.main()
