"""Actual native Windows runtime settings and atomic preservation fixtures."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

PORT = Path(__file__).resolve().parents[1] / 'Monado_Windows'


@unittest.skipUnless(sys.platform == 'win32', 'native Windows settings')
class WindowsSettings(unittest.TestCase):
    def test_parser_serializer_unicode_and_failure_preservation(self):
        python = PORT / '.venv/Scripts/python.exe'
        if not python.is_file():
            self.skipTest('project-local native compiler required')
        env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(PORT / '.deps/zig-global-cache'),
                   ZIG_LOCAL_CACHE_DIR=str(PORT / 'build/zig-cache'))
        with tempfile.TemporaryDirectory(prefix='settings-native-', dir=PORT / 'build') as directory:
            out = Path(directory) / 'fixture.exe'
            built = subprocess.run([python, '-m', 'ziglang', 'cc', '-std=gnu11', '-O1', '-UNDEBUG',
                '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', '-ffp-contract=off',
                str(PORT / 'tests/native/test_settings_windows.c'), str(PORT / 'runtime/host/settings_ini.c'),
                str(PORT / 'runtime/host/run_diagnostics_windows.c'), '-lbcrypt', '-ladvapi32',
                '-o', str(out)], env=env, capture_output=True, text=True, timeout=120)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            ran = subprocess.run([out, directory], env=env, capture_output=True, text=True, timeout=30)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
            self.assertIn('PASS', ran.stdout)
