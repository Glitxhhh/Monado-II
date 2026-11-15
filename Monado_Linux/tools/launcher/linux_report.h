// [XCDE] LINUX-103: local, bounded support ZIP; no shell, extra packages or uploads.
#pragma once
#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <vector>
namespace monado_report {
struct Entry { std::string name, data; uint32_t crc=0, offset=0; };
static uint32_t crc32(const std::string &s) {
    uint32_t c=~0u;for(unsigned char b:s){c^=b;for(int i=0;i<8;i++)c=(c>>1)^(0xedb88320u&uint32_t(-int(c&1)));}return ~c;
}
static void le(std::string &s,uint32_t n,int bytes) {for(int i=0;i<bytes;i++)s.push_back(char(n>>(8*i)));}
static bool write_all(int fd,const std::string &s) {
    size_t pos=0;while(pos<s.size()){ssize_t n=write(fd,s.data()+pos,s.size()-pos);if(n<0&&errno==EINTR)continue;if(n<=0)return false;pos+=size_t(n);}return true;
}
// Each path component is opened relative to an owned directory, without symlinks.
static int directory(int base,const char *name) {return openat(base,name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);}
static void collect(int base,const char *file,const std::string &name,std::vector<Entry>&entries,size_t cap=1048576) {
    int fd=openat(base,file,O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);if(fd<0)return;
    struct stat st{};
    if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_uid!=geteuid()||st.st_size<0){close(fd);return;}
    uint64_t size=uint64_t(st.st_size);off_t start=size>cap?off_t(size-cap):0;
    if(lseek(fd,start,SEEK_SET)<0){close(fd);return;}
    Entry e{name,{}};if(start)e.data="[report: earlier content omitted; bounded log tail follows]\n";
    size_t left=size_t(std::min<uint64_t>(size,cap));char buffer[16384];
    while(left){ssize_t n=read(fd,buffer,std::min(left,sizeof buffer));if(n<0&&errno==EINTR)continue;if(n<=0)break;e.data.append(buffer,size_t(n));left-=size_t(n);}
    close(fd);if(e.data.find('\0')==std::string::npos)entries.push_back(std::move(e)); // text only
}
static void recent(int base,const char *prefix,const char *suffix,const std::string &folder,
                   unsigned limit,std::vector<Entry>&entries) {
    int copy=dup(base);if(copy<0)return;DIR *dir=fdopendir(copy);if(!dir){close(copy);return;}
    std::vector<std::string> names;unsigned scanned=0;
    while(auto *e=readdir(dir)){
        if(++scanned>256)break;std::string n=e->d_name;
        if(n.size()<strlen(prefix)+strlen(suffix)||n.compare(0,strlen(prefix),prefix)||
           n.compare(n.size()-strlen(suffix),strlen(suffix),suffix))continue;
        if(n.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-")!=std::string::npos)continue;
        names.push_back(n);
    }
    closedir(dir);std::sort(names.rbegin(),names.rend());
    if(names.size()>limit)names.resize(limit);
    for(auto &n:names)collect(base,n.c_str(),folder+n,entries,32768);
}
static bool create(const std::string &state,std::string &output,std::string &error) {
    int root=open(state.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(root<0){error=strerror(errno);return false;}
    std::vector<Entry> entries;
    collect(root,"xcde.ini","xcde.ini",entries,262144);
    int logs=directory(root,"logs");
    if(logs>=0){
        for(auto n:{"monado.log","monado.prev.log","launcher-game.log","launcher.log","launcher-precompile.log","precompile-worker.log","launcher-console.log"})
            collect(logs,n,std::string("logs/")+n,entries);
        int runs=directory(logs,"game-runs");
        if(runs>=0){recent(runs,"run-",".json","logs/game-runs/",16,entries);close(runs);}
        close(logs);
    }
    int crash=directory(root,"crash");if(crash>=0){recent(crash,"crash_",".txt","crash/",2,entries);close(crash);}
    if(entries.empty()){close(root);error="No local text logs or settings to collect.";return false;}
    std::string manifest="{\"schema\":1,\"format\":\"monado-local-support\",\"uploaded\":false,\"note\":\"Review before sharing. Logs/settings may contain local paths. No saves, game files, environment dump or binary crash data.\",\"files\":[";
    bool first=true;for(auto &e:entries){if(!first)manifest+=",";first=false;manifest+="{\"name\":\""+e.name+"\",\"bytes\":"+std::to_string(e.data.size())+"}";}manifest+="]}\n";
    entries.push_back({"report-manifest.json",manifest});
    if(mkdirat(root,"reports",0700)&&errno!=EEXIST){error=strerror(errno);close(root);return false;}
    int reports=directory(root,"reports");close(root);if(reports<0){error=strerror(errno);return false;}
    struct stat st{};if(fstat(reports,&st)||st.st_uid!=geteuid()){error="Report directory is not owned by this user.";close(reports);return false;}
    char stamp[40];time_t now=time(nullptr);struct tm t{};gmtime_r(&now,&t);strftime(stamp,sizeof stamp,"%Y%m%dT%H%M%SZ",&t);
    std::string name;int fd=-1;
    for(unsigned i=0;i<100&&fd<0;i++){name="monado-report-"+std::string(stamp)+"-"+std::to_string(getpid())+"-"+std::to_string(i)+".zip";
        fd=openat(reports,(name+".tmp").c_str(),O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
        if(fd<0&&errno!=EEXIST)break;
        struct stat exists{};if(fd>=0&&!fstatat(reports,name.c_str(),&exists,AT_SYMLINK_NOFOLLOW)){close(fd);fd=-1;unlinkat(reports,(name+".tmp").c_str(),0);}
    }
    if(fd<0){error=strerror(errno);close(reports);return false;}
    std::string zip;
    for(auto &e:entries){e.crc=crc32(e.data);e.offset=uint32_t(zip.size());le(zip,0x04034b50,4);le(zip,20,2);le(zip,0x800,2);le(zip,0,2);le(zip,0,2);le(zip,0x21,2);le(zip,e.crc,4);le(zip,uint32_t(e.data.size()),4);le(zip,uint32_t(e.data.size()),4);le(zip,uint32_t(e.name.size()),2);le(zip,0,2);zip+=e.name;zip+=e.data;}
    uint32_t central=uint32_t(zip.size());
    for(auto &e:entries){le(zip,0x02014b50,4);le(zip,20,2);le(zip,20,2);le(zip,0x800,2);le(zip,0,2);le(zip,0,2);le(zip,0x21,2);le(zip,e.crc,4);le(zip,uint32_t(e.data.size()),4);le(zip,uint32_t(e.data.size()),4);le(zip,uint32_t(e.name.size()),2);le(zip,0,2);le(zip,0,2);le(zip,0,2);le(zip,0,2);le(zip,0,4);le(zip,e.offset,4);zip+=e.name;}
    uint32_t central_size=uint32_t(zip.size())-central;le(zip,0x06054b50,4);le(zip,0,2);le(zip,0,2);le(zip,uint32_t(entries.size()),2);le(zip,uint32_t(entries.size()),2);le(zip,central_size,4);le(zip,central,4);le(zip,0,2);
    bool ok=write_all(fd,zip)&&!fsync(fd);int saved=errno;if(close(fd))ok=false;
    if(ok)ok=!syscall(SYS_renameat2,reports,(name+".tmp").c_str(),reports,name.c_str(),1u); // RENAME_NOREPLACE
    if(!ok){if(!saved)saved=errno;unlinkat(reports,(name+".tmp").c_str(),0);error=strerror(saved);}
    else {fsync(reports);output=state+"/reports/"+name;}
    close(reports);return ok;
}
} // namespace monado_report
