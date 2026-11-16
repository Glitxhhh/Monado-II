// [XCDE] Portable native Windows setup frontend. Authored code, MIT license.
#include "payload_windows.h"
#include "../launcher/windows_preparation.h"
#include <commctrl.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <atomic>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

using namespace monado_builder;
#ifndef MONADO_WINDOWS_BASELINE_LABEL
#define MONADO_WINDOWS_BASELINE_LABEL "Windows development (authored component fixture)"
#endif
namespace {
constexpr UINT UPDATE=WM_APP+10,FINISHED=WM_APP+11;
constexpr int INPUT_FIELD=101,DEST=102,INPUT_BROWSE=103,DEST_BROWSE=104,BUILD=105,
    CANCEL=106,OPEN=107,ADOPT=108,LOCATE=109,WORKERS=110,PREPARE=111,UPDATES=112,APPLY_UPDATE=113,FSR4_PREPARE=114,FSR4_TOOLS=115;
HWND window=nullptr,input=nullptr,destination=nullptr,workers=nullptr,prepare=nullptr,phase=nullptr,bar=nullptr,details=nullptr;
HFONT font=nullptr;std::wstring cache,current_install,cancel_path;std::string payload_sha,manifest_sha;std::mutex state_mutex;
std::atomic<bool> busy{false},closing{false},pending_cancel{false};std::thread worker;
bool update_available=false;
bool bootstrap_fixture=false;int fixture_exit=1;
monado_preparation::Selection fsr4_selection;
monado_preparation::ToolSelection fsr4_tool_selection;
struct Event {std::wstring message,status,installation,launcher,log,fsr4_sdk,fsr4_tools,fsr4_component,fsr4_archives,fsr4_support,fsr4_cmake,fsr4_ninja,fsr4_compiler,fsr4_vulkan_prefix;std::vector<std::wstring> arguments;uint64_t done=0,total=0;bool complete=false,open_settings=false;DWORD exit=0;};
bool launcher_arguments(const std::vector<std::wstring> &arguments);
void event(Event *e,UINT kind=UPDATE){if(!PostMessageW(window,kind,0,(LPARAM)e))delete e;}
std::wstring text(HWND h){int n=GetWindowTextLengthW(h);std::wstring r((size_t)n+1,0);GetWindowTextW(h,r.data(),n+1);r.resize(n);return r;}
void append(const std::wstring &s){if(s.empty())return;int n=GetWindowTextLengthW(details);if(n>200000)SetWindowTextW(details,L"");SendMessageW(details,EM_SETSEL,(WPARAM)-1,(LPARAM)-1);std::wstring line=s+L"\r\n";SendMessageW(details,EM_REPLACESEL,FALSE,(LPARAM)line.c_str());}
void controls(bool active){for(int id:{INPUT_FIELD,DEST,INPUT_BROWSE,DEST_BROWSE,BUILD,OPEN,ADOPT,LOCATE,WORKERS,PREPARE,UPDATES})EnableWindow(GetDlgItem(window,id),active);EnableWindow(GetDlgItem(window,APPLY_UPDATE),active&&update_available);for(int id:{FSR4_PREPARE,FSR4_TOOLS})EnableWindow(GetDlgItem(window,id),active&&!current_install.empty());EnableWindow(GetDlgItem(window,CANCEL),!active);}
void payload_progress(uint32_t done,uint32_t total){if(pending_cancel.load())throw std::runtime_error("Setup cancelled; checked cache members are preserved for retry");auto e=new Event;e->message=L"Checking portable tools and source";e->done=done;e->total=total;event(e);}

// The backend protocol is one flat, bounded JSON object per line. Parse every
// string token (including escaped Unicode) before selecting a field, so a path
// or diagnostic cannot masquerade as a protocol key.
struct Json {
    const std::string &s;size_t p=0;std::vector<std::wstring> arguments;
    void space(){while(p<s.size()&&(s[p]==' '||s[p]=='\t'||s[p]=='\r'||s[p]=='\n'))++p;}
    std::string string(){
        require(p<s.size()&&s[p++]=='"',"Malformed setup progress JSON");std::string r;
        while(p<s.size()){
            unsigned char c=(unsigned char)s[p++];if(c=='"')return r;require(c>=32,"Control byte in setup progress");
            if(c!='\\'){r+=(char)c;continue;}require(p<s.size(),"Truncated setup progress escape");char e=s[p++];
            if(e=='"'||e=='\\'||e=='/')r+=e;else if(e=='n')r+='\n';else if(e=='r')r+='\r';else if(e=='t')r+='\t';else if(e=='b')r+='\b';else if(e=='f')r+='\f';
            else if(e=='u'){
                auto word=[&](){require(p+4<=s.size(),"Truncated setup Unicode escape");unsigned v=0;for(int i=0;i<4;++i){char x=s[p++];v<<=4;if(x>='0'&&x<='9')v+=x-'0';else if(x>='a'&&x<='f')v+=x-'a'+10;else if(x>='A'&&x<='F')v+=x-'A'+10;else throw std::runtime_error("Invalid setup Unicode escape");}return v;};
                unsigned a=word();std::wstring w(1,(wchar_t)a);if(a>=0xd800&&a<=0xdbff){require(p+2<=s.size()&&s[p]=='\\'&&s[p+1]=='u',"Missing setup Unicode surrogate");p+=2;unsigned b=word();require(b>=0xdc00&&b<=0xdfff,"Invalid setup Unicode surrogate");w+=(wchar_t)b;}else require(a<0xdc00||a>0xdfff,"Invalid setup Unicode surrogate");r+=utf8(w);
            }else throw std::runtime_error("Invalid setup JSON escape");
        }throw std::runtime_error("Truncated setup JSON string");
    }
    std::map<std::string,std::string> object(){
        std::map<std::string,std::string> r;space();require(p<s.size()&&s[p++]=='{',"Expected setup JSON object");space();if(p<s.size()&&s[p]=='}'){++p;return r;}
        while(p<s.size()){space();auto key=string();space();require(p<s.size()&&s[p++]==':',"Expected setup JSON field");space();std::string value;
            if(p<s.size()&&s[p]=='"')value=string();else if(key=="launcher_args"&&p<s.size()&&s[p]=='['){++p;space();if(p<s.size()&&s[p]==']')++p;else for(;;){space();require(arguments.size()<32,"Too many launcher arguments");arguments.push_back(wide(string()));space();require(p<s.size(),"Truncated launcher arguments");char end=s[p++];if(end==']')break;require(end==',',"Invalid launcher arguments");}}else{size_t start=p;while(p<s.size()&&s[p]!=','&&s[p]!='}')++p;value=s.substr(start,p-start);require(value.find_first_not_of("0123456789 \t\r\n-nulltruefals") == std::string::npos,"Unsupported setup JSON value");}
            require(r.emplace(key,value).second,"Duplicate setup progress field");space();require(p<s.size(),"Truncated setup JSON object");char end=s[p++];if(end=='}')break;require(end==',',"Expected setup JSON separator");
        }space();require(p==s.size(),"Trailing setup progress JSON");return r;
    }
};
uint64_t number(const std::map<std::string,std::string>&v,const char *key){auto i=v.find(key);if(i==v.end()||i->second.empty()||i->second=="null")return 0;uint64_t r=0;for(char c:i->second){if(c==' ')continue;require(c>='0'&&c<='9'&&r<=1000000000,"Invalid setup progress count");r=r*10+c-'0';}return r;}
std::wstring field(const std::map<std::string,std::string>&v,const char *key){auto i=v.find(key);return i==v.end()||i->second=="null"?L"":wide(i->second);}
Event parse(const std::string &line){Json parser{line};auto v=parser.object();Event e;e.arguments=std::move(parser.arguments);e.message=field(v,"message");if(e.message.empty())e.message=field(v,"phase");e.status=field(v,"status");e.installation=field(v,"installation");e.launcher=field(v,"launcher");e.log=field(v,"log");e.done=number(v,"done");e.total=number(v,"total");e.fsr4_sdk=field(v,"fsr4_sdk");e.fsr4_tools=field(v,"fsr4_tools");e.fsr4_component=field(v,"fsr4_component");e.fsr4_archives=field(v,"fsr4_archives");e.fsr4_support=field(v,"fsr4_support");e.fsr4_cmake=field(v,"fsr4_cmake");e.fsr4_ninja=field(v,"fsr4_ninja");e.fsr4_compiler=field(v,"fsr4_compiler");e.fsr4_vulkan_prefix=field(v,"fsr4_vulkan_prefix");return e;}
std::wstring unique(){GUID g{};require(SUCCEEDED(CoCreateGuid(&g)),"Cannot create setup request identity");wchar_t b[40];StringFromGUID2(g,b,40);return b;}
std::vector<wchar_t> clean_environment(){
    wchar_t *block=GetEnvironmentStringsW();require(block!=nullptr,"Cannot read setup process environment");std::vector<std::wstring> values;
    for(auto p=block;*p;p+=wcslen(p)+1){std::wstring value=p,key=value.substr(0,value.find(L'='));for(auto &c:key)c=(wchar_t)towupper(c);if(key.rfind(L"PYTHON",0)!=0&&key!=L"__PYVENV_LAUNCHER__")values.push_back(std::move(value));}FreeEnvironmentStringsW(block);
    values.push_back(L"PYTHONDONTWRITEBYTECODE=1");std::sort(values.begin(),values.end(),[](const auto &a,const auto &b){return _wcsicmp(a.c_str(),b.c_str())<0;});std::vector<wchar_t> result;
    for(auto &v:values){result.insert(result.end(),v.begin(),v.end());result.push_back(0);}result.push_back(0);return result;
}
bool protocol_fixture(){
    auto e=parse(R"({"status":"success","message":"literal \"status\": \"error\"","done":7,"total":9,"launcher":"C:\\authored \\launcher.exe","launcher_args":["--profile-root","C:\\profile \u00fc $() ` \\"]})");
    if(e.status!=L"success"||e.done!=7||e.total!=9||e.arguments.size()!=2||e.arguments[0]!=L"--profile-root"||e.arguments[1]!=LR"(C:\profile ü $() ` \)")return false;
    if(quote(L"C:\\authored path\\")!=L"\"C:\\authored path\\\\\"")return false;
    bool duplicate=false;try{parse(R"({"status":"success","status":"error"})");}catch(...){duplicate=true;}if(!duplicate)return false;
    bool cancelled=false;pending_cancel=true;try{payload_progress(1,2);}catch(...){cancelled=true;}pending_cancel=false;if(!cancelled)return false;
    auto prepared=parse(R"({"status":"success","fsr4_sdk":"C:\\local sdk.dll","fsr4_tools":"C:\\local tools","fsr4_component":"C:\\local component.dll","launcher_args":["--profile-root","C:\\profile","--fsr4-assets","C:\\models","--fsr4-component","C:\\component.dll"]})");
    if(prepared.fsr4_sdk!=L"C:\\local sdk.dll"||!launcher_arguments(prepared.arguments))return false;prepared.arguments[2]=L"--unknown";if(launcher_arguments(prepared.arguments))return false;
    SetEnvironmentVariableW(L"PYTHONPATH",L"authored hostile ambient module path");SetEnvironmentVariableW(L"PYTHONHOME",L"authored hostile ambient home");auto block=clean_environment();
    for(auto p=block.data();*p;p+=wcslen(p)+1){std::wstring value=p;for(auto &c:value)c=(wchar_t)towupper(c);if(value.rfind(L"PYTHONPATH=",0)==0||value.rfind(L"PYTHONHOME=",0)==0)return false;}return true;
}
std::wstring local_cache(){std::vector<wchar_t>b(32768);DWORD n=GetEnvironmentVariableW(L"LOCALAPPDATA",b.data(),(DWORD)b.size());require(n&&n<b.size(),"LOCALAPPDATA is unavailable; an ordinary user cache is required");std::wstring base=final_directory(std::wstring(b.data(),n));directory(base+L"\\MonadoII");directory(base+L"\\MonadoII\\BuilderCache");return base+L"\\MonadoII\\BuilderCache";}
bool choose(std::wstring &out,const wchar_t *title){
    IFileOpenDialog *dialog=nullptr;HRESULT hr=CoCreateInstance(CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&dialog));if(FAILED(hr))return false;
    DWORD flags=0;dialog->GetOptions(&flags);dialog->SetOptions(flags|FOS_PICKFOLDERS|FOS_FORCEFILESYSTEM|FOS_PATHMUSTEXIST);dialog->SetTitle(title);bool ok=false;
    if(SUCCEEDED(dialog->Show(window))){IShellItem *item=nullptr;if(SUCCEEDED(dialog->GetResult(&item))){PWSTR path=nullptr;if(SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH,&path))){out=path;CoTaskMemFree(path);ok=true;}item->Release();}}
    dialog->Release();return ok;
}
bool launcher_arguments(const std::vector<std::wstring> &arguments){return arguments.empty()||((arguments.size()==2||arguments.size()==6)&&arguments[0]==L"--profile-root"&&!arguments[1].empty()&&(arguments.size()==2||(arguments[2]==L"--fsr4-assets"&&!arguments[3].empty()&&arguments[4]==L"--fsr4-component"&&!arguments[5].empty())));}
void start_settings(const std::wstring &path,const std::vector<std::wstring> &arguments){
    require(ordinary(path)&&path.size()>4&&path.substr(path.size()-4)==L".exe","The checked settings launcher is missing");
    require(launcher_arguments(arguments),"Invalid checked settings launcher arguments");
    for(size_t i=1;i<arguments.size();i+=2)monado_preparation::absolute(arguments[i]);
    std::wstring command=quote(path);for(auto &a:arguments)command+=L" "+quote(a);std::vector<wchar_t>b(command.begin(),command.end());b.push_back(0);STARTUPINFOW si{};si.cb=sizeof si;PROCESS_INFORMATION pi{};
    auto slash=path.find_last_of(L"\\/");std::wstring cwd=path.substr(0,slash); // backend returns <install>/build/launcher; runtime cwd is parent of build.
    auto parent=cwd.find_last_of(L"\\/");if(parent!=std::wstring::npos)cwd.resize(parent);
    require(CreateProcessW(path.c_str(),b.data(),nullptr,nullptr,FALSE,0,nullptr,cwd.c_str(),&si,&pi),"Cannot open the checked settings launcher");CloseHandle(pi.hThread);CloseHandle(pi.hProcess);
}
void backend(std::wstring action,std::wstring selected_input,std::wstring selected_dest,int jobs,bool precompile,bool startup,monado_preparation::Selection sources,monado_preparation::ToolSelection tools){
    Event final;final.complete=true;
    try{
        Handle self(CreateFileW(executable().c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));require(self.value!=INVALID_HANDLE_VALUE,"Cannot read portable setup executable");
        PayloadLease immutable;auto payload=inspect(self.value);payload_sha=hex(payload.sha);auto checking=GetTickCount64();materialize(self.value,payload,local_cache(),payload_progress,&immutable);cache=payload.cache;manifest_sha=hex(payload.manifest_sha);require(payload.manifest_sha!=std::array<unsigned char,32>{},"Portable setup manifest is missing");
        fprintf(stderr,"Native bootstrap leases: files=%zu directories=%zu check_ms=%llu\n",immutable.file_count(),immutable.directory_count(),(unsigned long long)(GetTickCount64()-checking));
        if(pending_cancel.load())throw std::runtime_error("Setup cancelled before starting the backend; checked tools are preserved");
        std::wstring id=unique(),progress=cache+L"\\state\\progress-"+id+L".jsonl",log=cache+L"\\state\\backend-"+id+L".log",request_cancel=cache+L"\\state\\cancel-"+id;{std::lock_guard<std::mutex> lock(state_mutex);cancel_path=request_cancel;}
        std::wstring python=cache+L"\\python\\python.exe",script=cache+L"\\setup\\bootstrap_windows.py";require(ordinary(python)&&ordinary(script),"The portable Python or setup backend is missing");
        std::vector<std::wstring> args={python,L"-I",L"-S",L"-B",script,L"--action",action,L"--cache",cache,L"--payload-sha256",wide(payload_sha),L"--manifest-sha256",wide(manifest_sha),L"--progress",progress,L"--cancel-file",request_cancel,L"--jobs",std::to_wstring(jobs)};
        if(!selected_input.empty()){args.push_back(L"--input");args.push_back(selected_input);}if(!selected_dest.empty()){args.push_back(L"--destination");args.push_back(selected_dest);}if(precompile)args.push_back(L"--precompile");
        if(action==L"prepare-fsr4"){args.insert(args.end(),{L"--sdk",sources.sdk,L"--tools",sources.tools,L"--component",sources.component});}
        if(action==L"build-fsr4-tools")args.insert(args.end(),{L"--archives",tools.archives,L"--support",tools.support,L"--cmake",tools.cmake,L"--ninja",tools.ninja,L"--compiler",tools.compiler});
        std::wstring command;for(auto &arg:args){if(!command.empty())command+=L' ';command+=quote(arg);}std::vector<wchar_t>b(command.begin(),command.end());b.push_back(0);
        SECURITY_ATTRIBUTES sa{sizeof sa,nullptr,TRUE};Handle output(CreateFileW(log.c_str(),GENERIC_WRITE,FILE_SHARE_READ,&sa,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));Handle nul(CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&sa,OPEN_EXISTING,0,nullptr));require(output.value!=INVALID_HANDLE_VALUE&&nul.value!=INVALID_HANDLE_VALUE,"Cannot create setup diagnostic log");
        SIZE_T bytes=0;InitializeProcThreadAttributeList(nullptr,1,0,&bytes);std::vector<unsigned char>storage(bytes);auto attributes=(LPPROC_THREAD_ATTRIBUTE_LIST)storage.data();require(InitializeProcThreadAttributeList(attributes,1,0,&bytes),"Cannot initialize owned setup process");
        HANDLE handles[]={output.value,nul.value};BOOL attr=UpdateProcThreadAttribute(attributes,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,handles,sizeof handles,nullptr,nullptr);
        STARTUPINFOEXW si{};si.StartupInfo.cb=sizeof si;si.StartupInfo.dwFlags=STARTF_USESTDHANDLES;si.StartupInfo.hStdInput=nul.value;si.StartupInfo.hStdOutput=si.StartupInfo.hStdError=output.value;si.lpAttributeList=attributes;PROCESS_INFORMATION pi{};
        Handle job(CreateJobObjectW(nullptr,nullptr));JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;require(job.value&&SetInformationJobObject(job.value,JobObjectExtendedLimitInformation,&limits,sizeof limits),"Cannot create owned setup job");
        auto environment=clean_environment();BOOL created=attr&&CreateProcessW(python.c_str(),b.data(),nullptr,nullptr,TRUE,CREATE_SUSPENDED|CREATE_NO_WINDOW|CREATE_UNICODE_ENVIRONMENT|EXTENDED_STARTUPINFO_PRESENT,environment.data(),cache.c_str(),&si.StartupInfo,&pi);DeleteProcThreadAttributeList(attributes);require(created,"Cannot start the bundled setup backend");Handle process(pi.hProcess),thread(pi.hThread);
        if(!AssignProcessToJobObject(job.value,process.value)){TerminateProcess(process.value,1);throw std::runtime_error("Cannot supervise the owned setup backend");}require(ResumeThread(thread.value)!=DWORD(-1),"Cannot resume setup backend");
        uint64_t position=0;std::string pending;DWORD exit=STILL_ACTIVE;bool final_seen=false,drained=false,cancel_sent=false;uint64_t close_tick=0;
        do{
            if(pending_cancel.load()&&!cancel_sent){Handle cancel(CreateFileW(native(request_cancel).c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));require(cancel.value!=INVALID_HANDLE_VALUE||GetLastError()==ERROR_FILE_EXISTS,"Cannot request cancellation of the owned backend");cancel_sent=true;}
            if(closing.load()&&!close_tick){close_tick=GetTickCount64();Handle cancel(CreateFileW(request_cancel.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));}if(close_tick&&GetTickCount64()-close_tick>10000){TerminateJobObject(job.value,ERROR_CANCELLED);}
            Handle progress_file(CreateFileW(progress.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
            if(progress_file.value!=INVALID_HANDLE_VALUE){LARGE_INTEGER size{};require(GetFileSizeEx(progress_file.value,&size)&&size.QuadPart>=0&&(uint64_t)size.QuadPart<=64*1024*1024,"Setup progress exceeded its bounded log");require((uint64_t)size.QuadPart>=position,"Setup progress log changed unexpectedly");seek(progress_file.value,position);uint64_t available=(uint64_t)size.QuadPart-position;
                while(available){size_t n=(size_t)std::min<uint64_t>(available,4096);std::string chunk(n,0);read(progress_file.value,chunk.data(),n);pending+=chunk;position+=n;available-=n;require(pending.size()<=65536,"Setup progress line exceeded its limit");size_t end;
                    while((end=pending.find('\n'))!=std::string::npos){auto line=pending.substr(0,end);pending.erase(0,end+1);if(line.empty())continue;auto e=parse(line);if(e.status==L"success"||e.status==L"error"||e.status==L"cancelled"||e.status==L"needs-setup"||e.status==L"needs-input"||e.status==L"up-to-date"||e.status==L"update-available"){final=e;final_seen=true;}event(new Event(e));}
                }
            }
            require(GetExitCodeProcess(process.value,&exit),"Cannot inspect setup process");if(exit==STILL_ACTIVE)WaitForSingleObject(process.value,100);else{if(drained)break;drained=true;}
        }while(true);
        final.complete=true;final.open_settings=action==L"resume"||action==L"adopt"||action==L"locate-input";final.exit=exit;if(final.log.empty())final.log=log;if(!final_seen){final.status=L"error";final.message=L"Setup stopped before reporting a checked result. See the retained log.";}if(exit&&final.status==L"success"){final.status=L"error";final.message=L"Setup reported success but its process failed. See the retained log.";}
        if(startup&&final.status==L"success"&&!final.launcher.empty())final.message=L"Opening checked settings launcher";
    }catch(const std::exception &e){final.status=pending_cancel.load()?L"cancelled":L"error";final.message=wide(e.what());final.complete=true;}
    event(new Event(final),FINISHED);
}
void launch_action(const std::wstring &action,bool startup=false){
    if(busy.exchange(true))return;if(worker.joinable())worker.join();pending_cancel=false;{std::lock_guard<std::mutex> lock(state_mutex);cancel_path.clear();}controls(false);SetWindowTextW(phase,L"Checking selected installation and tools");SendMessageW(bar,PBM_SETMARQUEE,TRUE,30);
    std::wstring selected_input=text(input),selected_dest=text(destination);if((action==L"locate-input"||action==L"resume"||action==L"check-update"||action==L"update"||action==L"prepare-fsr4"||action==L"build-fsr4-tools")&&!current_install.empty())selected_dest=current_install;
    int jobs=(int)SendMessageW(workers,CB_GETCURSEL,0,0)+1;bool precompile=SendMessageW(prepare,BM_GETCHECK,0,0)==BST_CHECKED;
    worker=std::thread(backend,action,selected_input,selected_dest,std::clamp(jobs,1,6),precompile,startup,fsr4_selection,fsr4_tool_selection);
}
HWND make(const wchar_t *cls,const wchar_t *label,DWORD style,int x,int y,int w,int height,int id=0){HWND handle=CreateWindowExW(0,cls,label,WS_CHILD|WS_VISIBLE|style,x,y,w,height,window,(HMENU)(INT_PTR)id,GetModuleHandleW(nullptr),nullptr);SendMessageW(handle,WM_SETFONT,(WPARAM)font,TRUE);return handle;}
LRESULT CALLBACK procedure(HWND h,UINT message,WPARAM w,LPARAM l){
    try{
        if(message==WM_CREATE){window=h;font=CreateFontW(-16,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
            make(L"STATIC",wide(std::string("Monado II — ")+MONADO_WINDOWS_BASELINE_LABEL).c_str(),0,20,16,690,28);make(L"STATIC",L"Select your extracted DE 2.0.0 folder containing exefs and romfs. Game files stay in place.",0,20,49,690,32);
            input=make(L"EDIT",L"",WS_BORDER|ES_AUTOHSCROLL|WS_TABSTOP,20,85,580,27,INPUT_FIELD);make(L"BUTTON",L"Game files\u2026",WS_TABSTOP,610,84,110,29,INPUT_BROWSE);
            make(L"STATIC",L"Choose an empty installation folder. Existing installations can be adopted below.",0,20,122,690,23);
            destination=make(L"EDIT",L"",WS_BORDER|ES_AUTOHSCROLL|WS_TABSTOP,20,150,580,27,DEST);make(L"BUTTON",L"Install folder\u2026",WS_TABSTOP,610,149,110,29,DEST_BROWSE);
            make(L"STATIC",L"Build workers",0,20,195,100,24);workers=make(L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_TABSTOP,125,191,65,180,WORKERS);for(int i=1;i<=6;++i)SendMessageW(workers,CB_ADDSTRING,0,(LPARAM)std::to_wstring(i).c_str());SendMessageW(workers,CB_SETCURSEL,3,0);
            prepare=make(L"BUTTON",L"Prepare shaders after building (optional)",BS_AUTOCHECKBOX|WS_TABSTOP,215,191,400,28,PREPARE);
            make(L"BUTTON",L"Build installation",WS_TABSTOP|BS_DEFPUSHBUTTON,20,233,165,34,BUILD);make(L"BUTTON",L"Cancel",WS_TABSTOP,195,233,90,34,CANCEL);make(L"BUTTON",L"Open settings",WS_TABSTOP,295,233,130,34,OPEN);
            make(L"BUTTON",L"Prepare FSR4 Lite (optional)\u2026",WS_TABSTOP,435,233,285,34,FSR4_PREPARE);
            make(L"BUTTON",L"Use an existing installation\u2026",WS_TABSTOP,20,279,225,30,ADOPT);make(L"BUTTON",L"Locate game files\u2026",WS_TABSTOP,255,279,180,30,LOCATE);make(L"BUTTON",L"Check for updates",WS_TABSTOP,445,279,170,30,UPDATES);make(L"BUTTON",L"Update",WS_TABSTOP,625,279,95,30,APPLY_UPDATE);
            make(L"BUTTON",L"Build optional FSR4 preparation tools\u2026",WS_TABSTOP,20,320,340,30,FSR4_TOOLS);
            phase=make(L"STATIC",L"Starting portable setup",0,20,362,700,25);bar=make(PROGRESS_CLASSW,L"",PBS_MARQUEE,20,393,700,20);
            details=make(L"EDIT",L"",WS_BORDER|ES_MULTILINE|ES_AUTOVSCROLL|ES_READONLY|WS_VSCROLL,20,426,700,185);controls(true);return 0;
        }
        if(message==WM_COMMAND){int id=LOWORD(w);std::wstring path;if(id==INPUT_BROWSE&&choose(path,L"Choose extracted game files (exefs and romfs)"))SetWindowTextW(input,path.c_str());else if(id==DEST_BROWSE&&choose(path,L"Choose an empty final installation folder"))SetWindowTextW(destination,path.c_str());
            else if(id==BUILD)launch_action(L"build");else if(id==OPEN)launch_action(L"resume");else if(id==UPDATES)launch_action(L"check-update");else if(id==APPLY_UPDATE&&update_available)launch_action(L"update");
            else if(id==ADOPT&&choose(path,L"Choose an existing Monado II Windows installation")){SetWindowTextW(destination,path.c_str());launch_action(L"adopt");}
            else if(id==LOCATE&&choose(path,L"Locate the same extracted game files")){SetWindowTextW(input,path.c_str());launch_action(L"locate-input");}
            else if(id==FSR4_PREPARE&&!current_install.empty()&&!busy.load()&&monado_preparation::choose(window,fsr4_selection))launch_action(L"prepare-fsr4");
            else if(id==FSR4_TOOLS&&!current_install.empty()&&!busy.load()&&monado_preparation::choose_tools(window,fsr4_tool_selection,false))launch_action(L"build-fsr4-tools");
            else if(id==CANCEL&&busy.load()){pending_cancel=true;std::wstring target;{std::lock_guard<std::mutex> lock(state_mutex);target=cancel_path;}if(!target.empty()){Handle f(CreateFileW(native(target).c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));}append(L"Cancellation requested. Existing installation and logs are preserved.");}return 0;
        }
        if(message==UPDATE||message==FINISHED){std::unique_ptr<Event> e((Event*)l);if(!e->message.empty()){SetWindowTextW(phase,e->message.c_str());append(e->message);}if(e->total){SendMessageW(bar,PBM_SETMARQUEE,FALSE,0);SendMessageW(bar,PBM_SETRANGE32,0,1000);SendMessageW(bar,PBM_SETPOS,(WPARAM)std::min<uint64_t>(1000,e->done*1000/e->total),0);}
            if(!e->installation.empty())current_install=e->installation;
            if(!e->fsr4_sdk.empty())fsr4_selection.sdk=e->fsr4_sdk;if(!e->fsr4_tools.empty())fsr4_selection.tools=e->fsr4_tools;if(!e->fsr4_component.empty())fsr4_selection.component=e->fsr4_component;if(!e->fsr4_archives.empty())fsr4_tool_selection.archives=e->fsr4_archives;if(!e->fsr4_support.empty())fsr4_tool_selection.support=e->fsr4_support;if(!e->fsr4_cmake.empty())fsr4_tool_selection.cmake=e->fsr4_cmake;if(!e->fsr4_ninja.empty())fsr4_tool_selection.ninja=e->fsr4_ninja;if(!e->fsr4_compiler.empty())fsr4_tool_selection.compiler=e->fsr4_compiler;if(!e->fsr4_vulkan_prefix.empty())fsr4_tool_selection.vulkan_prefix=e->fsr4_vulkan_prefix;
            if(message==FINISHED){busy=false;if(e->status==L"update-available")update_available=true;else if(e->status==L"up-to-date"||e->status==L"success")update_available=false;controls(true);SendMessageW(bar,PBM_SETMARQUEE,FALSE,0);if(!e->log.empty())append(L"Log: "+e->log);
                if(bootstrap_fixture){fixture_exit=e->status==L"needs-setup"?0:1;if(fixture_exit)fwprintf(stderr,L"Portable bootstrap fixture: %ls\n",e->message.c_str());DestroyWindow(window);}
                else{if(!closing.load()&&e->open_settings&&!e->launcher.empty()&&e->status==L"success"){start_settings(e->launcher,e->arguments);PostMessageW(window,WM_CLOSE,0,0);}if(closing.load())DestroyWindow(window);}}
            return 0;
        }
        if(message==WM_CLOSE){if(busy.load()){closing=true;SendMessageW(window,WM_COMMAND,CANCEL,0);SetWindowTextW(phase,L"Cancelling owned setup work before closing\u2026");return 0;}DestroyWindow(h);return 0;}
        if(message==WM_DESTROY){PostQuitMessage(0);return 0;}
    }catch(const std::exception &e){MessageBoxW(h,wide(e.what()).c_str(),L"Monado II setup",MB_OK|MB_ICONERROR);}
    return DefWindowProcW(h,message,w,l);
}
} // namespace

int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int show){
    CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);INITCOMMONCONTROLSEX common{sizeof common,ICC_PROGRESS_CLASS};InitCommonControlsEx(&common);
    int argc=0;auto argv=CommandLineToArgvW(GetCommandLineW(),&argc);
    // Authored non-game fixture: exercise the actual native payload reader with
    // a synthetic executable bundle and caller-owned temporary cache only.
    if(argc==4&&!wcscmp(argv[1],L"--check-payload")){
        int result=0;try{Handle f(CreateFileW(argv[2],GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));require(f.value!=INVALID_HANDLE_VALUE,"Missing authored payload fixture");auto p=inspect(f.value);materialize(f.value,p,argv[3]);}catch(...){result=1;}LocalFree(argv);CoUninitialize();return result;
    }
    bool fixture=argc==2&&!wcscmp(argv[1],L"--hidden-ui-fixture");bootstrap_fixture=argc==2&&!wcscmp(argv[1],L"--hidden-bootstrap-fixture");LocalFree(argv);
    WNDCLASSW cls{};cls.lpfnWndProc=procedure;cls.hInstance=instance;cls.lpszClassName=L"MonadoIIPortableBuilder";cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);cls.hbrBackground=(HBRUSH)(COLOR_WINDOW+1);RegisterClassW(&cls);
    HWND h=CreateWindowExW(0,cls.lpszClassName,L"Monado II \u2014 Windows setup",WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,CW_USEDEFAULT,CW_USEDEFAULT,760,670,nullptr,nullptr,instance,nullptr);if(!h)return 1;
    if(fixture){bool ok=false;try{ok=GetDlgItem(h,INPUT_FIELD)&&GetDlgItem(h,DEST)&&GetDlgItem(h,BUILD)&&GetDlgItem(h,FSR4_PREPARE)&&!IsWindowEnabled(GetDlgItem(h,FSR4_PREPARE))&&GetDlgItem(h,FSR4_TOOLS)&&!IsWindowEnabled(GetDlgItem(h,FSR4_TOOLS))&&SendMessageW(prepare,BM_GETCHECK,0,0)==BST_UNCHECKED&&SendMessageW(workers,CB_GETCOUNT,0,0)==6&&protocol_fixture();}catch(...){ok=false;}DestroyWindow(h);if(font)DeleteObject(font);CoUninitialize();return ok?0:1;}
    ShowWindow(h,bootstrap_fixture?SW_HIDE:show);UpdateWindow(h);launch_action(L"resume",true);MSG msg{};while(GetMessageW(&msg,nullptr,0,0)>0){if(!IsDialogMessageW(h,&msg)){TranslateMessage(&msg);DispatchMessageW(&msg);}}
    if(worker.joinable())worker.join();if(font)DeleteObject(font);CoUninitialize();return bootstrap_fixture?fixture_exit:0;
}
