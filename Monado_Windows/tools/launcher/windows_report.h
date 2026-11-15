// [XCDE] WINPARITY-004: native bounded support ZIP; no shell or uploads.
// ZIP serialization adapts the authored accepted Linux reporter (MIT).
#pragma once
#include "windows_host.h"
#include "../../runtime/platform/windows_save_io.h"
#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>
namespace monado_report {
struct Entry { std::string name, data; uint32_t crc=0, offset=0; };
static uint32_t crc32(const std::string &s) {
    uint32_t c=~0u;for(unsigned char b:s){c^=b;for(int i=0;i<8;i++)c=(c>>1)^(0xedb88320u&uint32_t(-int(c&1)));}return ~c;
}
static void le(std::string &s,uint32_t n,int bytes) {for(int i=0;i<bytes;i++)s.push_back(char(n>>(8*i)));}

static bool write_all(int fd,const std::string &data) {
    size_t offset=0;
    while(offset<data.size()) {
        unsigned count=(unsigned)std::min<size_t>(data.size()-offset,1048576);
        int written=_write(fd,data.data()+offset,count);
        if(written<0 && errno==EINTR)continue;
        if(written<=0)return false;offset+=(size_t)written;
    }
    return true;
}
static void collect(const std::string &state,const std::string &name,std::vector<Entry> &entries,size_t cap=1048576) {
    int fd=ws_open((state+"/"+name).c_str(),O_RDONLY);if(fd<0)return;
    struct _stat64 info{};
    if(_fstat64(fd,&info) || info.st_size<0){_close(fd);return;}
    uint64_t size=(uint64_t)info.st_size,start=size>cap?size-cap:0;
    if(_lseeki64(fd,(__int64)start,SEEK_SET)<0){_close(fd);return;}
    Entry entry{name,{}};
    if(start)entry.data="[report: earlier content omitted; bounded log tail follows]\n";
    size_t left=(size_t)std::min<uint64_t>(size,cap);char buffer[16384];
    while(left) {
        int count=_read(fd,buffer,(unsigned)std::min(left,sizeof buffer));
        if(count<0 && errno==EINTR)continue;
        if(count<=0)break;entry.data.append(buffer,(size_t)count);left-=(size_t)count;
    }
    _close(fd);
    if(!left && entry.data.find('\0')==std::string::npos)entries.push_back(std::move(entry));
}
static void recent(const std::string &state,const char *folder,const char *prefix,const char *suffix,
                   unsigned limit,std::vector<Entry> &entries) {
    std::string directory=state+"/"+folder;HANDLE guard;
    if(ws_directory(directory.c_str(),false,&guard))return;
    WIN32_FIND_DATAW item{};HANDLE find=FindFirstFileW(monado_launcher_windows::wide(directory+"/*").c_str(),&item);
    std::vector<std::string> names;unsigned scanned=0;
    if(find!=INVALID_HANDLE_VALUE) {
        do {
            if(++scanned>256)break;
            if(item.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))continue;
            std::string name=monado_launcher_windows::utf8(item.cFileName);
            if(name.size()<strlen(prefix)+strlen(suffix) || name.compare(0,strlen(prefix),prefix) ||
               name.compare(name.size()-strlen(suffix),strlen(suffix),suffix))continue;
            if(name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-")!=std::string::npos)continue;
            names.push_back(name);
        } while(FindNextFileW(find,&item));
        FindClose(find);
    }
    std::sort(names.rbegin(),names.rend());if(names.size()>limit)names.resize(limit);
    for(const auto &name:names)collect(state,std::string(folder)+"/"+name,entries,32768);
    CloseHandle(guard);
}
static bool create(const std::string &state,std::string &output,std::string &error) {
    HANDLE root;
    if(ws_directory(state.c_str(),false,&root)){error="Report root: "+std::string(strerror(errno))+" ("+std::to_string(GetLastError())+")";return false;}
    std::vector<Entry> entries;
    collect(state,"xcde.ini",entries,262144);
    for(const char *name:{"build/last_game.log","build/last_precompile.log","build/launcher.log","build/lasrecomp.log","build/lasrecomp.prev.log"})collect(state,name,entries);
    recent(state,"logs/game-runs","run-",".json",16,entries);
    recent(state,"crash","crash_",".txt",2,entries);
    if(entries.empty()){CloseHandle(root);error="No selected local text logs or settings to collect.";return false;}
    std::string manifest="{\"schema\":1,\"format\":\"monado-local-support\",\"uploaded\":false,\"note\":\"Review before sharing. Logs/settings may contain local paths. No saves, game files, environment dump or binary crash data.\",\"files\":[";
    bool first=true;
    for(auto &entry:entries){if(!first)manifest+=",";first=false;manifest+="{\"name\":\""+entry.name+"\",\"bytes\":"+std::to_string(entry.data.size())+"}";}
    manifest+="]}\n";entries.push_back({"report-manifest.json",manifest});
    std::string directory=state+"/reports";HANDLE reports;
    if(ws_directory(directory.c_str(),true,&reports)){CloseHandle(root);error="Report destination: "+std::string(strerror(errno))+" ("+std::to_string(GetLastError())+")";return false;}
    std::string name,temporary;int fd=-1;
    for(unsigned attempt=0;attempt<100;attempt++) {
        unsigned random;if(rand_s(&random))break;
        name="monado-report-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(random)+".zip";
        temporary=directory+"/"+name+".tmp";
        fd=ws_open(temporary.c_str(),O_WRONLY|O_CREAT|O_EXCL,0600);
        if(fd>=0 || errno!=EEXIST)break;
    }
    if(fd<0){CloseHandle(reports);CloseHandle(root);error="Report temporary: "+std::string(strerror(errno))+" ("+std::to_string(GetLastError())+")";return false;}
    std::string zip;
    for(auto &e:entries){e.crc=crc32(e.data);e.offset=uint32_t(zip.size());le(zip,0x04034b50,4);le(zip,20,2);le(zip,0x800,2);le(zip,0,2);le(zip,0,2);le(zip,0x21,2);le(zip,e.crc,4);le(zip,uint32_t(e.data.size()),4);le(zip,uint32_t(e.data.size()),4);le(zip,uint32_t(e.name.size()),2);le(zip,0,2);zip+=e.name;zip+=e.data;}
    uint32_t central=uint32_t(zip.size());
    for(auto &e:entries){le(zip,0x02014b50,4);le(zip,20,2);le(zip,20,2);le(zip,0x800,2);le(zip,0,2);le(zip,0,2);le(zip,0x21,2);le(zip,e.crc,4);le(zip,uint32_t(e.data.size()),4);le(zip,uint32_t(e.data.size()),4);le(zip,uint32_t(e.name.size()),2);le(zip,0,2);le(zip,0,2);le(zip,0,2);le(zip,0,2);le(zip,0,4);le(zip,e.offset,4);zip+=e.name;}
    uint32_t central_size=uint32_t(zip.size())-central;le(zip,0x06054b50,4);le(zip,0,2);le(zip,0,2);le(zip,uint32_t(entries.size()),2);le(zip,uint32_t(entries.size()),2);le(zip,central_size,4);le(zip,central,4);le(zip,0,2);

    bool ok=write_all(fd,zip) && !_commit(fd);
    if(_close(fd))ok=false;
    std::string final=directory+"/"+name;
    if(ok)ok=MoveFileExW(monado_launcher_windows::wide(temporary).c_str(),monado_launcher_windows::wide(final).c_str(),MOVEFILE_WRITE_THROUGH)!=0;
    if(!ok){ws_unlink(temporary.c_str());error="Native report write/flush/activation failed (Windows error "+std::to_string(GetLastError())+").";}
    else output=final;
    CloseHandle(reports);CloseHandle(root);return ok;
}
} // namespace monado_report
