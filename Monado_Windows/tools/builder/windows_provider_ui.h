// [XCDE] Explicit local DLSS license review; no vendor download or implicit use.
#pragma once
#include "../launcher/windows_preparation.h"
#include <commctrl.h>
#include <shellapi.h>
#include <memory>
namespace monado_provider_ui {
struct License {
    monado_builder::PayloadLease lease;monado_builder::Handle file;std::wstring path,text;
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
    }
};
template<class Prompt,class Review> std::shared_ptr<License> confirm(const std::wstring &source,Prompt prompt,Review review){
    auto license=std::make_shared<License>();license->load(source);
    for(;;){int answer=prompt(license->path,license->text);if(answer==100)return license;if(answer!=101)return {};review(license->path);}
}
inline std::shared_ptr<License> choose(HWND owner,const std::wstring &source){
    return confirm(source,[&](const std::wstring &path,const std::wstring &text){
        TASKDIALOG_BUTTON buttons[]={{100,L"Use local DLSS SDK"},{101,L"Open full license"},{IDCANCEL,L"Cancel"}};TASKDIALOGCONFIG config{};config.cbSize=sizeof config;config.hwndParent=owner;config.dwFlags=TDF_ALLOW_DIALOG_CANCELLATION|TDF_SIZE_TO_CONTENT;config.pszWindowTitle=L"Local NVIDIA DLSS SDK";config.pszMainInstruction=L"Review the NVIDIA DLSS license before use";
        auto content=L"Using this optional SDK is subject to NVIDIA's license. Choose Use only if you agree to its terms. No files are downloaded and no driver settings change.\n\n"+path;
        auto expanded=text.substr(0,32768);config.pszContent=content.c_str();config.pszExpandedInformation=expanded.c_str();config.pszExpandedControlText=L"Show license text";config.pszCollapsedControlText=L"Hide license text";config.cButtons=3;config.pButtons=buttons;config.nDefaultButton=IDCANCEL;int selected=IDCANCEL;
        auto dialog=(decltype(&TaskDialogIndirect))GetProcAddress(GetModuleHandleW(L"comctl32.dll"),"TaskDialogIndirect");
        monado_builder::require(dialog&&SUCCEEDED(dialog(&config,&selected,nullptr,nullptr)),"Cannot display the local DLSS license; no provider was selected");return selected;
    },[&](const std::wstring &path){monado_builder::require((INT_PTR)ShellExecuteW(owner,L"open",path.c_str(),nullptr,nullptr,SW_SHOWNORMAL)>32,"Cannot open the local DLSS license");});
}
}
