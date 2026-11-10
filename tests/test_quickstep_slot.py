"""Authored real-gem ownership and bounded scratch fixture; no game input."""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

PORT = Path(__file__).resolve().parents[1] / 'Monado_Linux'


@unittest.skipUnless(sys.platform == 'linux' and PORT.is_dir(), 'native Linux ownership adapter')
class QuickstepSlotTests(unittest.TestCase):
    def test_real_gem_ownership_effect_scratch_and_story_isolation(self):
        local = PORT / '.deps/bin/clang-cc'
        compiler = os.environ.get('MONADO_TEST_CC') or (str(local) if local.is_file() else shutil.which('clang'))
        if not compiler:
            self.skipTest('Clang is required for the native CPU header')
        with tempfile.TemporaryDirectory(prefix='monado-quickstep-slot-') as temporary:
            binary = Path(temporary) / 'slot-test'
            command = [compiler, '-std=gnu11', '-O1', '-UNDEBUG', '-ffp-contract=off',
                       '-fno-strict-aliasing', '-Wall', '-Wextra', '-Werror',
                       '-pthread', '-I', str(PORT / 'runtime'),
                       str(PORT / 'tests/native/test_quickstep_slot.c'),
                       str(PORT / 'runtime/hle/quickstep_slot.c'), '-o', str(binary)]
            result = subprocess.run(command, capture_output=True, text=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(binary)], cwd=temporary,
                                    capture_output=True, text=True, timeout=45)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('mutation contracts: PASS', result.stdout)
            self.assertEqual({p.name for p in Path(temporary).iterdir()}, {binary.name})


if __name__ == '__main__':
    unittest.main()
