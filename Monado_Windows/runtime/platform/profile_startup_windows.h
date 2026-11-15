// Authored native startup profile lease. CreateProcessW cannot use a long CWD;
// apply the explicit checked profile inside the child before runtime work.
#pragma once
#include "windows_save_io.h"
#include <aclapi.h>

static WsGuard startup_profile_ancestors;
static HANDLE startup_profile_directory=INVALID_HANDLE_VALUE;
static bool startup_profile_owned(HANDLE directory) {
    PSID owner=NULL;PSECURITY_DESCRIPTOR security=NULL;HANDLE token=NULL;
    DWORD size=0;bool valid=false;
    if(GetSecurityInfo(directory,SE_FILE_OBJECT,OWNER_SECURITY_INFORMATION,&owner,NULL,NULL,NULL,&security)!=ERROR_SUCCESS)return false;
    if(OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token)) {
        GetTokenInformation(token,TokenUser,NULL,0,&size);
        void *bytes=size&&size<=65536?malloc(size):NULL;
        if(bytes&&GetTokenInformation(token,TokenUser,bytes,size,&size)) {
            TOKEN_USER *user=bytes;valid=owner&&IsValidSid(owner)&&EqualSid(owner,user->User.Sid);
        }
        free(bytes);CloseHandle(token);
    }
    LocalFree(security);return valid;
}
static bool startup_profile_apply(const char *profile) {
    wchar_t requested[WS_PATH];char probe[WS_PATH*4];
    if(startup_profile_directory!=INVALID_HANDLE_VALUE||!profile||!*profile||
       !MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,profile,-1,requested,WS_PATH)) {
        SetLastError(ERROR_INVALID_PARAMETER);return false;
    }
    size_t length=wcslen(requested);
    if(length<3||!((requested[1]==L':'&&(requested[2]==L'\\'||requested[2]==L'/'))||
                  (requested[0]==L'\\'&&requested[1]==L'\\'))) {
        SetLastError(ERROR_BAD_PATHNAME);return false;
    }
    int count=snprintf(probe,sizeof probe,"%s/monado-profile-lease",profile);
    if(count<0||(size_t)count>=sizeof probe||ws_guard_open(&startup_profile_ancestors,probe)) {
        SetLastError(ERROR_ACCESS_DENIED);return false;
    }
    wchar_t *leaf=wcsrchr(startup_profile_ancestors.path,L'\\');
    if(!leaf){ws_guard_close(&startup_profile_ancestors);SetLastError(ERROR_BAD_PATHNAME);return false;}
    *leaf=0;
    HANDLE directory=CreateFileW(startup_profile_ancestors.path,FILE_READ_ATTRIBUTES|READ_CONTROL,
        FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,NULL);
    bool valid=directory!=INVALID_HANDLE_VALUE&&ws_plain(directory,true,false)&&startup_profile_owned(directory);
    if(!valid||!SetCurrentDirectoryW(startup_profile_ancestors.path)) {
        DWORD error=valid?GetLastError():ERROR_ACCESS_DENIED;
        if(directory!=INVALID_HANDLE_VALUE)CloseHandle(directory);
        ws_guard_close(&startup_profile_ancestors);SetLastError(error);return false;
    }
    // Process lifetime: prevent profile/ancestor rename throughout runtime work.
    // The OS releases all handles on exit; no file or user data is removed.
    startup_profile_directory=directory;return true;
}
