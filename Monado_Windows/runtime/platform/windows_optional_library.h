// [XCDE] Native manually supplied optional SDK DLL boundary. No search path,
// download, forced hash sidecar or publisher-authentication claim. Keep exact
// file and ancestors immutable until LoadLibraryExW has completed mapping.
#pragma once
#ifdef _WIN32
#include "windows_save_io.h"
#include <aclapi.h>

static bool monado_optional_owned(HANDLE file) {
    PSID owner=NULL;PSECURITY_DESCRIPTOR descriptor=NULL;HANDLE token=NULL;DWORD size=0;bool valid=false;
    if(GetSecurityInfo(file,SE_FILE_OBJECT,OWNER_SECURITY_INFORMATION,&owner,NULL,NULL,NULL,&descriptor)!=ERROR_SUCCESS)return false;
    if(OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token)) {
        GetTokenInformation(token,TokenUser,NULL,0,&size);
        TOKEN_USER *user=size&&size<=65536?(TOKEN_USER*)malloc(size):NULL;
        if(user&&GetTokenInformation(token,TokenUser,user,size,&size))
            valid=owner&&IsValidSid(owner)&&IsValidSid(user->User.Sid)&&EqualSid(owner,user->User.Sid);
        free(user);CloseHandle(token);
    }
    LocalFree(descriptor);return valid;
}
static bool monado_optional_pe(HANDLE file) {
    LARGE_INTEGER size,where={0};DWORD count=0,offset=0;unsigned char dos[64],header[26];
    if(!GetFileSizeEx(file,&size)||size.QuadPart<90||size.QuadPart>512ll*1024*1024||
       !SetFilePointerEx(file,where,NULL,FILE_BEGIN)||!ReadFile(file,dos,sizeof dos,&count,NULL)||count!=sizeof dos||dos[0]!='M'||dos[1]!='Z')return false;
    memcpy(&offset,dos+60,sizeof offset);
    if(offset<64||(LONGLONG)offset>size.QuadPart-(LONGLONG)sizeof header)return false;
    where.QuadPart=offset;
    if(!SetFilePointerEx(file,where,NULL,FILE_BEGIN)||!ReadFile(file,header,sizeof header,&count,NULL)||count!=sizeof header||memcmp(header,"PE\0\0",4))return false;
    uint16_t machine,optional,flags,magic;
    memcpy(&machine,header+4,2);memcpy(&optional,header+20,2);memcpy(&flags,header+22,2);memcpy(&magic,header+24,2);
    return machine==0x8664&&optional>=2&&(flags&0x2000)&&magic==0x20b;
}
static HMODULE monado_optional_library_load(const wchar_t *path,char *reason,size_t capacity) {
    char utf8[WS_PATH*3];WsGuard guard;
    bool absolute=path&&((wcslen(path)>=3&&path[1]==L':'&&(path[2]==L'\\'||path[2]==L'/'))||(path[0]==L'\\'&&path[1]==L'\\'));
    if(!absolute||!WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,path,-1,utf8,sizeof utf8,NULL,NULL)) {
        snprintf(reason,capacity,"optional SDK DLL requires an explicit absolute Unicode path");return NULL;
    }
    if(ws_guard_open(&guard,utf8)) {snprintf(reason,capacity,"optional SDK DLL ancestors are inaccessible or contain a reparse point");return NULL;}
    HANDLE file=CreateFileW(guard.path,GENERIC_READ|FILE_READ_ATTRIBUTES|READ_CONTROL,FILE_SHARE_READ,NULL,
        OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,NULL);
    if(file==INVALID_HANDLE_VALUE) {
        DWORD error=GetLastError();ws_guard_close(&guard);snprintf(reason,capacity,"optional SDK DLL missing/inaccessible/locked (Windows %lu)",error);return NULL;
    }
    bool valid=GetFileType(file)==FILE_TYPE_DISK&&ws_plain(file,false,true)&&monado_optional_owned(file)&&monado_optional_pe(file);
    if(!valid) {
        CloseHandle(file);ws_guard_close(&guard);snprintf(reason,capacity,"optional SDK DLL must be an ordinary single-link current-user-owned AMD64 PE DLL");return NULL;
    }
    HMODULE module=LoadLibraryExW(guard.path,NULL,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
    DWORD error=GetLastError();CloseHandle(file);ws_guard_close(&guard);
    if(!module){snprintf(reason,capacity,"checked optional SDK DLL could not load (Windows %lu); local SDK siblings/system dependencies required",error);return NULL;}
    snprintf(reason,capacity,"checked local optional SDK DLL mapped");return module;
}
#endif
