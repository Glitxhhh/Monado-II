"""Native hidden authored SDL windows; no game or human-profile activity."""
import os
from pathlib import Path
import subprocess
import unittest
import uuid
from tests.native_renderer_support import WINDOWS,PORT,compiler_command,compiler_environment,graphics_prefix,native_flags

@unittest.skipUnless(WINDOWS,'Actual native SDL/Win32 window transitions')
class NativeWindowing(unittest.TestCase):
    def test_actual_hidden_monitor_placement_fullscreen_restore_and_failure(self):
        directory=PORT/'runs'/('window-native-'+uuid.uuid4().hex);directory.mkdir()
        compiler=compiler_command();environment=compiler_environment();prefix=graphics_prefix()
        environment['PATH']=str(prefix/'bin')+os.pathsep+environment.get('PATH','')
        resource=directory/'native-process.res'
        result=subprocess.run(compiler[:-1]+['rc','/I',str(PORT/'tools'),'/fo',str(resource),str(PORT/'tools/windows_utf8.rc')],env=environment,capture_output=True,text=True,timeout=30)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        binary=directory/'window.exe'
        result=subprocess.run(compiler+native_flags()+['-I'+str(prefix/'include'),str(PORT/'tests/native/test_window_windows.c'),str(resource),str(prefix/'lib/libSDL3.dll.a'),'-luser32','-o',str(binary)],env=environment,capture_output=True,text=True,timeout=120)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        result=subprocess.run([str(binary)],env=environment,capture_output=True,text=True,timeout=30)
        (directory/'stdout.log').write_bytes(result.stdout.encode());(directory/'stderr.log').write_bytes(result.stderr.encode())
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertIn('native hidden window transitions PASS',result.stdout)

if __name__=='__main__':unittest.main()
