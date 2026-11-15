"""Authored vibration protocol/envelope fixtures; no game or device access."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest
import sys

ROOT=Path(__file__).resolve().parents[1]
@unittest.skipUnless(sys.platform == 'linux', 'Linux platform fixture')
class RumblePolicy(unittest.TestCase):
    def compile_run(self,name):
        cc=os.environ.get('MONADO_TEST_CC') or str(ROOT/'Monado_Linux/.deps/bin/clang-cc')
        if not Path(cc).is_file(): cc=shutil.which('clang')
        if not cc:self.skipTest('Clang required by runtime headers')
        with tempfile.TemporaryDirectory(prefix='monado-rumble-') as td:
            exe=Path(td)/name
            args=[cc,'-std=gnu11','-O1','-UNDEBUG','-ffp-contract=off','-fno-strict-aliasing',
                  str(ROOT/'tests/native'/f'test_{name}.c'),'-lm','-o',str(exe)]
            if os.environ.get('MONADO_TEST_UBSAN')=='1':args[1:1]=['-fsanitize=undefined','-fno-sanitize-recover=all']
            p=subprocess.run(args,capture_output=True,text=True,timeout=90)
            self.assertEqual(p.returncode,0,p.stdout+p.stderr)
            p=subprocess.run([str(exe)],capture_output=True,text=True,timeout=10)
            self.assertEqual(p.returncode,0,p.stdout+p.stderr);self.assertIn('PASS',p.stdout)
    def test_envelope_and_stop(self):self.compile_run('rumble_policy')
    def test_single_and_array_protocol(self):self.compile_run('rumble_ipc')

