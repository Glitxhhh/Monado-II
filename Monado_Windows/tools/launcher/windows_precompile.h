// [XCDE] Session-bound native shader preparation supervisor; no game startup.
#pragma once
#include "windows_preparation.h"
#include "../../runtime/nvn/vulkan/precompile_status.h"
#include <sstream>
namespace monado_precompile_windows {
using monado_builder::require;
struct Work {
    bool valid=false;std::string phase,state;std::map<std::string,uint64_t> counts;
    uint64_t count(const char *key)const{auto i=counts.find(key);return i==counts.end()?0:i->second;}
    bool complete(DWORD exit)const{return valid&&exit==0&&phase=="done"&&state=="complete"&&count("failed")==0&&count("errors")==0&&count("skipped")==0&&count("files_done")==count("files_total")&&count("programs_done")==count("programs")&&count("seed_done")==count("seed_total")&&count("variants_done")==count("variants_total")&&count("attempted")==count("built");}
};
inline bool hex_token(const std::string &s,size_t n){if(s.size()!=n)return false;for(char c:s)if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')))return false;return true;}
inline Work parse(const std::string &text,const std::string &session,const std::string &identity){
    Work work;if(text.empty()||text.size()>8192||text.back()!='\n'||!hex_token(session,32)||!hex_token(identity,64))return work;
    std::istringstream stream(text);std::string line;if(!std::getline(stream,line)||line!=PRECOMPILE_WORK_MAGIC)return work;std::map<std::string,std::string> fields;
    while(std::getline(stream,line)){auto at=line.find(' ');if(at==std::string::npos||at==0||at>64||line.size()>256||fields.size()>=64)return work;auto key=line.substr(0,at),value=line.substr(at+1);for(char c:key)if(!((c>='a'&&c<='z')||c=='_'))return work;for(char c:value)if(c<33||c>126)return work;if(!fields.emplace(key,value).second)return work;}
    if(fields["session"]!=session||fields["identity"]!=identity||!hex_token(fields["gpu_cache_uuid"],32))return work;
    work.phase=fields["phase"];work.state=fields["state"];
    if(work.phase!="scan"&&work.phase!="build"&&work.phase!="seed"&&work.phase!="pipelines"&&work.phase!="done")return Work{};
    if(work.state!="running"&&work.state!="complete"&&work.state!="partial"&&work.state!="cancelled"&&work.state!="failed")return Work{};
    for(auto key:{"files_total","files_done","shaders","programs","programs_done","seed_total","seed_done","variants_total","variants_done","attempted","built","failed","skipped","unsupported","errors","gpu_vendor","gpu_device","gpu_driver"}){
        auto i=fields.find(key);if(i==fields.end()||i->second.empty())return Work{};uint64_t value=0;for(char c:i->second){if(c<'0'||c>'9'||value>(UINT64_MAX-(c-'0'))/10)return Work{};value=value*10+c-'0';}work.counts[key]=value;
    }work.valid=true;return work;
}
class Session {
    monado_builder::PayloadLease lease;monado_builder::Handle image;std::wstring profile;
public:
    std::string session,identity;
    void begin(const std::string &runtime,const std::string &state){
        using namespace monado_builder;auto root=monado_preparation::absolute(wide(runtime));profile=monado_preparation::absolute(wide(state));lease.begin(root);lease.owned_directory(profile);directory(profile+L"\\cache");lease.owned_directory(profile+L"\\cache");auto executable=root+L"\\build\\xcde.exe";lease.parents(executable);
        image=Handle(CreateFileW(native(executable).c_str(),GENERIC_READ|READ_CONTROL,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));BY_HANDLE_FILE_INFORMATION info{};LARGE_INTEGER size{};
        require(image.value!=INVALID_HANDLE_VALUE&&GetFileInformationByHandle(image.value,&info)&&info.nNumberOfLinks==1&&!(info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))&&PayloadLease::current_owner(image.value)&&GetFileSizeEx(image.value,&size)&&size.QuadPart>0&&size.QuadPart<=2LL*1024*1024*1024,"Shader preparation needs the current owned ordinary game program");
        identity=hex(hash_range(image.value,0,(uint64_t)size.QuadPart));std::array<unsigned char,32> nonce{};require(BCryptGenRandom(nullptr,nonce.data(),(ULONG)nonce.size(),BCRYPT_USE_SYSTEM_PREFERRED_RNG)==0,"Cannot name shader preparation session");session=hex(nonce).substr(0,32);
    }
    Work snapshot()const{
        using namespace monado_builder;auto name=profile+L"\\"+wide(PRECOMPILE_WORK_PATH);std::replace(name.begin(),name.end(),L'/',L'\\');Handle file(CreateFileW(native(name).c_str(),GENERIC_READ|READ_CONTROL,FILE_SHARE_READ|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));BY_HANDLE_FILE_INFORMATION info{};LARGE_INTEGER size{};
        if(file.value==INVALID_HANDLE_VALUE||!GetFileInformationByHandle(file.value,&info)||info.nNumberOfLinks!=1||(info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))||!PayloadLease::current_owner(file.value)||!GetFileSizeEx(file.value,&size)||size.QuadPart<=0||size.QuadPart>8192)return {};
        try{std::string text((size_t)size.QuadPart,0);read(file.value,text.data(),text.size());return parse(text,session,identity);}catch(...){return {};}
    }
    void cancel(){
        using namespace monado_builder;auto name=profile+L"\\"+wide(PRECOMPILE_CANCEL_PREFIX)+wide(session)+L".txt";std::replace(name.begin(),name.end(),L'/',L'\\');auto body=std::string(PRECOMPILE_CANCEL_MAGIC)+"\nsession "+session+"\n";
        Handle file(CreateFileW(native(name).c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
        if(file.value==INVALID_HANDLE_VALUE){require(GetLastError()==ERROR_FILE_EXISTS,"Cannot request shader preparation cancellation");Handle prior(CreateFileW(native(name).c_str(),GENERIC_READ|READ_CONTROL,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));BY_HANDLE_FILE_INFORMATION info{};LARGE_INTEGER size{};require(prior.value!=INVALID_HANDLE_VALUE&&GetFileInformationByHandle(prior.value,&info)&&info.nNumberOfLinks==1&&!(info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))&&PayloadLease::current_owner(prior.value)&&GetFileSizeEx(prior.value,&size)&&size.QuadPart==(LONGLONG)body.size(),"Existing unknown shader cancellation file preserved");std::string old(body.size(),0);read(prior.value,old.data(),old.size());require(old==body,"Existing unknown shader cancellation file preserved");return;}
        write(file.value,body.data(),body.size());require(FlushFileBuffers(file.value),"Cannot flush shader cancellation request");
    }
};
}
