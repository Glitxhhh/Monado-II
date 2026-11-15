"""Native checked optional SDK loader with authored DLLs only; no vendor/game."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile
import unittest
import uuid
sys.path.insert(0,str(Path(__file__).resolve().parent))
from native_renderer_support import PORT,compiler_command,compiler_environment

@unittest.skipUnless(os.name=='nt','Native Win32 optional DLL mapping')
class OptionalLibraryNative(unittest.TestCase):
    def test_native_optional_dll_boundaries(self):
        compiler=compiler_command()
        if not compiler:self.skipTest('Pinned native compiler required')
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
            binary=root/'fixture.exe';compile(PORT/'tests/native/test_optional_library_windows.c',binary,['-municode','-ladvapi32'])
            def run(path,mode='reject'):
                result=subprocess.run([binary,path,mode],cwd=root,capture_output=True,text=True,timeout=30)
                self.assertEqual(result.returncode,0,result.stdout+result.stderr);self.assertIn('PASS',result.stdout)
                return result.stdout
            self.assertIn('local sibling dependency',run(library,'good'))
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
            self.assertTrue(library.is_file())

if __name__=='__main__':unittest.main()
