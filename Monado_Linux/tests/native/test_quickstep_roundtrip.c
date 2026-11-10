// [XCDE] LINUX-044: real slot + save + FS modules, authored synthetic inventory.
// No game code, game files, user saves or metadata from gameplay.
#define _GNU_SOURCE
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../../runtime/hle.h"
#include "../../runtime/settings.h"
#include "../../runtime/services/fs.c"
void fixture_slot_adopt(uint64_t,unsigned,const QuickstepSnapshot *);
#define quickstep_slot_adopt fixture_slot_adopt
#define directory quickstep_metadata_directory
#include "../../runtime/hle/quickstep_save.c"
#undef directory
#undef quickstep_slot_adopt

#define PLAY (HEAP_BASE+0x100000)
#define CONTEXT (HEAP_BASE+0x1000)
#define VIEW (HEAP_BASE+0x2000)
#define RAW (HEAP_BASE+0x500000)
#define OTHER (HEAP_BASE+0x700000)
#define STACK (HEAP_BASE+0x1000000)
#define HEADER (HEAP_BASE+0x900000)
#define CACHE (HEAP_BASE+0xa00000)
#define ITEM(n) (PLAY+0x41c0+0x28870+(n)*0x2c)
#define UID(n) (0x30000u|(n))
Settings g_settings;
int g_rt_unwind;
static unsigned cases,refreshes;
static unsigned interrupt_story;
#ifndef QUICKSTEP_SPLIT_READ_TEST
// Baseline replay uses the old module, which has no native copy hooks.
void quickstep_save_header_copy(Cpu *c){(void)c;}
void quickstep_save_cache_copy(Cpu *c){(void)c;}
void quickstep_save_selected_copy(Cpu *c){(void)c;}
#endif
#ifdef QUICKSTEP_TRANSITION_TEST
static pthread_mutex_t adoption_test_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t adoption_test_ready=PTHREAD_COND_INITIALIZER;
static bool interrupt_adoption,adoption_worker_ready;
static pthread_t adoption_worker;
static void *concurrent_newgame(void *unused){
    (void)unused;
    // This boundary is immediately before the REAL slot adoption. The save
    // lifecycle gate must still be held, including across its required callback.
    assert(pthread_mutex_trylock(&transition)==EBUSY);
    pthread_mutex_lock(&adoption_test_lock);adoption_worker_ready=true;
    pthread_cond_signal(&adoption_test_ready);pthread_mutex_unlock(&adoption_test_lock);
    Cpu c={.sp=STACK};quickstep_save_newgame_begin(&c);quickstep_save_new_game(PLAY,0);return NULL;
}
#endif
void fixture_slot_adopt(uint64_t data,unsigned story,const QuickstepSnapshot *s){
#ifdef QUICKSTEP_TRANSITION_TEST
    if(interrupt_adoption){
        interrupt_adoption=false;adoption_worker_ready=false;
        assert(!pthread_create(&adoption_worker,NULL,concurrent_newgame,NULL));
        pthread_mutex_lock(&adoption_test_lock);
        while(!adoption_worker_ready)pthread_cond_wait(&adoption_test_ready,&adoption_test_lock);
        pthread_mutex_unlock(&adoption_test_lock);
    }
#endif
    quickstep_slot_adopt(data,story,s);
}
const char *rando_seed_dir(void){return NULL;}
void rt_fatal_trace(void){abort();}
void rt_unwind_point(void){}
void diag_unimplemented(const char *kind,const char *format,...){(void)kind;(void)format;abort();}
Service *service_new(const char *name,ServiceHandler handler,void *state){
    Service *s=calloc(1,sizeof *s);assert(s);s->name=name;s->handler=handler;s->state=state;return s;
}
void ipc_return_object(IpcResp *r,Service *s){r->objects[r->nobjects++]=s;}
void save_editor_file_read(const char *p,uint64_t a,int64_t o,int64_t n){(void)p;(void)a;(void)o;(void)n;}
void flags_ext_save_read(const char *p,uint64_t a,int64_t o,int64_t n){(void)p;(void)a;(void)o;(void)n;}
void flags_ext_save_written(const char *p,uint64_t a,int64_t o,int64_t n){(void)p;(void)a;(void)o;(void)n;}
void *hle_alloc(uint64_t n){(void)n;abort();}
void guest_call(Cpu *c,uint64_t target){
    uint64_t a=c->x[0],b=c->x[1],d=c->x[2],result=0;
    switch(target){
    case 0x7000592a30ull:
        assert(LD64(a)==CONTEXT);result=VIEW;
        if(interrupt_story){unsigned action=interrupt_story;interrupt_story=0;
            if(action==1)quickstep_save_loading();else quickstep_save_newgame_begin(c);}
        break;
    case 0x7000592b80ull:assert(LD64(a)==CONTEXT);result=PLAY;break;
    case 0x70000afab0ull:assert(a==CONTEXT&&b>0&&b<16);result=PLAY+0x154630+(b-1)*0x3480;break;
    case 0x700008b860ull:assert(a==PLAY+0x154634+(d-1)*0x3480&&b==CONTEXT);++refreshes;break;
    default:fprintf(stderr,"unexpected native dependency %llx\n",(unsigned long long)target);abort();
    }
    memset(c,0xa8,sizeof *c);c->x[0]=result;
}
static Cpu state(void){Cpu c;memset(&c,0x56,sizeof c);c.sp=STACK;return c;}
static void owned(unsigned n,unsigned serial){
    ST32(ITEM(n),UID(n));ST16(ITEM(n)+4,4580);ST16(ITEM(n)+6,3);
    ST32(ITEM(n)+0xc,serial);ST8(ITEM(n)+0x10,1);ST8(ITEM(n)+0x16,5);
    ST8(ITEM(n)+0x1a,1);ST16(ITEM(n)+0x1c,77);ST8(ITEM(n)+0x1e,20);
}
static QuickstepSnapshot snapshot(void){QuickstepSnapshot s;quickstep_slot_snapshot(&s);return s;}
static void assign(unsigned character,unsigned n){
    Cpu c=state();QuickstepGem list[4];size_t count=quickstep_slot_list(&c,CONTEXT,character,list,4);
    QuickstepGem *found=NULL;for(size_t i=0;i<count;++i)if(list[i].uid==UID(n))found=&list[i];
    assert(found&&quickstep_slot_set_checked(&c,CONTEXT,character,found->uid,found->fingerprint));
}
static void begin(const char *name,unsigned value){
    quickstep_save_reset();memset(g_journals,0,sizeof g_journals);g_njournals=0;
    memset((void*)PLAY,0,0x200000);memset((void*)RAW,0,QUICKSTEP_SAVE_BYTES);memset((void*)OTHER,0,QUICKSTEP_SAVE_BYTES);
    assert(!mkdir(name,0700)&&!chdir(name)&&!mkdir("save",0700));
    assert(!setenv("XCDE_SAVE_BACKUPS","0",1));g_settings.quickstep_slot=true;
    ST8(VIEW+0x150,value<<3);ST32(PLAY+0x6b0+0x5ec,value<<22);owned(1,101);owned(2,102);
    quickstep_slot_adopt(PLAY,value,NULL);Cpu c=state();quickstep_slot_tick(&c,CONTEXT);
}
static void finish(void){assert(!chdir(".."));++cases;}
static HostFile open_file(const char *root,const char *name,bool create){
    HostFile f={.save=true,.journal=journal_mount(root)};assert(f.journal>=0);
    snprintf(f.root,sizeof f.root,"%s",root);snprintf(f.path,sizeof f.path,"%s/%s",root,name);
    f.fd=open(f.path,O_RDWR|(create?O_CREAT:0),0600);assert(f.fd>=0);return f;
}
static void write_save(const char *root,const char *name){
    Cpu c=state(),before;c.x[1]=CONTEXT;before=c;quickstep_save_serializing(&c);assert(!memcmp(&c,&before,sizeof c));
    // Selected native73C30 copies this exact prefix, confirmed separately from
    // the synthetic fixture. Real slot snapshot filtering runs on the copy.
    memcpy((void*)RAW,(void*)(PLAY+0x6b0),QUICKSTEP_SAVE_BYTES);
    c=state();c.x[19]=RAW;before=c;quickstep_save_serialized(&c);assert(!memcmp(&c,&before,sizeof c));
    HostFile f=open_file(root,name,true);int64_t data[3]={0,0,QUICKSTEP_SAVE_BYTES};
    IpcReq q={.cmd=1,.data=(uint8_t*)data,.data_size=sizeof data,.na=1};q.a[0]=(IpcBuf){RAW,QUICKSTEP_SAVE_BYTES};
    IpcResp r={0};Service s={.state=&f};file(&s,&q,&r);assert(!r.result&&!journal_commit(f.journal));assert(!close(f.fd));
}
static void restart(void){
    quickstep_slot_adopt(PLAY,(LD8(VIEW+0x150)>>3)&1,NULL);quickstep_slot_suspend();quickstep_save_reset();
    memset(g_journals,0,sizeof g_journals);g_njournals=0;
}
static void read_piece(const char *root,const char *name,uint64_t dest,size_t bytes,int64_t offset,bool intent){
    if(intent){
        uint32_t story,file,automatic;assert(identity(name,&story,&file,&automatic));
        ST64(dest-0x150+8,CONTEXT);Cpu c=state(),before;c.x[0]=dest-0x150;c.x[1]=file;c.x[2]=automatic;
        before=c;quickstep_save_read_intent(&c);assert(!memcmp(&c,&before,sizeof c));
    }
    HostFile f=open_file(root,name,false);int64_t data[3]={0,offset,(int64_t)bytes};
    IpcReq q={.cmd=0,.data=(uint8_t*)data,.data_size=sizeof data,.nb=1};q.b[0]=(IpcBuf){dest,bytes};
    IpcResp r={0};Service s={.state=&f};file(&s,&q,&r);assert(!r.result&&r.data_size==8&&!close(f.fd));
    int64_t transferred;memcpy(&transferred,r.data,sizeof transferred);assert(transferred==(int64_t)bytes);
}
static void read_save(const char *root,const char *name,uint64_t dest,size_t bytes,bool intent){read_piece(root,name,dest,bytes,0,intent);}
static void adopt(uint64_t raw){
    quickstep_save_loading();
    memcpy((void*)(PLAY+0x6b0),(void*)raw,QUICKSTEP_SAVE_BYTES);
    Cpu c=state(),before;c.x[20]=PLAY;c.x[22]=raw;c.x[28]=CONTEXT;before=c;
    quickstep_save_adopt(&c);assert(!memcmp(&c,&before,sizeof c));
    c=state();c.x[19]=CONTEXT;before=c;quickstep_save_loaded(&c);assert(!memcmp(&c,&before,sizeof c));
}
static void expect(unsigned character,unsigned n){
    QuickstepSnapshot s=snapshot();assert(s.count==1&&s.entries[0].character==character&&s.entries[0].uid==UID(n));
}
static void roundtrips(void){
    const char *names[]={"bfsgame00.sav","bfsgame98.sav","bfsgame00a.sav","bfsmeria98.sav","bfsmeria00a.sav"};
    for(unsigned i=0;i<5;++i){char label[32];snprintf(label,sizeof label,"roundtrip-%u",i);begin(label,i>=3);
        unsigned character=i>=3?14:1;assign(character,1);write_save("save",names[i]);restart();
        read_save("save",names[i],RAW,QUICKSTEP_SAVE_BYTES,true);assert(!snapshot().count);adopt(RAW);expect(character,1);
        // OFF retains the inactive association across a fresh committed save.
        g_settings.quickstep_slot=false;write_save("save",names[i]);restart();read_save("save",names[i],RAW,QUICKSTEP_SAVE_BYTES,true);
        adopt(RAW);expect(character,1);g_settings.quickstep_slot=true;Cpu c=state();quickstep_slot_tick(&c,CONTEXT);expect(character,1);finish();
    }
}
static void interleaved(bool another_name){
    begin(another_name?"interleaved-native-reads":"same-name-distinct-roots",0);assign(1,1);write_save("save","bfsgame00.sav");
    assert(!mkdir("other",0700));assert(quickstep_slot_set_checked(&(Cpu){.sp=STACK},CONTEXT,1,0,NULL));assign(2,2);
    write_save("other","bfsgame00.sav");write_save("other","bfsgame98.sav");restart();
    read_save("save","bfsgame00.sav",RAW,QUICKSTEP_SAVE_BYTES,true);
    read_save("other","bfsgame00.sav",OTHER,QUICKSTEP_SAVE_BYTES,true);
    if(another_name)read_save("other","bfsgame98.sav",OTHER,QUICKSTEP_SAVE_BYTES,true);
    // A speculative complete read and its intent cannot retarget RAW's actual
    // decode, nor select another mount's same-name, identical-payload metadata.
    adopt(RAW);expect(1,1);finish();
}
static void shortened_observation(void){
    begin("short-summary-after-full",0);assign(1,1);write_save("save","bfsgame00.sav");restart();
    read_save("save","bfsgame00.sav",RAW,QUICKSTEP_SAVE_BYTES,true);
    read_save("save","bfsgame00.sav",RAW,1024*1024,false);adopt(RAW);expect(1,1);finish();
}
static void rejection(void){
    const char *labels[]={"missing-metadata","corrupt-metadata","wrong-payload","wrong-context","reused-instance","ordinary-transfer"};
    for(unsigned i=0;i<6;++i){begin(labels[i],0);assign(1,1);write_save("save","bfsgame00.sav");restart();
        const char *meta="save/.monado-quickstep/bfsgame00.sav";
        if(i==0)assert(!unlink(meta));
        if(i==1){int fd=open(meta,O_WRONLY);char x='!';assert(fd>=0&&pwrite(fd,&x,1,0)==1&&!close(fd));}
        read_save("save","bfsgame00.sav",RAW,QUICKSTEP_SAVE_BYTES,true);
        if(i==2)ST8(RAW+100,LD8(RAW+100)^1);
        quickstep_save_loading();memcpy((void*)(PLAY+0x6b0),(void*)RAW,QUICKSTEP_SAVE_BYTES);
        Cpu c=state();c.x[20]=PLAY;c.x[22]=RAW;c.x[28]=CONTEXT;quickstep_save_adopt(&c);
        if(i==4)ST32(ITEM(1)+0xc,999);
        if(i==5){uint64_t boots=PLAY+0x41c0+0x1d4c0;ST16(boots+6,8);ST8(boots+0x10,1);ST8(boots+0x15,1);ST32(boots+0x18,UID(1));}
        c=state();c.x[19]=CONTEXT+(i==3?8:0);quickstep_save_loaded(&c);assert(!snapshot().count);finish();
    }
}
static void lifecycle_yields(bool read_phase){
    begin(read_phase?"read-intent-yields-to-newgame":"finalization-yields-to-load",0);
    assign(1,1);write_save("save","bfsgame00.sav");restart();
    if(read_phase)interrupt_story=2;
    read_save("save","bfsgame00.sav",RAW,QUICKSTEP_SAVE_BYTES,true);
    quickstep_save_loading();memcpy((void*)(PLAY+0x6b0),(void*)RAW,QUICKSTEP_SAVE_BYTES);
    Cpu c=state();c.x[20]=PLAY;c.x[22]=RAW;c.x[28]=CONTEXT;quickstep_save_adopt(&c);
    if(!read_phase)interrupt_story=1;
    c=state();c.x[19]=CONTEXT;quickstep_save_loaded(&c);assert(!snapshot().count);finish();
}
static void continue_yield(void){
    begin("continue-yields-to-newgame",0);assign(1,1);write_save("save","bfsgame00.sav");restart();
    uint64_t owner=HEAP_BASE+0x3000,holder=HEAP_BASE+0x4000,selection=HEAP_BASE+0x8000;
    ST64(owner+0x10,holder);ST64(holder+0x28,selection);ST32(selection+0x18de0,1);ST64(owner+0xa8,CONTEXT);
    Cpu c=state();c.x[19]=owner;interrupt_story=2;quickstep_save_continue_intent(&c);
    read_save("save","bfsgame00.sav",RAW,QUICKSTEP_SAVE_BYTES,false);adopt(RAW);assert(!snapshot().count);finish();
}
static void split_cached_read(unsigned story,unsigned copies,bool poison,bool automatic,bool stale,bool intent){
    char label[80];snprintf(label,sizeof label,"actual-split-story%u-copies%u-poison%u-auto%u-stale%u-intent%u",story,copies,poison,automatic,stale,intent);
    begin(label,story);unsigned character=story?14:1;assign(character,1);assign(story?15:2,2);
    const char *name=automatic?(story?"bfsmeria00a.sav":"bfsgame00a.sav"):(story?"bfsmeria98.sav":"bfsgame00.sav");write_save("save",name);restart();
    // Actual R1 log:48 bytes to a separate stack header, then offset48 and
    // size0x153f00 into the raw buffer+48. Selected5C4130 proves this copy.
    if(intent){
        ST64(RAW-0x150+8,CONTEXT);Cpu request=state();request.x[0]=RAW-0x150;
        request.x[1]=automatic?0:story?98:0;request.x[2]=automatic;quickstep_save_read_intent(&request);
    }
    read_piece("save",name,HEADER,48,0,false);memcpy((void*)RAW,(void*)HEADER,48);
    Cpu c=state(),before;c.sp=HEADER-0x70;c.x[8]=RAW;before=c;
    quickstep_save_header_copy(&c);assert(!memcmp(&c,&before,sizeof c));
    read_piece("save",name,RAW+48,QUICKSTEP_SAVE_BYTES-48,48,false);
    uint64_t selected=RAW;
    if(copies){
        memcpy((void*)CACHE,(void*)RAW,QUICKSTEP_SAVE_BYTES);
        // Both selected native typed copies leave this four-byte padding gap.
        // Wire metadata stays bound to the original file, not copied padding.
        ST32(CACHE+0x4693c,0xa1b2c3d4);c=state();c.x[19]=CACHE;c.x[20]=RAW;before=c;
        quickstep_save_cache_copy(&c);assert(!memcmp(&c,&before,sizeof c));selected=CACHE;
    }
    if(copies>1){
        memcpy((void*)OTHER,(void*)CACHE,QUICKSTEP_SAVE_BYTES);ST32(OTHER+0x4693c,0xfaceabba);
        ST64(HEAP_BASE+0x4000,CACHE);c=state();c.x[21]=OTHER;c.x[19]=HEAP_BASE+0x4000;before=c;
        quickstep_save_selected_copy(&c);assert(!memcmp(&c,&before,sizeof c));selected=OTHER;
    }
    if(poison)ST32(selected+0x2c380+0x2c+0xc,999);
    if(stale){
        quickstep_save_intent(story,97,false);ST64(selected-0x150+8,CONTEXT);
        c=state();c.x[0]=selected-0x150;c.x[1]=97;c.x[2]=0;quickstep_save_read_intent(&c);
    }
    adopt(selected);QuickstepSnapshot s=snapshot();
    if(poison)assert(!s.count);
    else {assert(s.count==2&&s.story==story);assert(s.entries[0].uid==UID(1)&&s.entries[1].uid==UID(2));}
    finish();
}
#ifdef QUICKSTEP_SPLIT_READ_TEST
static void invalid_copy_source(bool descriptor){
    begin(descriptor?"invalid-selected-descriptor-clears-destination":"invalid-copy-source-clears-destination",0);
    assign(1,1);write_save("save","bfsgame00.sav");restart();read_save("save","bfsgame00.sav",RAW,QUICKSTEP_SAVE_BYTES,true);
    memcpy((void*)CACHE,(void*)RAW,QUICKSTEP_SAVE_BYTES);Cpu c=state();c.x[19]=CACHE;c.x[20]=RAW;quickstep_save_cache_copy(&c);
    c=state();
    if(descriptor){c.x[21]=CACHE;c.x[19]=0;quickstep_save_selected_copy(&c);}
    else {c.x[19]=CACHE;c.x[20]=0;quickstep_save_cache_copy(&c);}
    adopt(CACHE);assert(!snapshot().count);finish();
}
#endif
#ifdef QUICKSTEP_TRANSITION_TEST
static void concurrent_adoption(void){
    begin("checked-adoption-is-serialized",0);assign(1,1);write_save("save","bfsgame00.sav");restart();
    read_save("save","bfsgame00.sav",RAW,QUICKSTEP_SAVE_BYTES,true);
    interrupt_adoption=true;adopt(RAW);assert(!pthread_join(adoption_worker,NULL));assert(!snapshot().count);finish();
}
#endif
int main(int argc,char **argv){
    const struct rlimit limit={0,0};assert(!setrlimit(RLIMIT_CORE,&limit));
    assert(mmap((void*)HEAP_BASE,0x2000000,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0)==(void*)HEAP_BASE);
    if(argc==2&&!strcmp(argv[1],"--roots"))interleaved(false);
    else if(argc==2&&!strcmp(argv[1],"--reads"))interleaved(true);
    else if(argc==2&&!strcmp(argv[1],"--short"))shortened_observation();
    else if(argc==2&&!strcmp(argv[1],"--yield-read"))lifecycle_yields(true);
    else if(argc==2&&!strcmp(argv[1],"--yield-load"))lifecycle_yields(false);
    else if(argc==2&&!strcmp(argv[1],"--yield-continue"))continue_yield();
    else if(argc==2&&!strcmp(argv[1],"--actual-split"))split_cached_read(0,2,false,false,false,true);
    else if(argc==2&&!strcmp(argv[1],"--actual-split-direct"))split_cached_read(0,0,false,false,false,true);
    else {assert(argc==1);roundtrips();interleaved(false);interleaved(true);shortened_observation();rejection();lifecycle_yields(true);lifecycle_yields(false);continue_yield();}
#ifdef QUICKSTEP_TRANSITION_TEST
    if(argc==1)concurrent_adoption();
#endif
#ifdef QUICKSTEP_SPLIT_READ_TEST
    if(argc==1){for(unsigned story=0;story<2;++story)for(unsigned copies=0;copies<3;++copies)split_cached_read(story,copies,false,false,false,true);
        split_cached_read(0,2,true,false,false,true);split_cached_read(0,2,false,false,true,true);
        split_cached_read(0,2,false,true,false,true);split_cached_read(1,2,false,true,false,true);split_cached_read(0,2,false,false,false,false);
        invalid_copy_source(false);invalid_copy_source(true);}
#endif
    assert(refreshes>0&&!munmap((void*)HEAP_BASE,0x2000000));
    printf("PASS: real Quickstep slot/save/filesystem roundtrip (%u cases)\n",cases);return 0;
}
