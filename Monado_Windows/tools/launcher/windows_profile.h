// [XCDE] Native launcher runtime/profile separation; no process shell or path resolution through links.
#pragma once
#include "windows_host.h"
#include "../../runtime/platform/windows_save_io.h"
#include <aclapi.h>
namespace monado_launcher_windows {
struct Options {std::string profile,shot,fsr4_assets,fsr4_component;int shot_tab=0;bool shot_precompile=false;};
static bool parse_options(const std::vector<std::string>& args,Options& options,std::string& error) {
    bool profile=false,shot=false;
    for(size_t i=0;i<args.size();i++) {
        const std::string &arg=args[i];
        if(arg=="--profile-root") {
            if(profile||i+1==args.size()||args[i+1].empty()){error="--profile-root requires one absolute existing profile";return false;}
            options.profile=args[++i];profile=true;
        } else if(arg=="--fsr4-assets" || arg=="--fsr4-component") {
            std::string& value=arg=="--fsr4-assets"?options.fsr4_assets:options.fsr4_component;
            if(!value.empty()||i+1==args.size()||args[i+1].empty()){error="FSR4 options require one explicit path each";return false;}
            value=args[++i];
        } else if(arg=="--shot") {
            if(shot||i+1==args.size()||args[i+1].empty()){error="--shot requires one output filename";return false;}
            options.shot=args[++i];shot=true;
            if(i+1<args.size()&&args[i+1].size()==1&&args[i+1][0]>='0'&&args[i+1][0]<='4')options.shot_tab=args[++i][0]-'0';
            if(i+1<args.size()&&args[i+1]=="precompile"){options.shot_precompile=true;i++;}
        } else {error="Unknown launcher argument: "+arg;return false;}
    }
    if(options.fsr4_assets.empty()!=options.fsr4_component.empty()){error="FSR4 requires both --fsr4-assets and --fsr4-component";return false;}
    return true;
}
static bool owned_profile_handle(HANDLE file) {
    PSID owner=nullptr;PSECURITY_DESCRIPTOR descriptor=nullptr;HANDLE token=nullptr;DWORD size=0;bool valid=false;
    if(GetSecurityInfo(file,SE_FILE_OBJECT,OWNER_SECURITY_INFORMATION,&owner,nullptr,nullptr,nullptr,&descriptor)!=ERROR_SUCCESS)return false;
    if(OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token)) {
        GetTokenInformation(token,TokenUser,nullptr,0,&size);
        std::vector<unsigned char> bytes(size&&size<=65536?size:0);
        if(!bytes.empty()&&GetTokenInformation(token,TokenUser,bytes.data(),(DWORD)bytes.size(),&size)) {
            auto user=(TOKEN_USER*)bytes.data();valid=owner&&IsValidSid(owner)&&EqualSid(owner,user->User.Sid);
        }
        CloseHandle(token);
    }
    LocalFree(descriptor);return valid;
}
// Keep profile ancestors plus its log directory locked throughout launcher use.
class ProfileLease {
    WsGuard ancestors{};HANDLE directory=INVALID_HANDLE_VALUE,logs=INVALID_HANDLE_VALUE;
public:
    ~ProfileLease(){if(logs!=INVALID_HANDLE_VALUE)CloseHandle(logs);if(directory!=INVALID_HANDLE_VALUE)CloseHandle(directory);ws_guard_close(&ancestors);}
    bool acquire(const std::string &requested,std::string& canonical,std::string& error) {
        std::wstring value=wide(requested);
        bool absolute=value.size()>=3&&((value[1]==L':'&&(value[2]==L'\\'||value[2]==L'/'))||(value[0]==L'\\'&&value[1]==L'\\'));
        if(!absolute||value.size()>WS_PATH-40){error="Profile root must be an absolute Unicode Windows path";return false;}
        if(ws_guard_open(&ancestors,(requested+"/monado-profile-lease").c_str())){error="Profile ancestors are inaccessible or contain a reparse point";return false;}
        wchar_t *leaf=wcsrchr(ancestors.path,L'\\');if(!leaf){error="Invalid profile root";return false;}*leaf=0;
        directory=CreateFileW(ancestors.path,FILE_READ_ATTRIBUTES|READ_CONTROL,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
        if(directory==INVALID_HANDLE_VALUE||!ws_plain(directory,true,false)||!owned_profile_handle(directory)){error="Profile must be an existing ordinary directory owned by the current Windows user";return false;}
        canonical=utf8(ancestors.path);
        // New profiles need the launcher's append-only logs; a pre-existing
        // build directory is retained and checked, never replaced or cleaned.
        std::wstring logdir=std::wstring(ancestors.path)+L"\\build";
        if(!CreateDirectoryW(logdir.c_str(),nullptr)&&GetLastError()!=ERROR_ALREADY_EXISTS){error="Cannot create profile log directory";return false;}
        logs=CreateFileW(logdir.c_str(),FILE_READ_ATTRIBUTES|READ_CONTROL,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
        if(logs==INVALID_HANDLE_VALUE||!ws_plain(logs,true,false)||!owned_profile_handle(logs)){error="Profile build/log directory is unsafe or owned by another user";return false;}
        return !canonical.empty();
    }
};
static std::string runtime_executable(const std::string& runtime_root){return runtime_root+"/build/xcde.exe";}
} // namespace monado_launcher_windows
