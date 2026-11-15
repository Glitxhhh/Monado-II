"""Authored color sampler eligibility fixtures; no game or device access."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest
import sys

from native_renderer_support import ROOT, PORT, WINDOWS, compiler_command, compiler_environment, binary_name, native_flags
@unittest.skipUnless(sys.platform in ('linux', 'win32'), 'native Linux or Windows fixture')
class SamplerFilterPolicy(unittest.TestCase):
    def compile_run(self,name):
        cc=compiler_command()
        if not cc:self.skipTest('Clang required by runtime headers')
        with tempfile.TemporaryDirectory(prefix='monado-rumble-') as td:
            exe=Path(td)/binary_name(name)
            args=cc+native_flags()+['-I',str(PORT/'runtime'),
                  str(ROOT/'tests/native'/f'test_{name}.c'),'-o',str(exe)]
            if os.environ.get('MONADO_TEST_UBSAN')=='1':args[1:1]=['-fsanitize=undefined','-fno-sanitize-recover=all']
            p=subprocess.run(args,env=compiler_environment(),capture_output=True,text=True,timeout=90)
            self.assertEqual(p.returncode,0,p.stdout+p.stderr)
            p=subprocess.run([str(exe)],capture_output=True,text=True,timeout=10)
            self.assertEqual(p.returncode,0,p.stdout+p.stderr);self.assertIn('PASS',p.stdout)
    def test_color_only_filtering(self):self.compile_run('sampler_filter_policy')

