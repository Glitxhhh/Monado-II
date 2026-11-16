"""Native checked optional SDK loader with authored DLLs only; no vendor/game."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile
import unittest
import uuid
sys.path.insert(0,str(Path(__file__).resolve().parent))
from native_renderer_support import PORT,compiler_command,compiler_environment,compiler_is_zig

@unittest.skipUnless(os.name=='nt','Native Win32 optional DLL mapping')
class OptionalLibraryNative(unittest.TestCase):
    def test_native_optional_dll_boundaries(self):
        compiler=compiler_command()
        if not compiler or not compiler_is_zig():self.skipTest('Pinned native Windows Zig compiler/resource tools required')
        environment=compiler_environment()
        with tempfile.TemporaryDirectory(prefix='monado-sdk-loader-') as temporary:
            root=Path(temporary);local=root/'native SDK-雪-résumé-$()-`';local.mkdir()
            dependency='authored-dependency-'+uuid.uuid4().hex+'.dll'
            library=local/'authored-provider.dll';implib=root/'dependency.dll.a'
            source=root/'dependency.c';source.write_text('__declspec(dllexport) int fixture_dependency(void){return 17;}\n',encoding='ascii')
            def compile(source,output,extra=()):
                command=compiler+['-std=gnu11','-O1','-UNDEBUG','-I'+str(PORT/'runtime'),str(source),*extra,'-o',str(output)]
                result=subprocess.run(command,env=environment,capture_output=True,text=True,timeout=120)
                self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            compile(source,local/dependency,['-shared','-Wl,--out-implib,'+str(implib)])
            provider=root/'provider.c';provider.write_text('''#include <windows.h>
#include <wchar.h>
static int guarded;
__declspec(dllimport) int fixture_dependency(void);
BOOL WINAPI DllMain(HINSTANCE instance,DWORD reason,LPVOID reserved){
 (void)reserved;if(reason==DLL_PROCESS_ATTACH){
  wchar_t file[4096],parent[4096],moved[4096];DWORD n=GetModuleFileNameW(instance,file,4096);
  if(!n||n>=4096)return FALSE;
  HANDLE writer=CreateFileW(file,GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,0,OPEN_EXISTING,0,0);
  if(writer==INVALID_HANDLE_VALUE)guarded|=1;else CloseHandle(writer);
  lstrcpyW(parent,file);wchar_t *slash=wcsrchr(parent,L'\\\\');if(!slash)return FALSE;*slash=0;
  lstrcpyW(moved,parent);lstrcatW(moved,L".authored-moved");
  if(!MoveFileExW(parent,moved,0))guarded|=2;else MoveFileExW(moved,parent,0);
 }return TRUE;
}
__declspec(dllexport) int authored_provider_value(void){return fixture_dependency()+guarded;}
''',encoding='ascii')
            compile(provider,library,['-shared',str(implib)])
            resource=root/'windows_utf8.res'
            manifest=subprocess.run(compiler[:-1]+['rc','/I',str(PORT/'tools'),'/fo',str(resource),str(PORT/'tools/windows_utf8.rc')],env=environment,capture_output=True,text=True,timeout=60)
            self.assertEqual(manifest.returncode,0,manifest.stdout+manifest.stderr)
            binary=root/'fixture.exe';compile(PORT/'tests/native/test_optional_library_windows.c',binary,[str(resource),'-municode','-ladvapi32'])
            def run(path,mode='reject'):
                result=subprocess.run([binary,path,mode],cwd=root,capture_output=True,text=True,timeout=30)
                self.assertEqual(result.returncode,0,result.stdout+result.stderr);self.assertIn('PASS',result.stdout)
                return result.stdout
            self.assertIn('local sibling dependency',run(library,'good'))
            self.assertIn('local sibling dependency',run('\\\\?\\'+str(library),'good'))
            long_local=local/('long owned SDK 雪 '+('x'*95))/('Unicode résumé '+('y'*95));long_local.mkdir(parents=True)
            long_library=long_local/library.name
            import shutil
            shutil.copyfile(library,long_library);shutil.copyfile(local/dependency,long_local/dependency)
            self.assertGreater(len(str(long_library)),260)
            self.assertIn('local sibling dependency',run(long_library,'good'))
            self.assertIn('local sibling dependency',run('\\\\?\\'+str(long_library),'good'))
            for unsafe in ('\\\\?\\GLOBALROOT\\Device\\HarddiskVolume1\\evil.dll','\\\\.\\C:\\evil.dll',
                           '\\\\?\\C:\\safe\\file.dll:payload','\\\\?\\C:\\safe\\..\\evil.dll','\\\\?\\Volume{1234}\\evil.dll'):
                run(unsafe)
            run(local/'missing.dll')
            bad=local/'bad.dll';bad.write_bytes(b'authored invalid PE'+b'\0'*128);run(bad)
            alias=local/'hardlink.dll';os.link(library,alias);run(library);alias.unlink()
            run(library,'locked')
            run(library.name) # Exact basename is never an ambient load candidate.
            ambient=root/dependency;(local/dependency).rename(ambient)
            run(library) # CWD/PATH cannot satisfy missing SDK sibling dependency.
            ambient.rename(local/dependency)
            junction=root/'authored-junction'
            made=subprocess.run(['cmd','/c','mklink','/J',str(junction),str(local)],capture_output=True,text=True,timeout=10)
            self.assertEqual(made.returncode,0,made.stdout+made.stderr)
            try:run(junction/library.name)
            finally:junction.rmdir()
            long_alias=long_local/'hardlink.dll';os.link(long_library,long_alias)
            run('\\\\?\\'+str(long_library));long_alias.unlink()
            run('\\\\?\\'+str(long_library),'locked')
            self.assertTrue(library.is_file())

if __name__=='__main__':unittest.main()
