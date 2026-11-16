// [XCDE] Native cache/status transactions. MIT; ordinary owned files only.
// FlushFileBuffers + write-through rename; no POSIX directory-fsync claim.
// The path mutex serializes cooperating writers. Win32 has no expected-file-ID
// compare-and-swap against an arbitrary malicious process with the same token.
#pragma once
#include "windows_library_identity.h"
#include <wctype.h>
#ifndef _CRT_RAND_S
#ifdef __cplusplus
extern "C" int __cdecl rand_s(unsigned int *);
#else
int __cdecl rand_s(unsigned int *);
#endif
#endif
// Explicit extended-length spelling removes reliance on the caller's manifest.
// This is a normal drive/UNC path only: device names and alternate streams are refused.
static bool wct_absolute(const char *path,wchar_t output[WS_PATH]) {
    wchar_t input[WS_PATH],normal[WS_PATH];
    if(!path||!*path||!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,path,-1,input,WS_PATH)||
        !wcsncmp(input,L"\\\\?\\",4)||!wcsncmp(input,L"\\\\.\\",4))return false;
    DWORD n=GetFullPathNameW(input,WS_PATH,normal,NULL);if(!n||n>=WS_PATH-8)return false;
    // A native extended-length current directory may be reflected in relative results.
    if(!wcsncmp(normal,L"\\\\?\\UNC\\",8)){normal[6]=L'\\';memmove(normal,normal+6,(wcslen(normal+6)+1)*sizeof(wchar_t));}
    else if(!wcsncmp(normal,L"\\\\?\\",4))memmove(normal,normal+4,(wcslen(normal+4)+1)*sizeof(wchar_t));
    for(wchar_t *p=normal;*p;p++)if(*p==L'/')*p=L'\\';
    size_t root=3;
    if(normal[0]==L'\\'&&normal[1]==L'\\'){
        wchar_t *server=wcschr(normal+2,L'\\'),*share=server?wcschr(server+1,L'\\'):NULL;
        if(!share)return false;root=(size_t)(share-normal)+1;
        swprintf(output,WS_PATH,L"\\\\?\\UNC\\%ls",normal+2);
    }else{
        if(normal[1]!=L':'||normal[2]!=L'\\')return false;
        swprintf(output,WS_PATH,L"\\\\?\\%ls",normal);
    }
    return wcschr(normal+root,L':')==NULL;
}
static int wct_guard_open(WsGuard *guard,const char *path) {
    memset(guard,0,sizeof *guard);
    if(!wct_absolute(path,guard->path))return errno=EINVAL,-1;
    size_t root=7,length=wcslen(guard->path);
    if(!wcsncmp(guard->path,L"\\\\?\\UNC\\",8)){
        wchar_t *server=wcschr(guard->path+8,L'\\'),*share=server?wcschr(server+1,L'\\'):NULL;
        if(!share)return errno=EINVAL,-1;root=(size_t)(share-guard->path)+1;
    }
    for(size_t i=root;i<length;i++)if(guard->path[i]==L'\\'){
        if(guard->count==WS_DEPTH){ws_guard_close(guard);return errno=ENAMETOOLONG,-1;}
        guard->path[i]=0;
        HANDLE h=CreateFileW(guard->path,FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,NULL);
        guard->path[i]=L'\\';
        if(h==INVALID_HANDLE_VALUE){DWORD error=GetLastError();ws_guard_close(guard);return ws_error(error);}
        if(!ws_plain(h,true,false)){CloseHandle(h);ws_guard_close(guard);return -1;}
        guard->parents[guard->count++]=h;
    }
    return 0;
}
static HANDLE wct_owned_file(const wchar_t *path,DWORD access,DWORD share,DWORD creation) {
    HANDLE h=CreateFileW(path,access|READ_CONTROL,share,NULL,creation,FILE_FLAG_OPEN_REPARSE_POINT,NULL);
    if(h!=INVALID_HANDLE_VALUE&&(!ws_plain(h,false,true)||GetFileType(h)!=FILE_TYPE_DISK||!wl_owned(h))){CloseHandle(h);SetLastError(ERROR_ACCESS_DENIED);return INVALID_HANDLE_VALUE;}
    if(h==INVALID_HANDLE_VALUE)ws_error(GetLastError());return h;
}
static bool wct_directory(const char *path) {
    WsGuard guard;if(wct_guard_open(&guard,path))return false;
    if(!CreateDirectoryW(guard.path,NULL)&&GetLastError()!=ERROR_ALREADY_EXISTS){ws_guard_close(&guard);return false;}
    HANDLE h=CreateFileW(guard.path,FILE_READ_ATTRIBUTES|READ_CONTROL,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,
        OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,NULL);
    bool ok=h!=INVALID_HANDLE_VALUE&&ws_plain(h,true,false)&&wl_owned(h);
    if(h!=INVALID_HANDLE_VALUE)CloseHandle(h);ws_guard_close(&guard);return ok;
}
static bool wct_write(const char *path,const void *data,size_t size,const char *old_magic,size_t old_limit) {
    WsGuard guard;HANDLE mutex=NULL,old=INVALID_HANDLE_VALUE,temp=INVALID_HANDLE_VALUE;
    bool ok=false,existed=false;wchar_t name[96],temporary[WS_PATH]={0};
    uint64_t hash=1469598103934665603ull;DWORD waited=WAIT_FAILED;LARGE_INTEGER prior;
    size_t done=0;unsigned random=0;int length;
    if(!path||(!data&&size)||size>1024ull*1024*1024||wct_guard_open(&guard,path))return false;
    for(const wchar_t *p=guard.path;*p;p++){hash^=(uint16_t)towlower(*p);hash*=1099511628211ull;}
    swprintf(name,96,L"Local\\MonadoIICache%016llx",(unsigned long long)hash);
    mutex=CreateMutexW(NULL,FALSE,name);waited=mutex?WaitForSingleObject(mutex,3000):WAIT_FAILED;
    if(waited!=WAIT_OBJECT_0&&waited!=WAIT_ABANDONED)goto done;
    old=wct_owned_file(guard.path,GENERIC_READ,FILE_SHARE_READ,OPEN_EXISTING);
    if(old==INVALID_HANDLE_VALUE){if(GetLastError()!=ERROR_FILE_NOT_FOUND)goto done;}
    else {
        existed=true;
        if(!GetFileSizeEx(old,&prior)||prior.QuadPart<0||(uint64_t)prior.QuadPart>old_limit)goto done;
        if(old_magic){char magic[128];DWORD got=0;size_t n=strlen(old_magic);
            if(n>sizeof magic||!ReadFile(old,magic,(DWORD)n,&got,NULL)||got!=n||memcmp(magic,old_magic,n))goto done;}
    }
    for(unsigned attempt=0;attempt<32;attempt++){
        if(rand_s(&random))goto done;
        length=swprintf(temporary,WS_PATH,L"%ls.monado-%lu-%08x.tmp",guard.path,GetCurrentProcessId(),random);
        if(length<0||length>=WS_PATH)goto done;
        temp=wct_owned_file(temporary,GENERIC_WRITE|DELETE,FILE_SHARE_READ,CREATE_NEW);
        if(temp!=INVALID_HANDLE_VALUE||GetLastError()!=ERROR_FILE_EXISTS)break;
    }
    if(temp==INVALID_HANDLE_VALUE)goto done;
    while(done<size){DWORD put=0,count=(DWORD)(size-done>1024*1024?1024*1024:size-done);
        if(!WriteFile(temp,(const char*)data+done,count,&put,NULL)||put!=count)goto done;done+=put;}
    if(!FlushFileBuffers(temp))goto done;
    if(!CloseHandle(temp)){temp=INVALID_HANDLE_VALUE;goto done;}temp=INVALID_HANDLE_VALUE;
    if(old!=INVALID_HANDLE_VALUE){CloseHandle(old);old=INVALID_HANDLE_VALUE;}
    for(unsigned attempt=0;attempt<20;attempt++){
        ok=MoveFileExW(temporary,guard.path,MOVEFILE_WRITE_THROUGH|(existed?MOVEFILE_REPLACE_EXISTING:0))!=0;
        if(ok||GetLastError()!=ERROR_SHARING_VIOLATION)break;Sleep(10);
    }
done:
    if(temp!=INVALID_HANDLE_VALUE){FILE_DISPOSITION_INFO remove={TRUE};SetFileInformationByHandle(temp,FileDispositionInfo,&remove,sizeof remove);CloseHandle(temp);}
    if(old!=INVALID_HANDLE_VALUE)CloseHandle(old);
    if(mutex){if(waited==WAIT_OBJECT_0||waited==WAIT_ABANDONED)ReleaseMutex(mutex);CloseHandle(mutex);}
    ws_guard_close(&guard);return ok;
}
static FILE *wct_read(const char *path,size_t maximum) {
    WsGuard guard;if(wct_guard_open(&guard,path))return NULL;
    HANDLE h=wct_owned_file(guard.path,GENERIC_READ,FILE_SHARE_READ,OPEN_EXISTING);LARGE_INTEGER size;
    bool ok=h!=INVALID_HANDLE_VALUE&&GetFileSizeEx(h,&size)&&size.QuadPart>=0&&(uint64_t)size.QuadPart<=maximum;
    ws_guard_close(&guard);if(!ok){if(h!=INVALID_HANDLE_VALUE)CloseHandle(h);return NULL;}
    int fd=_open_osfhandle((intptr_t)h,O_RDONLY|O_BINARY);
    if(fd<0){CloseHandle(h);return NULL;}FILE *f=_fdopen(fd,"rb");if(!f)_close(fd);return f;
}
