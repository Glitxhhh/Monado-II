// Exact production plat_exe_path + owned companion loader, authored code only.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include "platform/windows_module_path.h"
#include "platform.h"
#include "gamelib.h"
static void lexical(void) {
    wchar_t path[1024];
    assert(monado_windows_module_path(path,1024,L"\\\\?\\C:\\owned 雪\\runtime.exe")&&!wcscmp(path,L"C:\\owned 雪\\runtime.exe"));
    assert(monado_windows_module_path(path,1024,L"\\\\?\\UNC\\server\\share\\owned 雪\\runtime.exe")&&!wcscmp(path,L"\\\\server\\share\\owned 雪\\runtime.exe"));
    assert(monado_windows_module_path(path,1024,L"\\\\server\\share\\runtime.exe")&&!wcscmp(path,L"\\\\server\\share\\runtime.exe"));
    assert(monado_windows_module_path(path,1024,L"C:/owned/runtime.exe")&&!wcscmp(path,L"C:\\owned\\runtime.exe"));
    assert(monado_windows_module_mapping_path(path,1024,L"C:\\owned 雪\\runtime.dll")&&!wcscmp(path,L"\\\\?\\C:\\owned 雪\\runtime.dll"));
    assert(monado_windows_module_mapping_path(path,1024,L"\\\\server\\share\\owned 雪\\runtime.dll")&&!wcscmp(path,L"\\\\?\\UNC\\server\\share\\owned 雪\\runtime.dll"));
    const wchar_t *bad[]={L"\\\\?\\GLOBALROOT\\Device\\HarddiskVolume1\\evil.dll",L"\\\\.\\pipe\\evil",L"\\??\\C:\\evil.dll",
        L"\\\\?\\Volume{1234}\\evil.dll",L"\\\\?\\C:relative.dll",L"\\\\?\\C:\\safe\\file.dll:payload",L"C:\\safe\\..\\evil.dll",
        L"C:\\safe\\.\\evil.dll",L"C:\\NUL.dll",L"C:\\CONOUT$",L"C:\\safe \\evil.dll",L"C:\\safe.\\evil.dll",L"C:\\COM1\\evil.dll",
        L"\\\\?\\UNC\\server\\share",L"\\\\?\\C:/safe/evil.dll",L"runtime.exe",L"C:\\safe\\\\runtime.exe"};
    for(unsigned i=0;i<sizeof bad/sizeof *bad;i++)assert(!monado_windows_module_path(path,1024,bad[i]));
    assert(!monado_windows_module_path(path,4,L"C:\\safe\\runtime.exe"));
    wcscpy(path,L"\\\\?\\UNC\\server\\share\\runtime.exe");assert(monado_windows_module_path(path,1024,path)&&!wcscmp(path,L"\\\\server\\share\\runtime.exe"));
}
int main(int argc,char **argv) {
    lexical();assert(argc==2);wchar_t original[32768];DWORD n=GetModuleFileNameW(NULL,original,32768);assert(n>260&&n<32768&&!wcsncmp(original,L"\\\\?\\",4));
    char executable[4096];assert(plat_exe_path(executable,sizeof executable));assert(strncmp(executable,"\\\\?\\",4)&&strlen(executable)>260);
    char why[512];bool expected=!strcmp(argv[1],"good");assert(!GetModuleHandleW(L"lasgame.dll"));assert(gamelib_present(NULL)==expected);
    bool loaded=gamelib_load(NULL,why,sizeof why);if(loaded!=expected)fprintf(stderr,"authored module companion load: %s\n",why);assert(loaded==expected);
    if(expected){const GameLibTables *tables=gamelib_tables();assert(tables&&*tables->main_count==1);Cpu cpu={0};tables->main_fns[0](&cpu);assert(cpu.x[0]==42);}
    puts("PASS production API extended DOS module normalization, owned long Unicode companion identity/load, namespace rejection; UNC lexical only");return 0;
}
