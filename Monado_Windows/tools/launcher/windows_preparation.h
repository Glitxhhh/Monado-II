// [XCDE] Native optional preparation: bound tools, owned child, no game launch.
#pragma once
#include "windows_json.h"
#include <shobjidl.h>
#include <atomic>
#include <mutex>
#include <thread>
#include <functional>
namespace monado_preparation {
using monado_builder::Handle;using monado_builder::require;
struct Selection {std::wstring sdk,tools,component;};
struct ToolSelection {std::wstring archives,support,cmake,ninja,compiler,vulkan_prefix;};
inline std::vector<wchar_t> environment(){
    wchar_t *block=GetEnvironmentStringsW();require(block!=nullptr,"Cannot read preparation environment");std::vector<std::wstring> values;
    for(auto p=block;*p;p+=wcslen(p)+1){std::wstring value=p,key=value.substr(0,value.find(L'='));for(auto &c:key)c=(wchar_t)towupper(c);if(key.rfind(L"PYTHON",0)!=0&&key!=L"__PYVENV_LAUNCHER__")values.push_back(std::move(value));}FreeEnvironmentStringsW(block);
    values.push_back(L"PYTHONDONTWRITEBYTECODE=1");std::sort(values.begin(),values.end(),[](auto &a,auto &b){return _wcsicmp(a.c_str(),b.c_str())<0;});std::vector<wchar_t> result;for(auto &v:values){result.insert(result.end(),v.begin(),v.end());result.push_back(0);}result.push_back(0);return result;
}
inline bool pick(HWND owner,std::wstring &path,const wchar_t *title,bool folder){
    HRESULT init=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);require(SUCCEEDED(init),"Cannot open the native source picker");IFileOpenDialog *dialog=nullptr;
    HRESULT made=CoCreateInstance(CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&dialog));bool accepted=false;
    if(SUCCEEDED(made)){DWORD flags=0;dialog->GetOptions(&flags);dialog->SetOptions(flags|FOS_FORCEFILESYSTEM|FOS_PATHMUSTEXIST|FOS_FILEMUSTEXIST|(folder?FOS_PICKFOLDERS:0));dialog->SetTitle(title);
        if(!path.empty()){auto slash=path.find_last_of(L"\\/");auto initial=folder?path:path.substr(0,slash);IShellItem *item=nullptr;if(SUCCEEDED(SHCreateItemFromParsingName(initial.c_str(),nullptr,IID_PPV_ARGS(&item)))){dialog->SetFolder(item);item->Release();}if(!folder&&slash!=std::wstring::npos)dialog->SetFileName(path.substr(slash+1).c_str());}
        if(SUCCEEDED(dialog->Show(owner))){IShellItem *item=nullptr;if(SUCCEEDED(dialog->GetResult(&item))){PWSTR value=nullptr;if(SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH,&value))){path=value;CoTaskMemFree(value);accepted=true;}item->Release();}}dialog->Release();}
    CoUninitialize();require(SUCCEEDED(made),"Cannot create the native source picker");return accepted;
}
inline bool choose(HWND owner,Selection &selection){Selection next=selection;if(!pick(owner,next.sdk,L"Choose your local accepted AMD SDK DLL",false)||!pick(owner,next.tools,L"Choose checked native FSR4 preparation tools",true)||!pick(owner,next.component,L"Choose your checked monado-fsr4.dll component",false))return false;selection=std::move(next);return true;}
inline bool choose_tools(HWND owner,ToolSelection &selection,bool vulkan=true){ToolSelection next=selection;
    if(!pick(owner,next.archives,L"Choose your pinned open-source preparation archives",true)||!pick(owner,next.support,L"Choose checked native Vulkan translation support",true)||!pick(owner,next.cmake,L"Choose your local pinned cmake.exe",false)||!pick(owner,next.ninja,L"Choose your local pinned ninja.exe",false)||!pick(owner,next.compiler,L"Choose your local pinned zig.exe compiler",false)||(vulkan&&!pick(owner,next.vulkan_prefix,L"Choose your checked native Vulkan development prefix",true)))return false;selection=std::move(next);return true;}
