// Authored synthetic payloads/real FS handlers. Never game data or user saves.
#define _GNU_SOURCE
#include <assert.h>
#include <setjmp.h>
#include <stdarg.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
// Declare real libc headers BEFORE redirecting the authored modules' calls.
// Rewriting libc's fortified declarations changes their assembler aliases and
// can silently bypass fault injection in sanitizer builds.
ssize_t __wrap_read(int,void*,size_t);
ssize_t __wrap_write(int,const void*,size_t);
ssize_t __wrap_pwrite(int,const void*,size_t,off_t);
DIR *__wrap_opendir(const char*);
#define read __wrap_read
#define write __wrap_write
#define pwrite __wrap_pwrite
#define opendir __wrap_opendir
#include "../../runtime/hle.h"
// This standalone fixture GCs unrelated services. Keep every real module reset
// registration (runtime links them without section GC), then enumerate them.
#undef RT_ON_RESET
#define RT_ON_RESET(fn) \
    static void fn(void); \
    __attribute__((used,retain,section(RESET_SECTION))) static const ResetFn fixture_reset_##fn=fn; \
    static void fn(void)
#include "../../runtime/services/fs.c"
#define directory quickstep_metadata_directory
#include "../../runtime/hle/quickstep_save.c"
#undef directory
#undef read
#undef write
#undef pwrite
#undef opendir
#include "../../runtime/settings.h"

Settings g_settings;
static QuickstepSnapshot exported, imported;
static bool export_ok=true, fail_clone, fail_meta, fail_partial, fail_restore, fail_clear;
static unsigned cases, ticks, adoptions;
static uint64_t imported_playing;
static const uint64_t raw=HEAP_BASE+0x10000, context=HEAP_BASE+0x350000;
static const uint64_t view=HEAP_BASE+0x340000, playing=HEAP_BASE+0x240000;
static jmp_buf strict_stop;
static bool expect_stop;
extern const ResetFn __start_resets[],__stop_resets[];
static void reset_runtime(void){for(const ResetFn *p=__start_resets;p<__stop_resets;++p)(*p)();}

