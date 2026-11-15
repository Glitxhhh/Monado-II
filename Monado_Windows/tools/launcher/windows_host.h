// [XCDE] WINPARITY-004: native Unicode paths and explicitly owned child handles.
#pragma once
#include <windows.h>
#include <stdio.h>
#include <io.h>
#include <fcntl.h>
#include <string>
#include <vector>
namespace monado_launcher_windows {
static std::wstring wide(const std::string &text) {
    if(text.empty())return {};
    int size=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),(int)text.size(),nullptr,0);
    if(!size)return {};
    std::wstring result(size,L'\0');
    if(!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),(int)text.size(),result.data(),size))return {};
    return result;
}
static std::string utf8(const wchar_t *text) {
    int length=(int)wcslen(text),size=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text,length,nullptr,0,nullptr,nullptr);
    if(!size)return {};
    std::string result(size,'\0');
    if(!WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text,length,result.data(),size,nullptr,nullptr))return {};
    return result;
}
static FILE *open(const std::string &path,const char *mode) {
    std::wstring name=wide(path),access=wide(mode);
    if(name.empty() || access.empty())return nullptr;
    return _wfopen(name.c_str(),access.c_str());
}
static FILE *open_append(const std::string &path) {
    std::wstring name=wide(path);if(name.empty())return nullptr;
    HANDLE file=CreateFileW(name.c_str(),FILE_APPEND_DATA|FILE_READ_ATTRIBUTES,FILE_SHARE_READ,nullptr,
        OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    if(file==INVALID_HANDLE_VALUE)return nullptr;
    BY_HANDLE_FILE_INFORMATION info{};
    if(!GetFileInformationByHandle(file,&info)||info.nNumberOfLinks!=1||
       (info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))) {
        CloseHandle(file);SetLastError(ERROR_ACCESS_DENIED);return nullptr;
    }
    int descriptor=_open_osfhandle((intptr_t)file,_O_WRONLY|_O_APPEND|_O_BINARY);
    if(descriptor<0){CloseHandle(file);return nullptr;}
    FILE *stream=_fdopen(descriptor,"ab");if(!stream)_close(descriptor);return stream;
}
static std::wstring quote(const std::wstring &arg) {
    std::wstring result=L"\"";size_t slashes=0;
    for(wchar_t ch:arg) {
        if(ch==L'\\'){++slashes;continue;}
        if(ch==L'\"'){result.append(slashes*2+1,L'\\');result+=ch;slashes=0;continue;}
        result.append(slashes,L'\\');slashes=0;result+=ch;
    }
    result.append(slashes*2,L'\\');result+=L'\"';return result;
}
static HANDLE start(const std::string &runtime_root,const std::string &profile_root,const std::vector<std::wstring> &arguments,
                    const char *log_name,DWORD priority=0) {
    std::wstring directory=wide(profile_root),exe=wide(runtime_root+"/build/xcde.exe"),log=wide(profile_root+"/"+log_name);
    if(directory.empty() || exe.empty() || log.empty()){SetLastError(ERROR_NO_UNICODE_TRANSLATION);return nullptr;}
    DWORD attributes=GetFileAttributesW(exe.c_str());
    if(attributes==INVALID_FILE_ATTRIBUTES || (attributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))) {
        SetLastError(ERROR_FILE_NOT_FOUND);return nullptr;
    }
    SECURITY_ATTRIBUTES sa={sizeof sa,nullptr,TRUE};
    HANDLE output=CreateFileW(log.c_str(),FILE_APPEND_DATA,FILE_SHARE_READ,&sa,OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    if(output==INVALID_HANDLE_VALUE)return nullptr;
    BY_HANDLE_FILE_INFORMATION info{};
    if(!GetFileInformationByHandle(output,&info) || info.nNumberOfLinks!=1 ||
       (info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))){
        CloseHandle(output);SetLastError(ERROR_ACCESS_DENIED);return nullptr;
    }
    HANDLE input=CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&sa,OPEN_EXISTING,0,nullptr);
    if(input==INVALID_HANDLE_VALUE){DWORD error=GetLastError();CloseHandle(output);SetLastError(error);return nullptr;}
    SIZE_T bytes=0;InitializeProcThreadAttributeList(nullptr,1,0,&bytes);
    std::vector<unsigned char> storage(bytes);
    auto list=(LPPROC_THREAD_ATTRIBUTE_LIST)storage.data();
    HANDLE handles[]={input,output};
    if(!InitializeProcThreadAttributeList(list,1,0,&bytes)) {
        DWORD error=GetLastError();CloseHandle(input);CloseHandle(output);SetLastError(error);return nullptr;
    }
    bool updated=UpdateProcThreadAttribute(list,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,handles,sizeof handles,nullptr,nullptr);
    STARTUPINFOEXW si{};si.StartupInfo.cb=sizeof si;si.lpAttributeList=list;
    si.StartupInfo.dwFlags=STARTF_USESTDHANDLES;si.StartupInfo.hStdInput=input;
    si.StartupInfo.hStdOutput=si.StartupInfo.hStdError=output;
    std::wstring command=quote(exe);
    for(const auto &arg:arguments)command+=L" "+quote(arg);
    std::vector<wchar_t> buffer(command.begin(),command.end());buffer.push_back(0);
    PROCESS_INFORMATION process{};
    BOOL ok=updated && CreateProcessW(exe.c_str(),buffer.data(),nullptr,nullptr,TRUE,
             EXTENDED_STARTUPINFO_PRESENT|CREATE_NO_WINDOW|priority,nullptr,directory.c_str(),&si.StartupInfo,&process);
    DWORD error=GetLastError();DeleteProcThreadAttributeList(list);CloseHandle(input);CloseHandle(output);
    if(!ok){SetLastError(error);return nullptr;}
    CloseHandle(process.hThread);return process.hProcess;
}
} // namespace monado_launcher_windows