inline std::array<unsigned char,32> checksum(const std::string &text){require(text.size()==64,"Missing preparation context identity; rebuild with the current native builder");std::array<unsigned char,32> result{};for(size_t i=0;i<64;++i){char c=text[i];require((c>='0'&&c<='9')||(c>='a'&&c<='f'),"Invalid preparation context identity");result[i/2]|=(unsigned char)((c<='9'?c-'0':c-'a'+10)<<(i%2?0:4));}return result;}
inline std::wstring absolute(const std::wstring &path){require(path.size()>=3&&((path[1]==L':'&&(path[2]==L'\\'||path[2]==L'/'))||path.rfind(L"\\\\",0)==0),"Preparation paths must be absolute");std::vector<wchar_t> value(32768);DWORD n=GetFullPathNameW(path.c_str(),(DWORD)value.size(),value.data(),nullptr);require(n&&n<value.size(),"Invalid preparation path");std::wstring result(value.data(),n);std::replace(result.begin(),result.end(),L'/',L'\\');return result;}
struct Context {
    monado_builder::PayloadLease lease;JsonValue value;std::wstring path;
    void load(const std::string &runtime,const std::string &identity,const std::function<void(size_t,size_t)>&progress){
        using namespace monado_builder;auto root=absolute(wide(runtime));lease.begin(root);path=root+L"\\fsr4-preparation-context.json";auto raw=lease.capture(path,16*1024*1024,checksum(identity));value=JsonReader(raw).read();
        require(value.at("schema").integer()==1&&value.text("platform")=="windows-amd64","Unrecognized preparation context");
        require(value.text("status")=="available",value.optional("reason").empty()?"Optional preparation tools are unavailable":value.optional("reason").c_str());
        auto &files=value.at("files");require(files.kind=='o'&&!files.fields.empty()&&files.fields.size()<=30000,"Preparation tool closure is missing or too large");size_t index=0;
        for(auto &file:files.fields){lease.member(absolute(wide(file.first)),file.second.at("bytes").integer(),checksum(file.second.text("sha256")));if(++index%64==0||index==files.fields.size())progress(index,files.fields.size());}
        for(auto key:{"python","worker","pipeline","guard"})require(files.fields.count(value.text(key))==1,"Preparation executable or helper is outside its checked closure");
    }
};
struct Snapshot {bool busy=false;std::string status,message,assets,component,sdk,tools,log,archives,support,cmake,ninja,compiler,vulkan_prefix;uint64_t done=0,total=0;};
class Worker {
    mutable std::mutex mutex;Snapshot state;bool completion_pending=false;std::thread thread;std::atomic<bool> cancel{false},closing{false};std::wstring cancel_path;
    void publish(const Snapshot &next){std::lock_guard<std::mutex> lock(mutex);state=next;}
    void run(std::string runtime,std::string profile,std::string identity,Selection selection,bool verify,bool build_tools,ToolSelection tool){
        using namespace monado_builder;Snapshot next;next.busy=true;next.message="Checking local preparation tools";publish(next);
        try{
            Context context;context.load(runtime,identity,[&](size_t n,size_t total){require(!cancel.load(),"Optional preparation cancelled; current model is unchanged");next.done=n;next.total=total;publish(next);});require(!cancel.load(),"Optional preparation cancelled; current model is unchanged");
            auto root=absolute(wide(profile));context.lease.owned_directory(root);for(auto suffix:{L"\\logs",L"\\logs\\fsr4-preparation"}){directory(root+suffix);context.lease.owned_directory(root+suffix);}
            std::array<unsigned char,32> nonce{};require(BCryptGenRandom(nullptr,nonce.data(),(ULONG)nonce.size(),BCRYPT_USE_SYSTEM_PREFERRED_RNG)==0,"Cannot name preparation attempt");auto id=wide(hex(nonce).substr(0,32));auto logs=root+L"\\logs\\fsr4-preparation";auto progress=logs+L"\\progress-"+id+L".jsonl",log=logs+L"\\worker-"+id+L".log";{std::lock_guard<std::mutex> lock(mutex);cancel_path=logs+L"\\cancel-"+id;}
            FILETIME now{};GetSystemTimeAsFileTime(&now);ULARGE_INTEGER ticks{};ticks.LowPart=now.dwLowDateTime;ticks.HighPart=now.dwHighDateTime;auto output=absolute(wide(context.value.text("installation")))+L"\\fsr4-preparations\\prepare-"+std::to_wstring((ticks.QuadPart-116444736000000000ULL)*100)+L"-"+id.substr(0,8);
            auto python=absolute(wide(context.value.text("python")));std::vector<std::wstring> args={python,L"-I",L"-S",L"-B",wide(context.value.text("worker")),L"--context",context.path,L"--context-sha256",wide(identity),L"--profile",root,L"--sdk",selection.sdk,L"--tools",selection.tools,L"--component",selection.component,L"--output",output,L"--progress",progress,L"--cancel-file",cancel_path};
            if(verify)args={python,L"-I",L"-S",L"-B",wide(context.value.text("worker")),L"--context",context.path,L"--context-sha256",wide(identity),L"--profile",root,L"--verify-selection",L"--progress",progress,L"--cancel-file",cancel_path};
            if(build_tools){auto at=output.find(L"\\fsr4-preparations\\prepare-");require(at!=std::wstring::npos,"Invalid tools output path");output.replace(at,wcslen(L"\\fsr4-preparations\\prepare-"),L"\\fsr4-preparation-tools\\tools-");args={python,L"-I",L"-S",L"-B",wide(context.value.text("worker")),L"--context",context.path,L"--context-sha256",wide(identity),L"--profile",root,L"--build-tools",L"--archives",tool.archives,L"--support",tool.support,L"--cmake",tool.cmake,L"--ninja",tool.ninja,L"--compiler",tool.compiler,L"--vulkan-prefix",tool.vulkan_prefix,L"--jobs",L"4",L"--output",output,L"--progress",progress,L"--cancel-file",cancel_path};}
            std::wstring command;for(auto &arg:args){if(!command.empty())command+=L' ';command+=quote(arg);}std::vector<wchar_t> line(command.begin(),command.end());line.push_back(0);
            SECURITY_ATTRIBUTES sa{sizeof sa,nullptr,TRUE};Handle stream(CreateFileW(native(log).c_str(),GENERIC_WRITE,FILE_SHARE_READ,&sa,CREATE_NEW,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr)),input(CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&sa,OPEN_EXISTING,0,nullptr));require(stream.value!=INVALID_HANDLE_VALUE&&input.value!=INVALID_HANDLE_VALUE,"Cannot create preparation log");
            SIZE_T bytes=0;InitializeProcThreadAttributeList(nullptr,1,0,&bytes);std::vector<unsigned char> storage(bytes);auto list=(LPPROC_THREAD_ATTRIBUTE_LIST)storage.data();require(InitializeProcThreadAttributeList(list,1,0,&bytes),"Cannot initialize preparation process");HANDLE inherited[]={stream.value,input.value};BOOL updated=UpdateProcThreadAttribute(list,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,inherited,sizeof inherited,nullptr,nullptr);STARTUPINFOEXW si{};si.StartupInfo.cb=sizeof si;si.lpAttributeList=list;si.StartupInfo.dwFlags=STARTF_USESTDHANDLES;si.StartupInfo.hStdInput=input.value;si.StartupInfo.hStdOutput=si.StartupInfo.hStdError=stream.value;PROCESS_INFORMATION pi{};
            Handle job(CreateJobObjectW(nullptr,nullptr));JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;require(job.value&&SetInformationJobObject(job.value,JobObjectExtendedLimitInformation,&limits,sizeof limits),"Cannot supervise optional preparation");
            auto env=environment();BOOL started=updated&&CreateProcessW(python.c_str(),line.data(),nullptr,nullptr,TRUE,CREATE_SUSPENDED|CREATE_NO_WINDOW|CREATE_UNICODE_ENVIRONMENT|EXTENDED_STARTUPINFO_PRESENT,env.data(),root.c_str(),&si.StartupInfo,&pi);DeleteProcThreadAttributeList(list);require(started,"Cannot start checked preparation worker");Handle child(pi.hProcess),child_thread(pi.hThread);if(!AssignProcessToJobObject(job.value,child.value)){TerminateProcess(child.value,1);throw std::runtime_error("Cannot own optional preparation worker");}require(ResumeThread(child_thread.value)!=DWORD(-1),"Cannot resume preparation worker");
            next.log=utf8(log);uint64_t offset=0,close_tick=0;std::string pending;bool requested=false,final=false,drained=false;DWORD exit=STILL_ACTIVE;
            do{
                if(cancel.load()&&!requested){Handle request(CreateFileW(native(cancel_path).c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));require(request.value!=INVALID_HANDLE_VALUE||GetLastError()==ERROR_FILE_EXISTS,"Cannot request preparation cancellation");requested=true;}
                if(closing.load()&&!close_tick)close_tick=GetTickCount64();if(close_tick&&GetTickCount64()-close_tick>10000)TerminateJobObject(job.value,ERROR_CANCELLED);
                Handle progress_file(CreateFileW(native(progress).c_str(),GENERIC_READ|READ_CONTROL,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
                if(progress_file.value!=INVALID_HANDLE_VALUE){BY_HANDLE_FILE_INFORMATION attributes{};require(GetFileInformationByHandle(progress_file.value,&attributes)&&attributes.nNumberOfLinks==1&&!(attributes.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))&&PayloadLease::current_owner(progress_file.value),"Preparation progress file is linked or has a different owner");LARGE_INTEGER size{};require(GetFileSizeEx(progress_file.value,&size)&&size.QuadPart>=0&&(uint64_t)size.QuadPart>=offset&&size.QuadPart<=64*1024*1024,"Preparation progress changed or exceeded its bound");seek(progress_file.value,offset);while(offset<(uint64_t)size.QuadPart){size_t count=(size_t)std::min<uint64_t>(4096,(uint64_t)size.QuadPart-offset);std::string part(count,0);read(progress_file.value,part.data(),count);offset+=count;pending+=part;require(pending.size()<=65536,"Preparation progress line exceeded its bound");size_t end;while((end=pending.find('\n'))!=std::string::npos){auto record=JsonReader(pending.substr(0,end)).read();pending.erase(0,end+1);next.status=record.optional("status");next.message=record.optional("message");if(record.fields.count("done"))next.done=record.at("done").integer();if(record.fields.count("total"))next.total=record.at("total").integer();next.assets=record.optional("fsr4_assets");next.component=record.optional("fsr4_component");next.sdk=record.optional("fsr4_sdk");next.tools=record.optional("fsr4_tools");next.archives=record.optional("fsr4_archives");next.support=record.optional("fsr4_support");next.cmake=record.optional("fsr4_cmake");next.ninja=record.optional("fsr4_ninja");next.compiler=record.optional("fsr4_compiler");next.vulkan_prefix=record.optional("fsr4_vulkan_prefix");if(next.status=="success"||next.status=="error"||next.status=="cancelled"||next.status=="needs-setup")final=true;publish(next);}}}
                require(GetExitCodeProcess(child.value,&exit),"Cannot inspect preparation worker");if(exit==STILL_ACTIVE)WaitForSingleObject(child.value,100);else{if(drained)break;drained=true;}
            }while(true);
            require(final&&(!exit||next.status!="success"),"Preparation worker stopped without a verified result; see retained log");
        }catch(const std::exception &error){next.status=cancel.load()?"cancelled":"error";next.message=error.what();}
        next.busy=false;publish(next);
    }
public:
    ~Worker(){cancel=true;closing=true;if(thread.joinable())thread.join();}
    Snapshot snapshot()const{std::lock_guard<std::mutex> lock(mutex);return state;}
    bool take_completion(Snapshot &result){std::lock_guard<std::mutex> lock(mutex);if(!completion_pending||state.busy)return false;result=state;completion_pending=false;return true;}
    bool start(const std::string &runtime,const std::string &profile,const std::string &identity,const Selection &selection,bool verify=false){if(snapshot().busy)return false;if(thread.joinable())thread.join();cancel=false;closing=false;{std::lock_guard<std::mutex> lock(mutex);state={};state.busy=true;completion_pending=true;cancel_path.clear();}thread=std::thread(&Worker::run,this,runtime,profile,identity,selection,verify,false,ToolSelection{});return true;}
    bool start_tools(const std::string &runtime,const std::string &profile,const std::string &identity,const ToolSelection &tools){if(snapshot().busy)return false;if(thread.joinable())thread.join();cancel=false;closing=false;{std::lock_guard<std::mutex> lock(mutex);state={};state.busy=true;completion_pending=true;cancel_path.clear();}thread=std::thread(&Worker::run,this,runtime,profile,identity,Selection{},false,true,tools);return true;}
    void stop(){cancel=true;}
};
}
