// Actual native launcher start() with an authored production-entry child.
// No game library, window, input or human profile is used.
#include "../../tools/launcher/windows_host.h"
#include <assert.h>
#include <stdio.h>
int wmain(int argc,wchar_t **argv) {
    assert(argc==3);
    using namespace monado_launcher_windows;
    const std::vector<std::wstring> arguments={L"--launch-monitor",L"\\\\.\\DISPLAY2",L"--precompile",
        L"authored target 雪 $() ` quote\" slash\\",L"--session",L"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
        L"--identity",L"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"};
    HANDLE child=start(utf8(argv[1]),utf8(argv[2]),arguments,"build/authored-start.log");
    if(!child)fprintf(stderr,"authored production launcher spawn Windows error %lu\n",(unsigned long)GetLastError());
    assert(child);DWORD wait=WaitForSingleObject(child,20000),code=0;
    if(wait!=WAIT_OBJECT_0){TerminateProcess(child,1);WaitForSingleObject(child,10000);}
    assert(wait==WAIT_OBJECT_0&&GetExitCodeProcess(child,&code));CloseHandle(child);
    assert(code==23);puts("PASS production launcher short-CWD spawn, explicit long profile and literal Unicode arguments");return 0;
}
