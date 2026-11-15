// [XCDE] File availability only: never load an SDK or call staged files active.
#pragma once
#include "windows_preparation.h"
namespace monado_provider_status {
inline std::string dlss(const std::string &runtime){
    using namespace monado_builder;
    try{
        auto root=monado_preparation::absolute(wide(runtime));PayloadLease lease;lease.begin(root);
        for(auto name:{L"sl.interposer.dll",L"sl.common.dll",L"sl.dlss.dll",L"nvngx_dlss.dll"}){
            auto path=root+L"\\build\\"+name;lease.parents(path);
            Handle file(CreateFileW(native(path).c_str(),FILE_READ_ATTRIBUTES|READ_CONTROL,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
            BY_HANDLE_FILE_INFORMATION info{};
            if(file.value==INVALID_HANDLE_VALUE||!GetFileInformationByHandle(file.value,&info)||
               info.nNumberOfLinks!=1||(info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))||!PayloadLease::current_owner(file.value))
                return "DLSS unavailable in this runtime: missing, unsafe or inaccessible "+utf8(name)+". Open graphics SDK management to download the official SDK or select a local licensed source. FSR3 is used when available; otherwise spatial scaling.";
        }
        return "All four local DLSS files are present. This is not an active-provider check. Restart the game to validate the selected RTX GPU, SDK, surface and frame dispatch; F1 shows the actual provider and failure reason.";
    }catch(const std::exception &error){return std::string("DLSS files cannot be checked: ")+error.what()+". No DLSS availability is claimed.";}
}
}
