"""Authored native title Exit regressions; no game input, window or GPU."""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
PORT = ROOT / 'Monado_Linux'


@unittest.skipUnless(sys.platform == 'linux', 'native Linux title-menu implementation')
class TitleExitNativeTests(unittest.TestCase):
    def compile_and_run(self, name):
        local = PORT / '.deps/bin/clang-cc'
        # cpu.h's portable SIMD helpers use Clang builtins even though these
        # fixtures exercise only native UI contracts. GCC is not a fallback.
        compiler = os.environ.get('MONADO_TEST_CC') or (str(local) if local.is_file() else shutil.which('clang'))
        if not compiler:
            self.skipTest('Clang is unavailable for the authored native C fixture')
        source = PORT / 'tests/native' / (name + '.c')
        with tempfile.TemporaryDirectory(prefix='monado-title-exit-') as temporary:
            binary = Path(temporary) / name
            command = [compiler, '-std=c11', '-O1', '-UNDEBUG', '-ffp-contract=off',
                       '-fno-strict-aliasing', '-Wall', '-Wextra', '-Werror',
                       str(source), '-o', str(binary)]
            compiled = subprocess.run(command, capture_output=True, text=True, timeout=120)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            executed = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
            self.assertEqual(executed.returncode, 0, executed.stdout + executed.stderr)
            return executed.stdout

    def test_row_order_status_capacity_and_failure_atomicity(self):
        output = self.compile_and_run('test_title_exit_model')
        self.assertRegex(output, r'title Exit row model: [0-9]+ synthetic scenarios passed')

    def test_native_bridge_confirmation_and_register_preservation(self):
        output = self.compile_and_run('test_title_exit_bridge')
        self.assertIn('native-call/register preservation', output)

    def test_caption_updates_only_the_owned_positive_choice(self):
        output = self.compile_and_run('test_desktop_exit_caption')
        self.assertIn('native positive-only update', output)

    def test_system_menu_rows_permissions_and_confirmation(self):
        output = self.compile_and_run('test_system_exit_bridge')
        self.assertIn('System Exit', output)


if __name__ == '__main__':
    unittest.main()
