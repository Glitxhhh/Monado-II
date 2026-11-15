"""LINUX-099: actual authored downstream counter, no driver/game/window."""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[1]
PORT = ROOT / 'Monado_Linux'


@unittest.skipUnless(sys.platform == 'linux', 'Linux local Vulkan layer')
class PresentObserver(unittest.TestCase):
    def test_actual_accounting_failure_passthrough_window_and_concurrent_snapshot(self):
        local = PORT / '.deps/bin/clang-cc'
        compiler = os.environ.get('MONADO_TEST_CC') or (str(local) if local.is_file() else shutil.which('clang'))
        if not compiler:
            self.skipTest('native Clang required')
        marker = Path(os.environ.get('MONADO_TEST_SDL_PORT', str(PORT))) / '.deps/graphics/active-sysroot.txt'
        if not marker.is_file():
            self.skipTest('pinned Vulkan headers required')
        include = Path(marker.read_text().strip()) / 'usr/include'
        with tempfile.TemporaryDirectory(prefix='monado-present-observer-') as temporary:
            out = Path(temporary); binary = out / 'observer'
            cmd = [compiler, '-std=c11', '-O1', '-UNDEBUG', '-Wall', '-Wextra', '-Werror',
                   '-I' + str(include), str(ROOT / 'tests/native/test_present_observer.c'), '-pthread', '-lm', '-o', str(binary)]
            env = {'PATH': '/usr/bin:/bin', 'LANG': 'C.UTF-8', 'LC_ALL': 'C.UTF-8'}
            if os.environ.get('MONADO_TEST_UBSAN') == '1':
                cmd[1:1] = ['-fsanitize=undefined', '-fno-sanitize-recover=all']
                env['UBSAN_OPTIONS'] = 'halt_on_error=1'
            result = subprocess.run(cmd, env=env, capture_output=True, text=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(binary)], env=env, capture_output=True, text=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(result.stdout.count('PASS '), 4)


if __name__ == '__main__':
    unittest.main()
