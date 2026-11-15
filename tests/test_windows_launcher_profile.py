"""Native authored launcher profile/argument fixture; no game or visible UI."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parent))
from native_renderer_support import ROOT,PORT,compiler_command,compiler_environment

@unittest.skipUnless(sys.platform=='win32','Native Windows profile API')
class LauncherProfileNative(unittest.TestCase):
    def test_runtime_and_profile_paths_and_strict_arguments(self):
        command=compiler_command()
        if not command:self.skipTest('Pinned native compiler required')
        if command[-1]=='cc':command=command[:-1]+['c++']
        with tempfile.TemporaryDirectory(prefix='monado-profile-') as temporary:
            root=Path(temporary);profile=root/'profile-résumé-雪-$()-`';profile.mkdir()
            binary=root/'profile-fixture.exe'
            args=command+['-std=c++17','-O1','-UNDEBUG',str(PORT/'tests/native/test_launcher_profile_windows.cpp'),'-ladvapi32','-lshell32','-o',str(binary)]
            result=subprocess.run(args,env=compiler_environment(),capture_output=True,text=True,timeout=120)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            result=subprocess.run([str(binary),str(profile)],capture_output=True,text=True,timeout=30)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            self.assertIn('PASS native launcher explicit canonical owned profile',result.stdout)
            self.assertTrue((profile/'build').is_dir())

if __name__=='__main__':unittest.main()
