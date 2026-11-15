// [XCDE] Explicit local DLSS license review; no vendor download or implicit use.
#pragma once
#include "../launcher/windows_preparation.h"
#include <commctrl.h>
#include <shellapi.h>
#include <memory>
namespace monado_provider_ui {
inline std::wstring dependent_license_text(const std::string &bytes){
    int count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,bytes.data(),(int)bytes.size(),nullptr,0);
    UINT codepage=count?CP_UTF8:1252;if(!count)count=MultiByteToWideChar(codepage,0,bytes.data(),(int)bytes.size(),nullptr,0);
    monado_builder::require(count>0,"Cannot decode the dependent SDK license");std::wstring result((size_t)count,0);
    monado_builder::require(MultiByteToWideChar(codepage,codepage==CP_UTF8?MB_ERR_INVALID_CHARS:0,bytes.data(),(int)bytes.size(),result.data(),count)==count,"Cannot decode the dependent SDK license");return result;
}
enum class SourceChoice {cancel,local,download};
inline SourceChoice source_choice(HWND owner){
    TASKDIALOG_BUTTON buttons[]={{100,L"Download official NVIDIA SDK"},{101,L"Use an existing local SDK"},{IDCANCEL,L"Cancel"}};
    TASKDIALOGCONFIG config{};config.cbSize=sizeof config;config.hwndParent=owner;config.dwFlags=TDF_ALLOW_DIALOG_CANCELLATION|TDF_SIZE_TO_CONTENT;
    config.pszWindowTitle=L"NVIDIA DLSS setup";config.pszMainInstruction=L"Choose the optional DLSS source";
    config.pszContent=L"Download the pinned official Streamline 2.14.16 SDK (263 MiB), or choose your existing SDK. Downloading does not select or enable DLSS. You must separately review NVIDIA's exact license and choose Use before it is staged. No driver settings change.";
    config.cButtons=3;config.pButtons=buttons;config.nDefaultButton=IDCANCEL;int selected=IDCANCEL;
    auto dialog=(decltype(&TaskDialogIndirect))GetProcAddress(GetModuleHandleW(L"comctl32.dll"),"TaskDialogIndirect");
    monado_builder::require(dialog&&SUCCEEDED(dialog(&config,&selected,nullptr,nullptr)),"Cannot display DLSS source choices; no download or provider selection occurred");
    return selected==100?SourceChoice::download:selected==101?SourceChoice::local:SourceChoice::cancel;
}
struct License {
    monado_builder::PayloadLease lease;monado_builder::Handle file;std::wstring path,text;std::vector<monado_builder::Handle> additional_files;std::vector<std::wstring> additional_paths;
    void load(const std::wstring &source){
        using namespace monado_builder;auto root=monado_preparation::absolute(source);lease.begin(root);
        for(auto layout:{L"",L"bin\\",L"bin\\x64\\",L"bin\\x64\\production\\"}){
            auto candidate=root+L"\\"+layout+L"nvngx_dlss.license.txt";
            Handle current(CreateFileW(native(candidate).c_str(),GENERIC_READ|READ_CONTROL,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
            if(current.value==INVALID_HANDLE_VALUE){DWORD error=GetLastError();require(error==ERROR_FILE_NOT_FOUND||error==ERROR_PATH_NOT_FOUND,"Cannot read the selected DLSS license");continue;}
            require(path.empty(),"Multiple DLSS licenses found; choose one ordinary flat SDK folder");lease.parents(candidate);
            BY_HANDLE_FILE_INFORMATION info{};LARGE_INTEGER size{};require(GetFileInformationByHandle(current.value,&info)&&info.nNumberOfLinks==1&&!(info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))&&PayloadLease::current_owner(current.value)&&GetFileSizeEx(current.value,&size)&&size.QuadPart>0&&size.QuadPart<=1024*1024,"DLSS license must be an ordinary owned single-link text file");
            std::string bytes((size_t)size.QuadPart,0);read(current.value,bytes.data(),bytes.size());require(bytes.find('\0')==std::string::npos,"DLSS license contains unsupported text");text=wide(bytes);path=candidate;file=std::move(current);
        }
        monado_builder::require(!path.empty(),"The local SDK is missing nvngx_dlss.license.txt; choose the complete licensed SDK folder");
        for(auto layout:{L"",L"bin\\",L"bin\\x64\\",L"bin\\x64\\production\\"}){
            auto candidate=root+L"\\"+layout+L"reflex.license.txt";
            Handle current(CreateFileW(native(candidate).c_str(),GENERIC_READ|READ_CONTROL,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
            if(current.value==INVALID_HANDLE_VALUE){DWORD error=GetLastError();require(error==ERROR_FILE_NOT_FOUND||error==ERROR_PATH_NOT_FOUND,"Cannot read the dependent NVIDIA SDK license");continue;}
            require(additional_paths.empty(),"Multiple dependent SDK licenses found; choose one ordinary flat SDK folder");lease.parents(candidate);
            BY_HANDLE_FILE_INFORMATION info{};LARGE_INTEGER size{};require(GetFileInformationByHandle(current.value,&info)&&info.nNumberOfLinks==1&&!(info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))&&PayloadLease::current_owner(current.value)&&GetFileSizeEx(current.value,&size)&&size.QuadPart>0&&size.QuadPart<=1024*1024,"Dependent SDK license must be an ordinary owned single-link text file");
            std::string bytes((size_t)size.QuadPart,0);read(current.value,bytes.data(),bytes.size());require(bytes.find('\0')==std::string::npos,"Dependent SDK license contains unsupported text");text+=L"\n\nADDITIONAL NVIDIA VULKAN HELPER LICENSE:\n"+candidate+L"\n\n"+dependent_license_text(bytes);additional_paths.push_back(candidate);additional_files.push_back(std::move(current));
        }
    }
};
template<class Prompt,class Review> std::shared_ptr<License> confirm(const std::wstring &source,Prompt prompt,Review review){
    auto license=std::make_shared<License>();license->load(source);
    for(;;){int answer=prompt(license->path,license->text);if(answer==100)return license;if(answer!=101)return {};review(license->path);for(const auto &path:license->additional_paths)review(path);}
}
inline std::shared_ptr<License> choose(HWND owner,const std::wstring &source){
    return confirm(source,[&](const std::wstring &path,const std::wstring &text){
        TASKDIALOG_BUTTON buttons[]={{100,L"Use local DLSS SDK"},{101,L"Open full license"},{IDCANCEL,L"Cancel"}};TASKDIALOGCONFIG config{};config.cbSize=sizeof config;config.hwndParent=owner;config.dwFlags=TDF_ALLOW_DIALOG_CANCELLATION|TDF_SIZE_TO_CONTENT;config.pszWindowTitle=L"Local NVIDIA DLSS SDK";config.pszMainInstruction=L"Review the NVIDIA DLSS license before use";
        auto content=L"Using this optional SDK and its supplied Vulkan helper is subject to the NVIDIA license agreements listed below. Choose Use only if you agree to all listed terms. Open full license opens each exact local license. No driver settings change.\n\n"+path;
        auto expanded=text;config.pszContent=content.c_str();config.pszExpandedInformation=expanded.c_str();config.pszExpandedControlText=L"Show license text";config.pszCollapsedControlText=L"Hide license text";config.cButtons=3;config.pButtons=buttons;config.nDefaultButton=IDCANCEL;int selected=IDCANCEL;
        auto dialog=(decltype(&TaskDialogIndirect))GetProcAddress(GetModuleHandleW(L"comctl32.dll"),"TaskDialogIndirect");
        monado_builder::require(dialog&&SUCCEEDED(dialog(&config,&selected,nullptr,nullptr)),"Cannot display the local DLSS license; no provider was selected");return selected;
    },[&](const std::wstring &path){monado_builder::require((INT_PTR)ShellExecuteW(owner,L"open",path.c_str(),nullptr,nullptr,SW_SHOWNORMAL)>32,"Cannot open the local DLSS license");});
}
}
