"""Authored expanded-save native bridge regressions; no game input or user saves."""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
PORT = ROOT / 'Monado_Linux'


@unittest.skipUnless(sys.platform == 'linux' and PORT.is_dir(), 'native Linux save UI bridge')
class ExpandedSaveNativeTests(unittest.TestCase):
    def compile_and_run(self, name, extra_sources=()):
        local = PORT / '.deps/bin/clang-cc'
        compiler = os.environ.get('MONADO_TEST_CC') or (str(local) if local.is_file() else shutil.which('clang'))
        if not compiler:
            self.skipTest('Clang is unavailable for the authored native C fixture')
        source = PORT / 'tests/native' / (name + '.c')
        with tempfile.TemporaryDirectory(prefix='monado-expanded-saves-') as temporary:
            binary = Path(temporary) / name
            command = [compiler, '-std=c11', '-O1', '-UNDEBUG', '-ffp-contract=off',
                       '-D_POSIX_C_SOURCE=200809L',
                       '-fno-strict-aliasing', '-Wall', '-Wextra', '-Werror',
                       '-pthread', '-I', str(PORT / 'runtime'), str(source),
                       *(str(PORT / path) for path in extra_sources), '-o', str(binary)]
            compiled = subprocess.run(command, capture_output=True, text=True, timeout=120)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            executed = subprocess.run([str(binary)], cwd=temporary,
                                      capture_output=True, text=True, timeout=45)
            self.assertEqual(executed.returncode, 0, executed.stdout + executed.stderr)
            self.assertEqual({path.name for path in Path(temporary).iterdir()}, {binary.name},
                             'The in-memory bridge must not create empty save/thumbnail files')
            return executed.stdout

    def test_record_capacity_ownership_and_selection_provenance(self):
        output = self.compile_and_run('test_expanded_saves')
        self.assertIn('expanded saves: authored native bridge invariants passed', output)
        self.assertIn('expanded saves: bounded initial-read pump passed', output)

    def test_bounded_continue_and_availability_scans(self):
        output = self.compile_and_run('test_expanded_save_scan',
                                      ('runtime/hle/expanded_save_scan.c',))
        self.assertIn('PASS: bounded Continue/availability scans', output)


if __name__ == '__main__':
    unittest.main()
