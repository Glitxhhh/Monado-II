"""Actual Win32 fiber migration, host/guest FP, nonlocal unwind and ownership."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

PORT = Path(__file__).resolve().parents[1] / 'Monado_Windows'


@unittest.skipUnless(sys.platform == 'win32', 'native Windows fibers')
class WindowsFibers(unittest.TestCase):
    def test_native_lifetime_fp_migration_and_nonlocal_unwind(self):
        python = PORT / '.venv/Scripts/python.exe'
        if not python.is_file():
            self.skipTest('project-local native compiler required')
        env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(PORT / '.deps/zig-global-cache'),
                   ZIG_LOCAL_CACHE_DIR=str(PORT / 'build/zig-cache'))
        with tempfile.TemporaryDirectory(prefix='fibers-native-', dir=PORT / 'build') as directory:
            out = Path(directory) / 'fixture.exe'
            built = subprocess.run([python, '-m', 'ziglang', 'cc', '-std=gnu11', '-O1', '-UNDEBUG',
                '-march=x86_64_v2', '-Wall', '-Wextra', '-Werror', '-ffp-contract=off', '-fno-strict-aliasing',
                str(PORT / 'tests/native/test_fibers.c'), str(PORT / 'runtime/hle/fibers.c'), '-o', str(out)],
                env=env, capture_output=True, text=True, timeout=120)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            ran = subprocess.run([out], env=env, capture_output=True, text=True, timeout=30)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
            self.assertIn('OK', ran.stdout)
            refused = subprocess.run([out, 'finalize-running'], env=env, capture_output=True, text=True, timeout=10)
            self.assertNotIn(refused.returncode, (0, 99), refused.stderr)
            self.assertIn('cannot finalize a running fiber', refused.stderr)
