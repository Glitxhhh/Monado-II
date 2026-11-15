"""LINUX-046: registration lifetime, compiler constants and former registry cap."""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

from native_renderer_support import PORT, WINDOWS, compiler_command, compiler_environment, binary_name, native_flags


@unittest.skipUnless(sys.platform in ('linux', 'win32'), 'native Linux or Windows NVN fixture')
class ShaderSnapshotTests(unittest.TestCase):
    def test_immutable_registration_and_constants(self):
        compiler = compiler_command()
        if not compiler:
            self.skipTest('Clang is required for the native CPU header')
        with tempfile.TemporaryDirectory(prefix='monado-shader-snapshots-') as temporary:
            binary = Path(temporary) / binary_name('snapshots')
            command = compiler + native_flags() + [
                       '-I', str(PORT / 'runtime'), str(PORT / 'tests/native/test_shader_snapshots.c'),
                       '-o', str(binary)]
            result = subprocess.run(command, env=compiler_environment(), capture_output=True, text=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(binary)], cwd=temporary, capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('immutable shader snapshots: PASS (8 contract groups)', result.stdout)


if __name__ == '__main__':
    unittest.main()