const char *rando_seed_dir(void){return NULL;}
void rt_fatal_trace(void){abort();}
void diag_unimplemented(const char *kind,const char *format,...){
    (void)kind;(void)format;if(expect_stop)longjmp(strict_stop,1);abort();
}
Service *service_new(const char *name,ServiceHandler handler,void *state){
    Service *s=calloc(1,sizeof *s);assert(s);s->name=name;s->handler=handler;s->state=state;return s;
}
void ipc_return_object(IpcResp *r,Service *s){r->objects[r->nobjects++]=s;}
void save_editor_file_read(const char *p,uint64_t a,int64_t o,int64_t n){(void)p;(void)a;(void)o;(void)n;}
void flags_ext_save_read(const char *p,uint64_t a,int64_t o,int64_t n){(void)p;(void)a;(void)o;(void)n;}
void flags_ext_save_written(const char *p,uint64_t a,int64_t o,int64_t n){(void)p;(void)a;(void)o;(void)n;}
void quickstep_slot_snapshot(QuickstepSnapshot *s){*s=exported;}
bool quickstep_slot_snapshot_for_save(uint64_t p,QuickstepSnapshot *s){assert(p==raw);*s=exported;return export_ok;}
void quickstep_slot_suspend(void){}
void quickstep_slot_adopt(uint64_t p,uint32_t story,const QuickstepSnapshot *s){
    assert(story<2&&s->story==story);imported=*s;imported_playing=p;++adoptions;
}
void quickstep_slot_tick(Cpu *c,uint64_t p){assert(p==context);++ticks;memset(c,0x97,sizeof *c);}
void guest_call(Cpu *c,uint64_t target){
    assert(LD64(c->x[0])==context);uint64_t result;
    if(target==0x7000592a30ull)result=view;
    else {assert(target==0x7000592b80ull);result=playing;}
    memset(c,0xac,sizeof *c);c->x[0]=result;
}
static bool fd_has(int fd,const char *part){
    char name[64],path[4096];snprintf(name,sizeof name,"/proc/self/fd/%d",fd);
    ssize_t n=readlink(name,path,sizeof path-1);if(n<0)return false;path[n]=0;return strstr(path,part)!=NULL;
}
ssize_t __real_read(int fd,void *p,size_t n){return syscall(SYS_read,fd,p,n);}
ssize_t __wrap_read(int fd,void *p,size_t n){
    if((fail_clone&&fd_has(fd,"/save/bfsgame"))||(fail_restore&&fd_has(fd,"/save.journal/bfsgame"))){errno=EIO;return -1;}return __real_read(fd,p,n);
}
ssize_t __real_write(int fd,const void *p,size_t n){return syscall(SYS_write,fd,p,n);}
ssize_t __wrap_write(int fd,const void *p,size_t n){
    if(fail_meta&&fd_has(fd,".monado-quickstep")){errno=ENOSPC;return -1;}return __real_write(fd,p,n);
}
ssize_t __real_pwrite(int fd,const void *p,size_t n,off_t off){return syscall(SYS_pwrite64,fd,p,n,off);}
ssize_t __wrap_pwrite(int fd,const void *p,size_t n,off_t off){
    if(fail_partial){if(off){errno=EIO;return -1;}return __real_pwrite(fd,p,n/2,off);}return __real_pwrite(fd,p,n,off);
}
DIR *__wrap_opendir(const char *path){
    if(fail_clear&&!strcmp(path,"save")){errno=EACCES;return NULL;}return opendir(path);
}
static Cpu cpu(void){Cpu c;memset(&c,0x48,sizeof c);c.sp=HEAP_BASE+0x3f0000;return c;}
static void capture(void){Cpu c=cpu();c.x[19]=raw;Cpu before=c;quickstep_save_serialized(&c);assert(!memcmp(&c,&before,sizeof c));}
static void put(const char *path,const void *p,size_t n){
    int fd=open(path,O_CREAT|O_TRUNC|O_WRONLY,0600);assert(fd>=0);
    assert(__real_write(fd,p,n)==(ssize_t)n);assert(!close(fd));
}
static void original(const char *name,uint8_t value,size_t n){
    char path[128];snprintf(path,sizeof path,"save/%s",name);uint8_t *p=malloc(n);assert(p);memset(p,value,n);put(path,p,n);free(p);
}
static void begin(const char *label){
    quickstep_save_reset();
    memset(g_journals,0,sizeof g_journals);g_njournals=0;
    assert(!mkdir(label,0700));assert(!chdir(label));assert(!mkdir("save",0700));
    assert(!setenv("XCDE_SAVE_BACKUPS","0",1));
    memset((void *)(uintptr_t)raw,0x37,QUICKSTEP_SAVE_BYTES);
    memset(&exported,0,sizeof exported);exported.count=1;exported.entries[0].character=1;
    exported.entries[0].uid=0x30001;memset(exported.entries[0].fingerprint,0x29,32);
    g_settings.quickstep_slot=true;quickstep_save_set_required(true);export_ok=true;
    fail_clone=fail_meta=fail_partial=fail_restore=fail_clear=false;ST8(view+0x150,0);
}
static void finish(void){assert(!chdir(".."));++cases;}
static HostFile opened(const char *name,int journal){
    HostFile f={.save=true,.journal=journal};strcpy(f.root,"save");snprintf(f.path,sizeof f.path,"save/%s",name);
    f.fd=open(f.path,O_RDWR);assert(f.fd>=0);return f;
}
static uint32_t write_payload(HostFile *f){
    int64_t data[3]={0,0,QUICKSTEP_SAVE_BYTES};IpcReq q={.cmd=1,.data=(uint8_t*)data,.data_size=sizeof data,.na=1};
    q.a[0]=(IpcBuf){raw,QUICKSTEP_SAVE_BYTES};IpcResp r={0};Service s={.state=f};file(&s,&q,&r);return r.result;
}
static uint32_t commit_mount(int j){
    HostFs h={.save=true,.journal=j};strcpy(h.root,"save");Service s={.state=&h};IpcReq q={.cmd=10};IpcResp r={0};filesystem(&s,&q,&r);return r.result;
}
static void check_meta(const char *name,uint32_t count){
    char path[128];snprintf(path,sizeof path,"save/.monado-quickstep/%s",name);int fd=open(path,O_RDONLY);assert(fd>=0);
    uint8_t bytes[QUICKSTEP_META_MAX],hash[32];ssize_t n=pread(fd,bytes,sizeof bytes,0);assert(n>0);close(fd);
    quickstep_save_hash((void*)(uintptr_t)raw,QUICKSTEP_SAVE_BYTES,hash);QuickstepSnapshot s;
    assert(!quickstep_save_decode(name,hash,QUICKSTEP_SAVE_BYTES,bytes,(size_t)n,&s)&&s.count==count);
}
static void codec(void){
    static const char *vectors[]={"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"};
    for(unsigned v=0;v<2;++v){uint8_t hash[32];char hex[65];quickstep_save_hash(v?"abc":"",v?3:0,hash);
        for(unsigned i=0;i<32;++i)snprintf(hex+i*2,3,"%02x",hash[i]);assert(!strcmp(hex,vectors[v]));}
    begin("codec");uint8_t hash[32],bytes[QUICKSTEP_META_MAX+16];size_t n;
    quickstep_save_hash((void*)(uintptr_t)raw,QUICKSTEP_SAVE_BYTES,hash);memset(bytes,0xc9,sizeof bytes);
    assert(!quickstep_save_encode("bfsgame98.sav",hash,QUICKSTEP_SAVE_BYTES,&exported,bytes,&n));
    assert(n==144);QuickstepSnapshot s;
    assert(!quickstep_save_decode("bfsgame98.sav",hash,QUICKSTEP_SAVE_BYTES,bytes,n,&s));
    assert(s.count==1&&!memcmp(s.entries,exported.entries,sizeof(QuickstepEntry)));
    for(size_t i=0;i<n;++i){bytes[i]^=1;assert(quickstep_save_decode("bfsgame98.sav",hash,QUICKSTEP_SAVE_BYTES,bytes,n,&s));bytes[i]^=1;}
    for(size_t i=n;i<sizeof bytes;++i)assert(bytes[i]==0xc9);
    assert(quickstep_save_decode("bfsmeria98.sav",hash,QUICKSTEP_SAVE_BYTES,bytes,n,&s));
    assert(quickstep_save_encode("bfsgame99.sav",hash,QUICKSTEP_SAVE_BYTES,&exported,bytes,&n));finish();
}
static void transaction(void){
    begin("transaction");original("bfsgame00.sav",0x19,QUICKSTEP_SAVE_BYTES);put("save/bfssystem.sav","untouched",9);
    int j=journal_mount("save");assert(j>=0);HostFile f=opened("bfsgame00.sav",j);
    // Missing serialization cannot mutate even one byte or dirty the mount.
    assert(write_payload(&f)&&!g_journals[j].dirty);uint8_t first;assert(pread(f.fd,&first,1,0)==1&&first==0x19);
    capture();assert(!write_payload(&f)&&g_journals[j].dirty);assert(access("save/.monado-quickstep",F_OK));
    fail_meta=true;assert(commit_mount(j)&&g_journals[j].dirty);assert(!access("save.journal/bfsgame00.sav",F_OK));
    fail_meta=false;assert(!commit_mount(j));check_meta("bfsgame00.sav",1);
    // Metadata reads observe many candidates but must never adopt any state.
    unsigned before=adoptions;quickstep_save_observe("save",f.path,(void*)(uintptr_t)raw,0,QUICKSTEP_SAVE_BYTES);assert(adoptions==before);
    quickstep_save_intent(0,0,false);quickstep_save_loading();Cpu c=cpu();c.x[20]=playing;c.x[22]=raw;c.x[28]=context;Cpu old=c;
    quickstep_save_adopt(&c);assert(adoptions==before&&!memcmp(&c,&old,sizeof c));
    c=cpu();c.x[19]=context;old=c;quickstep_save_loaded(&c);
    assert(adoptions==before+1&&imported.count==1&&imported_playing==playing&&!memcmp(&c,&old,sizeof c));
    g_settings.quickstep_slot=false;quickstep_save_set_required(false);exported.count=0;capture();
    assert(!write_payload(&f)&&!commit_mount(j));assert(access("save/.monado-quickstep/bfsgame00.sav",F_OK));
    assert(!close(f.fd));finish();
}
static void failures(void){
    begin("snapshot-failure");original("bfsgame00.sav",0x11,QUICKSTEP_SAVE_BYTES);int j=journal_mount("save");HostFile f=opened("bfsgame00.sav",j);capture();
    fail_clone=true;assert(write_payload(&f)&&!g_journals[j].dirty);fail_clone=false;
    uint8_t value;assert(pread(f.fd,&value,1,0)==1&&value==0x11);
    assert(!write_payload(&f)&&!commit_mount(j));check_meta("bfsgame00.sav",1);close(f.fd);finish();
    begin("partial-write");original("bfsgame00.sav",0x11,QUICKSTEP_SAVE_BYTES);j=journal_mount("save");f=opened("bfsgame00.sav",j);capture();
    fail_partial=true;assert(write_payload(&f));fail_partial=false;
    assert(commit_mount(j)&&g_journals[j].dirty);assert(!write_payload(&f)&&!commit_mount(j));close(f.fd);finish();
    begin("no-journal");original("bfsgame00.sav",0x11,QUICKSTEP_SAVE_BYTES);f=opened("bfsgame00.sav",-1);capture();
    assert(write_payload(&f));close(f.fd);finish();
}
static void legacy_and_lifecycle(void){
    begin("legacy");g_settings.quickstep_slot=false;quickstep_save_set_required(false);
    original("bfsgame00.sav",0x42,1390688);unsigned before=adoptions;
    quickstep_save_observe("save","save/bfsgame00.sav",(void*)(uintptr_t)raw,0,1390688);quickstep_save_intent(0,0,false);quickstep_save_loading();
    Cpu c=cpu();c.x[20]=playing;c.x[22]=raw;c.x[28]=context;quickstep_save_adopt(&c);
    c=cpu();c.x[19]=context;quickstep_save_loaded(&c);assert(adoptions==before+1&&!imported.count);
    c=cpu();Cpu old=c;quickstep_save_newgame_begin(&c);assert(!memcmp(&c,&old,sizeof c));
    c.x[19]=context;old=c;quickstep_save_newgame_end(&c);assert(!memcmp(&c,&old,sizeof c)&&!imported.count);
    // A strict-stop on an unknown file command must release the save mutex.
    HostFile f={.save=true,.journal=-1};Service s={.state=&f};IpcReq q={.cmd=999};IpcResp r={0};expect_stop=true;
    if(!setjmp(strict_stop)){file(&s,&q,&r);assert(!"strict stop returned");}
    expect_stop=false;assert(!pthread_mutex_trylock(&g_save_transaction_lock));pthread_mutex_unlock(&g_save_transaction_lock);
    finish();
}
static void namespaces(void){
    begin("namespaces");
    const char *names[]={"bfsgame98.sav","bfsgame00a.sav","bfsmeria98.sav","bfsmeria00a.sav"};
    for(unsigned i=0;i<4;++i)original(names[i],0x12,QUICKSTEP_SAVE_BYTES);
    original("bfsgame00.sav",0x18,QUICKSTEP_SAVE_BYTES);
    int j=journal_mount("save");assert(j>=0);
    // Identical native payloads with DIFFERENT optional assignments must stay
    // scoped to the exact main/FC/manual/autosave filename, never just its hash.
    for(unsigned i=0;i<4;++i){
        exported.story=i/2;exported.entries[0].character=i<2?1:14;
        exported.entries[0].uid=0x30001+i;capture();HostFile f=opened(names[i],j);
        assert(!write_payload(&f));close(f.fd);
    }
    assert(!commit_mount(j));
    for(unsigned i=0;i<4;++i){
        char path[128];snprintf(path,sizeof path,"save/%s",names[i]);
        quickstep_save_observe("save",path,(void*)(uintptr_t)raw,0,QUICKSTEP_SAVE_BYTES);
    }
    for(unsigned i=0;i<4;++i){
        quickstep_save_intent(i/2,i%2?0:98,i%2!=0);quickstep_save_loading();
        quickstep_save_intent(1-i/2,0,true); // Cannot retarget an already-started load.
        Cpu c=cpu();c.x[20]=playing;c.x[22]=raw;c.x[28]=context;quickstep_save_adopt(&c);
        ST8(view+0x150,i<2?0:8);c=cpu();c.x[19]=context;quickstep_save_loaded(&c);
        assert(imported.story==i/2&&imported.count==1&&imported.entries[0].uid==0x30001+i);
    }
    // OFF does not consume/remove/refund the owned gem or erase a valid inactive
    // reference. A normal subsequent save rebinds it to the new payload digest.
    g_settings.quickstep_slot=false;quickstep_save_set_required(true);exported.story=0;
    exported.entries[0].character=1;exported.entries[0].uid=0x30001;
    memset((void*)(uintptr_t)raw,0x65,QUICKSTEP_SAVE_BYTES);capture();HostFile f=opened(names[0],j);
    assert(!write_payload(&f)&&!commit_mount(j));check_meta(names[0],1);close(f.fd);
    assert(access("save/.monado-quickstep/bfsgame97.sav",F_OK));
    int fd=open("save/bfsgame00.sav",O_RDONLY);uint8_t byte;assert(fd>=0&&pread(fd,&byte,1,0)==1&&byte==0x18);close(fd);
    // A different payload cannot inherit these references merely by slot name.
    ((uint8_t*)(uintptr_t)raw)[123]^=1;quickstep_save_intent(0,98,false);quickstep_save_loading();
    Cpu c=cpu();c.x[20]=playing;c.x[22]=raw;c.x[28]=context;quickstep_save_adopt(&c);
    ST8(view+0x150,0);c=cpu();c.x[19]=context;quickstep_save_loaded(&c);assert(!imported.count);
    finish();
}
static void recovery(void){
    begin("paired-recovery");original("bfsgame00.sav",0x11,QUICKSTEP_SAVE_BYTES);
    int j=journal_mount("save");HostFile f=opened("bfsgame00.sav",j);capture();
    assert(!write_payload(&f)&&!commit_mount(j));check_meta("bfsgame00.sav",1);
    exported.entries[0].uid=0x30009;memset((void*)(uintptr_t)raw,0x77,QUICKSTEP_SAVE_BYTES);capture();
    assert(!write_payload(&f));fail_meta=true;assert(commit_mount(j));fail_meta=false;close(f.fd);
    // A fresh runtime/mount recovers BOTH the old native file and its references.
    reset_runtime();
    assert(journal_mount("save")>=0);memset((void*)(uintptr_t)raw,0x37,QUICKSTEP_SAVE_BYTES);check_meta("bfsgame00.sav",1);
    int fd=open("save/bfsgame00.sav",O_RDONLY);uint8_t byte;assert(fd>=0&&pread(fd,&byte,1,0)==1&&byte==0x37);close(fd);
    assert(access("save.journal",F_OK));finish();
}
static uint32_t change_file(int j,unsigned cmd,const char *first,const char *second){
    HostFs h={.save=true,.journal=j};strcpy(h.root,"save");Service service={.state=&h};
    IpcReq q={.cmd=cmd,.nx=second?2:1};q.x[0]=(IpcBuf){(uint64_t)(uintptr_t)first,strlen(first)+1};
    if(second)q.x[1]=(IpcBuf){(uint64_t)(uintptr_t)second,strlen(second)+1};
    IpcResp r={0};filesystem(&service,&q,&r);return r.result;
}
static void rename_delete(void){
    begin("rename-delete");original("bfsgame98.sav",0x11,QUICKSTEP_SAVE_BYTES);int j=journal_mount("save");
    capture();HostFile f=opened("bfsgame98.sav",j);assert(!write_payload(&f)&&!commit_mount(j));close(f.fd);
    assert(!change_file(j,5,"bfsgame98.sav","bfsgame97.sav")&&!commit_mount(j));
    check_meta("bfsgame97.sav",1);assert(access("save/.monado-quickstep/bfsgame98.sav",F_OK));
    assert(!change_file(j,1,"bfsgame97.sav",NULL)&&!commit_mount(j));
    assert(access("save/.monado-quickstep/bfsgame97.sav",F_OK));finish();
}
static void failed_recovery(void){
    begin("failed-recovery");original("bfsgame00.sav",0x11,QUICKSTEP_SAVE_BYTES);
    int j=journal_mount("save");HostFile f=opened("bfsgame00.sav",j);capture();
    assert(!write_payload(&f)&&!commit_mount(j));
    memset((void*)(uintptr_t)raw,0x77,QUICKSTEP_SAVE_BYTES);capture();assert(!write_payload(&f));
    fail_meta=true;assert(commit_mount(j));fail_meta=false;close(f.fd);
    reset_runtime();assert(g_njournals==0);fail_clear=true;j=journal_mount("save");
    assert(j>=0&&g_journals[j].poisoned&&!access("save.journal/bfsgame00.sav",F_OK));
    fail_clear=false;fail_restore=true;j=journal_mount("save");
    assert(j>=0&&g_journals[j].poisoned&&!g_journals[j].dirty);
    assert(!access("save.journal/bfsgame00.sav",F_OK));
    f=opened("bfsgame00.sav",j);uint8_t *output=(uint8_t*)(uintptr_t)(raw+1024);memset(output,0xb3,16);
    int64_t data[3]={0,0,16};IpcReq q={.cmd=0,.data=(uint8_t*)data,.data_size=sizeof data,.nb=1};
    q.b[0]=(IpcBuf){(uint64_t)(uintptr_t)output,16};IpcResp r={0};Service file_service={.state=&f};
    file(&file_service,&q,&r);assert(r.result);for(unsigned i=0;i<16;++i)assert(output[i]==0xb3);
    assert(write_payload(&f)&&journal_touch(j)&&commit_mount(j));
    assert(!access("save.journal/bfsgame00.sav",F_OK));close(f.fd);
    HostFs mount={.save=true,.journal=j};strcpy(mount.root,"save");Service fs_service={.state=&mount};
    q=(IpcReq){.cmd=7,.nx=1};q.x[0]=(IpcBuf){(uint64_t)(uintptr_t)"bfsgame00.sav",14};r=(IpcResp){0};
    filesystem(&fs_service,&q,&r);assert(r.result);
    // Failure is recoverable on the next explicit mount, after the transient
    // I/O error ends. The original rollback copy was never replaced or removed.
    fail_restore=false;assert(journal_mount("save")==j&&!g_journals[j].poisoned);
    memset((void*)(uintptr_t)raw,0x37,QUICKSTEP_SAVE_BYTES);check_meta("bfsgame00.sav",1);
    int fd=open("save/bfsgame00.sav",O_RDONLY);uint8_t byte;assert(fd>=0&&pread(fd,&byte,1,0)==1&&byte==0x37);close(fd);
    assert(access("save.journal",F_OK));finish();
}
int main(void){
    const struct rlimit no_core={0,0};assert(!setrlimit(RLIMIT_CORE,&no_core));
    assert(mmap((void*)(uintptr_t)HEAP_BASE,0x400000,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0)==(void*)(uintptr_t)HEAP_BASE);
    codec();transaction();failures();legacy_and_lifecycle();namespaces();recovery();rename_delete();failed_recovery();
    assert(munmap((void*)(uintptr_t)HEAP_BASE,0x400000)==0);
    printf("PASS: Quickstep metadata codec, native serialization/adoption boundaries and paired FS transactions (%u cases)\n",cases);return 0;
}
