"""LINUX-097: explicit refresh cap; no game, runtime dependencies or GPU."""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
class RefreshPolicy(unittest.TestCase):
    def test_actual_two_output_policy_respects_native_and_refresh_limits(self):
        compiler=os.environ.get('MONADO_TEST_CC') or shutil.which('clang') or shutil.which('cc')
        if not compiler:self.skipTest('C11 compiler required')
        with tempfile.TemporaryDirectory(prefix='monado-refresh-policy-') as tmp:
            binary=Path(tmp)/('policy.exe' if sys.platform=='win32' else 'policy')
            command=[compiler,'-std=c11','-O1','-UNDEBUG','-Wall','-Wextra','-Werror',str(ROOT/'tests/native/test_framegen_refresh_policy.c')]
            if sys.platform!='win32':command+=['-lm']
            command+=['-o',str(binary)]
            if os.environ.get('MONADO_TEST_UBSAN')=='1':command[1:1]=['-fsanitize=undefined','-fno-sanitize-recover=all']
            result=subprocess.run(command,capture_output=True,text=True,timeout=120)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            result=subprocess.run([str(binary)],capture_output=True,text=True,timeout=15)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            self.assertIn('PASS refresh policy',result.stdout)
if __name__=='__main__':unittest.main()
