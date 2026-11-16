"""Native ordinary API module paths; no game/vendor/profile/network input."""
from pathlib import Path
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parent))
from native_renderer_support import PORT,compiler_command,compiler_environment,compiler_is_zig

@unittest.skipUnless(os.name=='nt','Native Win32 API module path fixture')
class ModulePathsNative(unittest.TestCase):
    def test_production_module_path_and_long_owned_game_library(self):
        compiler=compiler_command()
        if not compiler or not compiler_is_zig():self.skipTest('Pinned native Windows Zig compiler/resource tools required')
        environment=compiler_environment()
        with tempfile.TemporaryDirectory(prefix='monado-module-path-') as temporary:
            root=Path(temporary);identity='a'*64
            resource=root/'windows_utf8.res'
            def run(command,**kwargs):
                result=subprocess.run([str(item) for item in command],env=environment,capture_output=True,text=True,encoding='utf-8',errors='replace',timeout=120,**kwargs)
                self.assertEqual(result.returncode,0,result.stdout+result.stderr);return result
            run(compiler[:-1]+['rc','/I',PORT/'tools','/fo',resource,PORT/'tools/windows_utf8.rc'])
            source=(PORT/'runtime/platform/windows.c').read_text(encoding='utf-8');begin=source.index('bool plat_exe_path(');end=source.index('\n}',begin)+2
            platform=root/'production-exe-path.c';platform.write_text('#include <windows.h>\n#include <stdbool.h>\n#include <limits.h>\n#include "platform/windows_module_path.h"\n'+source[begin:end]+'\n',encoding='utf-8')
            guest=root/'authored-code.c';guest.write_text('#include "gamelib.h"\nstatic void authored(Cpu*c){c->x[0]=42;}\nconst uint64_t main_fn_addrs[]={1},sdk_fn_addrs[]={2},main_fn_count=1,sdk_fn_count=1;\nconst GuestFn main_fn_ptrs[]={authored},sdk_fn_ptrs[]={authored};\n',encoding='utf-8')
            flags=['-std=gnu11','-O1','-UNDEBUG','-march=x86_64_v2','-ffp-contract=off','-fno-fast-math','-I'+str(PORT/'runtime'),'-DGAMELIB_ID="'+identity+'"']
            run(compiler+flags+['-shared',guest,PORT/'runtime/host/gamelib_entry_windows.c','-o',root/'valid.dll'])
            run(compiler+flags+[PORT/'tests/native/test_module_path_windows.c',platform,PORT/'runtime/gamelib.c',resource,'-lbcrypt','-ladvapi32','-o',root/'fixture.exe'])
            long_root=root/('owned 雪 runtime '+('x'*100))/('literal résumé $() ` '+('y'*100));long_root.mkdir(parents=True)
            executable=long_root/'authored.exe';shutil.copyfile(root/'fixture.exe',executable);shutil.copyfile(root/'valid.dll',long_root/'lasgame.dll')
            (long_root/'lasgame.dll.id').write_bytes((identity+'\n').encode('ascii'))
            sidecar=long_root/'lasgame.dll.sha256';sidecar.write_bytes((hashlib.sha256((long_root/'lasgame.dll').read_bytes()).hexdigest()+'\n').encode('ascii'))
            self.assertGreater(len(str(executable)),300)
            extended='\\\\?\\'+str(executable)
            self.assertIn('PASS production API',run([extended,'good'],executable=extended,cwd=root).stdout)
            sidecar.write_bytes(('0'*64+'\n').encode('ascii'))
            self.assertIn('PASS production API',run([extended,'reject'],executable=extended,cwd=root).stdout)

if __name__=='__main__':unittest.main()
