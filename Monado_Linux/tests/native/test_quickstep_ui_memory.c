// Authored allocator records/metrics. No game resource, native allocator or UI.
#define _GNU_SOURCE
#include "../../runtime/hle.h"
#include "../../runtime/settings.h"
#include "../../runtime/hle/quickstep_ui_memory.h"
#include <assert.h>
#include <sys/mman.h>

#define BASE 0x7200000000ull
#define OWNER (BASE+0x1000)
#define CONTEXT (BASE+0x2000)
#define MANAGER (BASE+0x3000)
#define REGISTRY (BASE+0x10000)
#define ALLOCATOR (BASE+0xd0000)
#define HEAP (BASE+0xd1000)
#define STACK (BASE+0x1f0000)
#define CLASS 0x7000b6b530ull
#define MIB (1024ull*1024)
Settings g_settings;
static uint64_t total,used,free_bytes,largest,record;
static unsigned calls,queries,depth;
static bool change_lock,change_unlock,change_allocator,change_cookie,nested;
static bool change_owner_lock,change_registry_lock,change_mutex_lock,off_lock,off_unlock;
static void setup(uint32_t selector){
    assert(!depth);memset((void*)BASE,0,0x200000);calls=queries=0;
    change_lock=change_unlock=change_allocator=change_cookie=nested=false;
    change_owner_lock=change_registry_lock=change_mutex_lock=off_lock=off_unlock=false;
    ST64(0x7000b8a278ull,OWNER);ST64(OWNER,CONTEXT);ST32(CONTEXT+0x28,selector);
    ST64(0x7000b935a8ull,MANAGER);ST64(MANAGER,REGISTRY);
    record=REGISTRY+((selector?selector:1)-1)*0x558ull;
    if(selector>=1&&selector<=512){
        ST32(REGISTRY+0xab000+4*((selector-1)>>5),1u<<((selector-1)&31));
        ST8(record+0x20,1);ST64(record+0x28,ALLOCATOR);ST8(record+0xc0,2);
    }
    ST64(ALLOCATOR,CLASS);ST64(CLASS+0x50,0x70005f3f60ull);
    ST64(ALLOCATOR+0x30,HEAP);ST64(HEAP+0x40,0x12345678);
    ST64(0x7000bb51a0ull,0x12345678);
    total=16*MIB;used=4*MIB;free_bytes=12*MIB;largest=8*MIB;
    g_settings.quickstep_slot=true;
}
void guest_call(Cpu*c,uint64_t fn){
    uint64_t a=c->x[0],b=c->x[1];++calls;
    assert(!(c->sp&15)&&c->sp<STACK);
    switch(fn){
    case 0x70005dbde0ull:
        assert(b==record);
        if(change_mutex_lock)ST8(record+0x20,0);
        if(LD8(record+0x20))++depth;
        ST64(a,record);
        if(change_lock)ST32(CONTEXT+0x28,0);
        if(change_owner_lock)ST64(0x7000b8a278ull,OWNER+0x100);
        if(change_registry_lock)ST64(MANAGER,REGISTRY+0x1000);
        if(off_lock)g_settings.quickstep_slot=false;
        if(nested&&depth==1){nested=false;assert(quickstep_ui_memory_allow(c,0x1000,"nested"));assert(depth==1);}
        break;
    case 0x70005f3f60ull:
        assert(depth&&a==ALLOCATOR&&c->x[2]==0);++queries;
        memset((void*)(uintptr_t)b,0,0x30);
        ST64(b,total);ST64(b+8,used);ST64(b+0x10,free_bytes);ST64(b+0x18,largest);
        break;
    case 0x70005dbe20ull:
        assert(LD64(a)==record);if(LD8(record+0x20)){assert(depth);--depth;}
        if(change_unlock)ST32(CONTEXT+0x28,0);
        if(change_allocator)ST64(record+0x28,ALLOCATOR+0x1000);
        if(change_cookie)ST64(HEAP+0x40,0);
        if(off_unlock)g_settings.quickstep_slot=false;
        break;
    default:assert(!"unexpected allocator preflight native helper");
    }
    memset(c,0xbc,sizeof*c);
}
static bool check(uint64_t bytes){
    Cpu c;memset(&c,0x53,sizeof c);c.sp=STACK;Cpu before=c;
    unsigned char scratch[0x80];memset(scratch,0x67,sizeof scratch);
    memcpy((void*)(STACK-0x100),scratch,sizeof scratch);
    bool allowed=quickstep_ui_memory_allow(&c,bytes,"fixture");
    assert(!memcmp(&before,&c,sizeof c)&&!depth);
    assert(!memcmp((void*)(STACK-0x100),scratch,sizeof scratch));
    return allowed;
}
int main(void){
    assert(mmap((void*)BASE,0x200000,PROT_READ|PROT_WRITE,
        MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0)==(void*)BASE);
    assert(mmap((void*)0x7000b00000ull,0x100000,PROT_READ|PROT_WRITE,
        MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0)==(void*)0x7000b00000ull);
    assert(mmap((void*)(GUEST_HI-0x1000),0x1000,PROT_READ|PROT_WRITE,
        MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0)==(void*)(GUEST_HI-0x1000));
    const uint32_t selectors[]={1,32,33,54,512};
    for(unsigned i=0;i<sizeof selectors/sizeof selectors[0];++i){setup(selectors[i]);assert(check(4*MIB)&&calls==3&&queries==1);}
    setup(54);nested=true;assert(check(4*MIB)&&calls==6&&queries==2);
    setup(54);g_settings.quickstep_slot=false;assert(!check(4*MIB)&&!calls);
    setup(54);assert(!check(0)&&!calls);assert(!check(65*MIB)&&!calls);
    setup(0);assert(!check(MIB)&&!calls);setup(513);assert(!check(MIB)&&!calls);
    setup(54);ST32(REGISTRY+0xab000+4*((54-1)>>5),0);assert(!check(MIB)&&!calls);
    setup(54);ST8(record+0xc0,0);assert(!check(MIB)&&!calls);
    setup(54);ST8(record+0x20,0);assert(!check(MIB)&&!calls);
    setup(54);ST64(ALLOCATOR,CLASS+8);assert(!check(MIB)&&calls==2&&!queries);
    setup(54);ST64(CLASS+0x50,0);assert(!check(MIB)&&calls==2&&!queries);
    setup(54);ST64(HEAP+0x40,0);assert(!check(MIB)&&calls==2&&!queries);
    setup(54);ST64(ALLOCATOR+0x30,GUEST_HI-0x48);ST64(GUEST_HI-8,0x12345678);
    assert(!check(MIB)&&calls==2&&!queries);
    setup(54);free_bytes=MIB;assert(!check(4*MIB)&&calls==3);
    setup(54);largest=0x1000;assert(!check(MIB)&&calls==3);
    setup(54);largest=0x800;assert(check(0x800)&&calls==3);
    setup(54);used=total+1;assert(!check(MIB)&&calls==3);
    setup(54);largest=free_bytes+1;assert(!check(MIB)&&calls==3);
    setup(54);free_bytes=total+1;assert(!check(MIB)&&calls==3);
    setup(54);total=0;assert(!check(MIB)&&calls==3);
    setup(54);change_lock=true;assert(!check(MIB)&&calls==2&&!queries);
    setup(54);change_owner_lock=true;assert(!check(MIB)&&calls==2&&!queries);
    setup(54);change_registry_lock=true;assert(!check(MIB)&&calls==2&&!queries);
    setup(54);change_mutex_lock=true;assert(!check(MIB)&&calls==2&&!queries);
    setup(54);off_lock=true;assert(!check(MIB)&&calls==2&&!queries);
    setup(54);off_unlock=true;assert(!check(MIB)&&calls==3);
    setup(54);change_unlock=true;assert(!check(MIB)&&calls==3);
    setup(54);change_allocator=true;assert(!check(MIB)&&calls==3);
    setup(54);change_cookie=true;assert(!check(MIB)&&calls==3);
    setup(54);Cpu c={0};c.sp=STACK+1;assert(!quickstep_ui_memory_allow(&c,MIB,"unaligned")&&!calls);
    puts("native UI memory preflight contracts: PASS");
}
