// [XCDE] Native Unicode/private-recorder I/O. MIT; upstream recorder licence retained.
#pragma once
#include "../../runtime/platform/windows_save_io.h"
#undef MAX_PATH
#define MAX_PATH 16384
static void cap_module_utf8(char *out, unsigned cap) {
    wchar_t path[4096]; DWORD n=GetModuleFileNameW(NULL,path,4096);
    int bytes=n&&n<4096?WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,path,-1,out,(int)cap,NULL,NULL):0;
    if(!bytes||(unsigned)bytes+128>=cap||!strrchr(out,'\\')) ExitProcess(23);
}
static FILE *cap_open(const char *path,const char *mode) {
    bool write=strchr(mode,'w')!=NULL;
    int fd=ws_open(path,write?O_WRONLY|O_CREAT|O_EXCL:O_RDONLY,0600);
    if(fd<0)return NULL;
    FILE *f=_fdopen(fd,write?"wb":"rb"); if(!f)_close(fd);return f;
}
static bool cap_redirect(const char *path) {
    FILE *f=cap_open(path,"wb");if(!f)return false;
    int ok=!_dup2(_fileno(f),_fileno(stdout));fclose(f);return ok;
}
static BOOL cap_mkdir(const char *path,void *security) {
    (void)security;return ws_mkdir(path,0700)==0;
}
static HMODULE cap_load_local(const char *leaf) {
    if(!leaf||strchr(leaf,'/')||strchr(leaf,'\\')||strchr(leaf,':'))return NULL;
    wchar_t path[4096],name[128];DWORD n=GetModuleFileNameW(NULL,path,4096);
    if(!n||n>=4096||!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,leaf,-1,name,128))return NULL;
    wchar_t *slash=wcsrchr(path,L'\\');if(!slash||(size_t)(slash-path)+wcslen(name)+2>=4096)return NULL;
    wcscpy(slash+1,name);
    return LoadLibraryExW(path,NULL,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
}
#define fopen cap_open
#define CreateDirectoryA cap_mkdir
