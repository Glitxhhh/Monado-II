// Authored Unicode recorder I/O fixture; no SDK, D3D device or model.
#include <windows.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../third_party/fsr4cap/windows_capture_io.h"
int wmain(int argc,wchar_t **argv){
    assert(argc==2&&SetCurrentDirectoryW(argv[1]));char module[MAX_PATH];cap_module_utf8(module,sizeof module);
    assert(strstr(module,"fsr4")&&strstr(module,".exe"));
    assert(CreateDirectoryA("capture-\xe6\xb8\xac\xe8\xa9\xa6 $() `",NULL));
    const char *name="capture-\xe6\xb8\xac\xe8\xa9\xa6 $() `/trace.txt";
    FILE *file=fopen(name,"wb");assert(file);assert(fwrite("authored\n",1,9,file)==9);assert(!fclose(file));
    assert(!fopen(name,"wb"));file=fopen(name,"rb");assert(file);char data[16]={0};assert(fread(data,1,9,file)==9);assert(!fclose(file));assert(!strcmp(data,"authored\n"));
    assert(CreateHardLinkW(L"hardlink",L"capture-\x6e2c\x8a66 $() `/trace.txt",NULL));assert(!fopen("hardlink","rb"));
    assert(!cap_load_local("../ambient.dll"));puts("PASS native recorder Unicode/singlelink/exclusive-new IO; no GPU/SDK");return 0;
}
