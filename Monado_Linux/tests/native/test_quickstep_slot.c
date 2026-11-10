// [XCDE] LINUX-039: authored inventory/ownership contracts; no game code or saves.
#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "hle.h"
#include "settings.h"
#include "hle/quickstep_slot.h"
#define BASE 0x7200000000ull
#define DATA (BASE+0x100000)
#define CONTEXT (BASE+0x1000)
#define VIEW (BASE+0x2000)
#define ITEM_VIEW (BASE+0x4000)
#define OWNER_CACHE (BASE+0x5000)
#define STACK (BASE+0x1000000)
#define ITEM(n) (DATA+0x41c0+0x28870+(n)*0x2c)
#define UID(n) (0x30000u|(n))
Settings g_settings;
static uint64_t resolved=DATA,allocated=BASE+0x1100000;
static unsigned refreshed[16],decoded,merged,native_calls;
static bool required_flag,reenter,reset_during_call;
static bool refresh_effect;
static uint64_t suspend_after,replace_after;
static uint64_t readopt_after;
static uint64_t marker_cache=OWNER_CACHE;
static bool marker_rebind,marker_replace;
static QuickstepSnapshot readopt_snapshot;
static uint32_t seen_count;
static void effect(Cpu *c,unsigned character,unsigned count);
int g_rt_unwind;
void rt_fatal_trace(void) { abort(); }
void rt_unwind_point(void) {}
void quickstep_save_set_required(bool needed) { required_flag=needed; }
void *hle_alloc(uint64_t bytes) { uint64_t at=allocated;allocated+=(bytes+15)&~15ull;return(void*)at; }
static Cpu cpu(void) { Cpu c;memset(&c,0x56,sizeof c);c.sp=STACK;c.x[29]=STACK+0x2c0;return c; }
static void mock_merge(uint64_t array,uint64_t count,uint64_t input) {
    unsigned n=LD32(count),index=n;
    for(unsigned i=0;i<n;i++) if(LD16(array+i*0x2c)==LD16(input)) index=i;
    if(index==n) {memcpy((void*)(array+n*0x2c),(void*)input,0x2c);ST32(count,n+1);}
    else {unsigned value=LD16(array+index*0x2c+0xe)+LD16(input+0xe);ST16(array+index*0x2c+0xe,value>25?25:value);}
}
void guest_call(Cpu *c,uint64_t function) {
    ++native_calls;
    uint64_t a=c->x[0],b=c->x[1],d=c->x[2],e=c->x[3],f=c->x[4],ret=0;
    switch(function) {
    case 0x7000592b80ull: assert(LD64(a)==CONTEXT);ret=resolved;break;
    case 0x7000592a30ull: assert(LD64(a)==CONTEXT);ret=VIEW;break;
    case 0x7000174ed0ull:
        assert(a==CONTEXT&&!b&&!d&&!e&&!f);ret=marker_cache;
        if(marker_rebind) {marker_rebind=false;ST64(ITEM_VIEW+0x138,ITEM(0));}
        if(marker_replace) {marker_replace=false;ST32(ITEM(5)+0xc,LD32(ITEM(5)+0xc)+1);}
        break;
    case 0x70000afab0ull:assert(a==CONTEXT&&b>0&&b<16);ret=DATA+0x154630+(b-1)*0x3480;break;
    case 0x700008b860ull:
        assert(a==DATA+0x154634+(d-1)*0x3480&&b==CONTEXT&&!e);++refreshed[d];
        if(refresh_effect) {
            refresh_effect=false;Cpu nested=*c;nested.x[29]=nested.sp+0x2c0;effect(&nested,(unsigned)d,0);
            quickstep_slot_effects(&nested);assert(nested.x[21]==1);
            quickstep_slot_effects_end(&nested);quickstep_slot_effects_finish(&nested);
        }
        if(reenter) {reenter=false;quickstep_slot_tick(c,CONTEXT);}
        if(reset_during_call) {reset_during_call=false;quickstep_slot_suspend();}
        break;
    case 0x70000a8290ull:
        assert(a==CONTEXT&&(b&0xffff0000)==0x30000&&!d&&e==f+4);
        ++decoded;ST16(f,77);ST16(f+0xe,LD8(ITEM(b&0xffff)+0x1e));ST16(f+0x16,25);ret=1;break;
    case 0x700008a800ull:assert(a==BASE+0x3000);++merged;mock_merge(b,d,e);seen_count=LD32(d);break;
    default:fprintf(stderr,"unexpected call %llx\n",(unsigned long long)function);abort();
    }
    if(function==suspend_after) {suspend_after=0;quickstep_slot_suspend();}
    if(function==replace_after) {replace_after=0;ST32(ITEM(0)+0xc,LD32(ITEM(0)+0xc)+1);}
    if(function==readopt_after) {readopt_after=0;quickstep_slot_adopt(DATA,0,&readopt_snapshot);}
    memset(c,0xad,sizeof*c);c->x[0]=ret;
}
static void owned(unsigned n,unsigned serial,unsigned value) {
    memset((void*)ITEM(n),0,0x2c);ST32(ITEM(n),UID(n));ST16(ITEM(n)+4,4580);ST16(ITEM(n)+6,3);
    ST16(ITEM(n)+8,1);ST32(ITEM(n)+0xc,serial);ST8(ITEM(n)+0x10,1);ST8(ITEM(n)+0x16,6);
    ST8(ITEM(n)+0x1a,1);ST16(ITEM(n)+0x1c,77);ST8(ITEM(n)+0x1e,value);
}
static uint64_t gear(unsigned type,bool special,unsigned index,unsigned sockets) {
    static const unsigned normal[]={0,0x5dc0,0xbb80,0x11940,0x17700,0x1d4c0};
    static const unsigned unique[]={0x40470,0x40a70,0x41070,0x41670,0x41c70,0x42270};
    assert(type==2||(type>=4&&type<=8));unsigned p=type==2?0:type-3;
    assert(index<(special?32u:500u));
    uint64_t at=DATA+0x41c0+(special?unique[p]:normal[p])+index*0x30;
    memset((void*)at,0,0x30);ST32(at,(special?0x80000000u:0)|(type<<16)|index);
    ST16(at+4,1);ST16(at+6,type);ST8(at+0x10,1);ST8(at+0x15,sockets);return at;
}
static void incoming(Cpu *c,uint64_t parent,unsigned socket,unsigned uid) {
    c->x[8]=parent+0x18;c->x[9]=socket*8;c->x[19]=uid;
}
static void assign(Cpu *c,unsigned character,unsigned n) {
    QuickstepGem gems[8];size_t count=quickstep_slot_list(c,CONTEXT,character,gems,8);assert(count>0);
    QuickstepGem *found=NULL;for(size_t i=0;i<count;i++)if(gems[i].uid==UID(n))found=&gems[i];
    assert(found&&quickstep_slot_set_checked(c,CONTEXT,character,found->uid,found->fingerprint));
}
static void effect(Cpu *c,unsigned character,unsigned count) {
    c->x[19]=c->sp+0x98;c->x[21]=count;c->x[24]=CONTEXT;ST32(c->sp+0x58,character);
    c->x[20]=DATA+0x154634+(character-1)*0x3480;
    ST64(c->sp+0x28,BASE+0x3000);ST32(c->sp+0x1fc,count);
}
int main(void) {
    assert(mmap((void*)BASE,0x2000000,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0)==(void*)BASE);
    Cpu c=cpu(),before=c;owned(0,100,25);owned(1,101,10);owned(2,102,22);
    // No adopted playing object: UI/effects cannot attach to speculative decodes.
    g_settings.quickstep_slot=true;assert(!quickstep_slot_list(&c,CONTEXT,1,NULL,0));
    assert(!memcmp(&c,&before,sizeof c));
    quickstep_slot_adopt(DATA,0,NULL);reenter=true;quickstep_slot_tick(&c,CONTEXT);
    for(unsigned i=1;i<=8;i++)assert(refreshed[i]==1);assert(!memcmp(&c,&before,sizeof c));
    QuickstepGem gems[8],item;size_t owned_count=quickstep_slot_list(&c,CONTEXT,1,gems,8);
    if(owned_count!=3) {fprintf(stderr,"FAIL actual native rankType6 owned Quick Step gems rejected\n");return 1;}
    assert(gems[0].rank==6&&gems[0].value==25&&!gems[0].assigned&&required_flag);
    assign(&c,1,0);assert(quickstep_slot_get(&c,CONTEXT,1,&item)&&item.uid==UID(0));
    assert(!quickstep_slot_set_checked(&c,CONTEXT,2,item.uid,item.fingerprint));
    assert(quickstep_slot_list(&c,CONTEXT,2,gems,8)==3);
    assert(gems[0].availability==QUICKSTEP_ASSIGNED_ELSEWHERE && gems[0].character==1);
    assert(!quickstep_slot_set_checked(&c,CONTEXT,9,item.uid,item.fingerprint));
    effect(&c,1,8);c.x[20]+=0x400000;before=c;quickstep_slot_effects(&c);
    assert(!memcmp(&c,&before,sizeof c)&&!decoded&&!merged);
    unsigned refreshes=refreshed[1];refresh_effect=true;quickstep_slot_effects_finish(&c);
    assert(!memcmp(&c,&before,sizeof c)&&refreshed[1]==refreshes+1&&decoded==1&&merged==1);
    // Common final boundary is idempotent, and stale temporary frames cannot
    // refresh a newly adopted playing object after a load begins.
    quickstep_slot_effects_finish(&c);assert(refreshed[1]==refreshes+1);
    effect(&c,1,8);c.x[20]+=0x400000;quickstep_slot_effects(&c);
    QuickstepSnapshot early;quickstep_slot_snapshot(&early);quickstep_slot_suspend();before=c;
    quickstep_slot_effects_finish(&c);assert(!memcmp(&c,&before,sizeof c)&&refreshed[1]==refreshes+1);
    quickstep_slot_adopt(DATA,0,&early);quickstep_slot_tick(&c,CONTEXT);
    // All eight original records/count and adjacent stack storage remain intact.
    memset((void*)(c.sp+0x98),0,8*0x2c);for(unsigned i=0;i<8;i++)ST16(c.sp+0x98+i*0x2c,200+i);
    effect(&c,1,8);unsigned char original[0x240];memcpy(original,(void*)c.sp,sizeof original);before=c;
    quickstep_slot_effects(&c);assert(c.x[21]==9&&seen_count==9&&c.x[19]!=before.x[19]);
    assert(!memcmp(original,(void*)c.sp,sizeof original));before.x[19]=c.x[19];before.x[21]=c.x[21];assert(!memcmp(&c,&before,sizeof c));
    c.x[19]+=4+c.x[21]*44;c.x[22]=c.x[21];quickstep_slot_effects_end(&c);
    assert(c.x[19]==c.sp+0x98+4+8*44&&c.x[21]==8&&c.x[22]==8);
    before=c;quickstep_slot_effects_end(&c);assert(!memcmp(&c,&before,sizeof c));
    // Existing Quick Step merges through native merger rather than a second cap.
    effect(&c,1,8);ST16(c.sp+0x98,77);ST16(c.sp+0x98+0xe,10);
    quickstep_slot_effects(&c);assert(c.x[21]==8&&LD16(c.x[19]+0xe)==25);quickstep_slot_effects_end(&c);
    // Originally empty collection skipped its loop and x22 initialization.
    effect(&c,1,0);before=c;quickstep_slot_effects(&c);assert(c.x[21]==1);
    c.x[22]=1;quickstep_slot_effects_end(&c);
    assert(c.x[19]==before.x[19]&&c.x[21]==0&&c.x[22]==before.x[22]);
    // Disabled retains association/real gem but removes effect; inactive use clears it.
    QuickstepSnapshot snapshot;quickstep_slot_snapshot(&snapshot);assert(snapshot.count==1);
    // Yield during either decoder/merger: old ownership cannot publish a lease.
    uint64_t stages[]={0x70000a8290ull,0x700008a800ull};
    for(unsigned j=0;j<2;j++) for(unsigned repeat=0;repeat<40;repeat++) {
        quickstep_slot_adopt(DATA,0,&snapshot);quickstep_slot_tick(&c,CONTEXT);
        effect(&c,1,8);before=c;suspend_after=stages[j];quickstep_slot_effects(&c);
        assert(!memcmp(&c,&before,sizeof c));quickstep_slot_effects_end(&c);
    }
    quickstep_slot_adopt(DATA,0,&snapshot);quickstep_slot_tick(&c,CONTEXT);
    effect(&c,1,8);before=c;replace_after=0x70000a8290ull;quickstep_slot_effects(&c);
    assert(!memcmp(&c,&before,sizeof c));quickstep_slot_effects_end(&c);assign(&c,1,0);
    g_settings.quickstep_slot=false;quickstep_slot_tick(&c,CONTEXT);effect(&c,1,8);before=c;
    quickstep_slot_effects(&c);assert(!memcmp(&c,&before,sizeof c));quickstep_slot_snapshot(&snapshot);assert(snapshot.count==1);
    uint64_t boots=gear(8,false,0,1);
    incoming(&c,boots,0,UID(0));before=c;quickstep_slot_native_equip(&c);
    assert(!memcmp(&c,&before,sizeof c));quickstep_slot_snapshot(&snapshot);assert(!snapshot.count&&required_flag==false);
    assert(LD32(ITEM(0))==UID(0)&&LD8(ITEM(0)+0x10));
    // Serial rejects identical replacement and stale UI commands; no phantom item.
    g_settings.quickstep_slot=true;assign(&c,1,1);assert(quickstep_slot_get(&c,CONTEXT,1,&item));
    owned(1,501,10);assert(!quickstep_slot_set_checked(&c,CONTEXT,1,item.uid,item.fingerprint));
    quickstep_slot_snapshot(&snapshot);assert(!snapshot.count);
    // A later acquired/reused gem cannot enter the already serialized snapshot.
    assign(&c,1,0);uint64_t raw=BASE+0x1300000;
    memcpy((void*)raw,(void*)(DATA+0x6b0),0x153f30);
    QuickstepSnapshot frozen;assert(quickstep_slot_snapshot_for_save(raw,&frozen)&&frozen.count==1);
    unsigned serial=LD32(ITEM(0)+0xc);owned(0,serial+1,25);assign(&c,1,0);
    assert(quickstep_slot_snapshot_for_save(raw,&frozen)&&!frozen.count);
    quickstep_slot_snapshot(&snapshot);assert(snapshot.count==1);
    assert(quickstep_slot_set_checked(&c,CONTEXT,1,0,NULL));
    // Native +11 is favourite/lock. Toggling it preserves the exact identity,
    // assignment and effect eligibility, including after a metadata reload.
    assign(&c,1,0);assert(quickstep_slot_get(&c,CONTEXT,1,&item));
    uint8_t identity[32];memcpy(identity,item.fingerprint,sizeof identity);
    ST8(ITEM(0)+0x11,1);assert(quickstep_slot_get(&c,CONTEXT,1,&item));
    assert(!memcmp(identity,item.fingerprint,sizeof identity));
    quickstep_slot_snapshot(&snapshot);quickstep_slot_adopt(DATA,0,&snapshot);
    assert(quickstep_slot_get(&c,CONTEXT,1,&item));ST8(ITEM(0)+0x11,0);
    assert(quickstep_slot_get(&c,CONTEXT,1,&item));
    // All native gear pools, including their last instance, reserve an owned
    // gem through actual socket refs even if that gear is not currently worn.
    unsigned types[]={2,4,5,6,7,8};
    for(unsigned special=0;special<2;special++)for(unsigned p=0;p<6;p++) {
        uint64_t at=gear(types[p],special,special?31:499,3);
        ST32(at+0x28,UID(0));
        assert(!quickstep_slot_get(&c,CONTEXT,1,&item));
        assert(quickstep_slot_list(&c,CONTEXT,1,gems,8)==3);
        assert(gems[0].availability==QUICKSTEP_ORDINARY_EQUIPPED);
        assert(!quickstep_slot_set_checked(&c,CONTEXT,1,UID(0),identity));
        ST8(at+0x2e,1);assign(&c,1,0); // Static/intrinsic refs do not own it.
        ST8(at+0x2e,0);ST8(at+0x10,0);assert(quickstep_slot_get(&c,CONTEXT,1,&item));
        memset((void*)at,0,0x30);
    }
    // Ordinary socket hook mirrors the native successful write boundary only.
    // A rejected slot, intrinsic socket, inactive parent, foreign data or clear
    // must not revoke the bonus assignment. Favourite operations use no hook.
    boots=gear(8,true,2,1);
    incoming(&c,boots,1,UID(0));before=c;quickstep_slot_native_equip(&c);assert(!memcmp(&c,&before,sizeof c));
    assert(quickstep_slot_get(&c,CONTEXT,1,&item));
    incoming(&c,boots,0,UID(0));ST8(boots+0x1e,1);quickstep_slot_native_equip(&c);
    assert(quickstep_slot_get(&c,CONTEXT,1,&item));ST8(boots+0x1e,0);
    ST8(boots+0x10,0);quickstep_slot_native_equip(&c);assert(quickstep_slot_get(&c,CONTEXT,1,&item));ST8(boots+0x10,1);
    incoming(&c,boots+0x400000,0,UID(0));quickstep_slot_native_equip(&c);assert(quickstep_slot_get(&c,CONTEXT,1,&item));
    incoming(&c,boots,0,0);quickstep_slot_native_equip(&c);assert(quickstep_slot_get(&c,CONTEXT,1,&item));
    incoming(&c,boots,0,UID(0));before=c;quickstep_slot_native_equip(&c);assert(!memcmp(&c,&before,sizeof c));
    quickstep_slot_snapshot(&snapshot);assert(!snapshot.count);assert(!LD32(boots+0x18));
    ST32(boots+0x18,UID(0));assert(!quickstep_slot_set_checked(&c,CONTEXT,1,UID(0),identity));
    ST32(boots+0x18,0);assign(&c,1,0);
    // The same attachment proof filters the exact serialized save snapshot.
    memcpy((void*)raw,(void*)(DATA+0x6b0),0x153f30);
    assert(quickstep_slot_snapshot_for_save(raw,&frozen)&&frozen.count==1);
    ST32(raw+(boots-DATA)-0x6b0+0x18,UID(0));
    assert(quickstep_slot_snapshot_for_save(raw,&frozen)&&!frozen.count);
    assert(quickstep_slot_get(&c,CONTEXT,1,&item));
    // Native Remove cannot clear an identity changed after the user saw it,
    // nor an identical-looking association adopted during a yielding helper.
    assert(!quickstep_slot_clear_checked(&c,CONTEXT,1,UID(1),identity));
    assert(!quickstep_slot_clear_checked(&c,CONTEXT,1,item.uid,NULL));
    assert(quickstep_slot_get(&c,CONTEXT,1,&item));
    quickstep_slot_snapshot(&readopt_snapshot);readopt_after=0x7000592b80ull;
    assert(!quickstep_slot_clear_checked(&c,CONTEXT,1,item.uid,item.fingerprint));
    assert(quickstep_slot_get(&c,CONTEXT,1,&item));
    quickstep_slot_snapshot(&readopt_snapshot);readopt_after=0x7000592b80ull;
    assert(!quickstep_slot_set_checked(&c,CONTEXT,1,item.uid,item.fingerprint));
    assert(quickstep_slot_get(&c,CONTEXT,1,&item));
    assert(quickstep_slot_clear_checked(&c,CONTEXT,1,item.uid,item.fingerprint));assign(&c,1,0);
    // Both native inventory consumers invalidate without changing an item.
    c.x[0]=CONTEXT;c.x[1]=ITEM(0);c.x[2]=0x10000;before=c;
    quickstep_slot_native_consume(&c);assert(!memcmp(&c,&before,sizeof c));
    assert(quickstep_slot_get(&c,CONTEXT,1,&item));
    memcpy((void*)(ITEM(0)+0x400000),(void*)ITEM(0),0x2c);
    c.x[1]=ITEM(0)+0x400000;c.x[2]=1;quickstep_slot_native_consume(&c);
    assert(quickstep_slot_get(&c,CONTEXT,1,&item));
    assign(&c,1,0);c.x[0]=CONTEXT;c.x[1]=ITEM(0);c.x[2]=1;before=c;quickstep_slot_native_consume(&c);assert(!memcmp(&c,&before,sizeof c));
    quickstep_slot_snapshot(&snapshot);assert(!snapshot.count);
    assign(&c,1,2);c.x[0]=DATA+0x41c0;c.x[1]=ITEM(2);before=c;quickstep_slot_native_destroy(&c);assert(!memcmp(&c,&before,sizeof c));
    quickstep_slot_snapshot(&snapshot);assert(!snapshot.count);
    // Persisted identities roundtrip only into the confirmed same-story object.
    assign(&c,1,0);quickstep_slot_snapshot(&snapshot);quickstep_slot_suspend();assert(!quickstep_slot_get(&c,CONTEXT,1,&item));
    quickstep_slot_adopt(DATA,0,&snapshot);assert(quickstep_slot_get(&c,CONTEXT,1,&item));
    resolved=DATA+0x400000;effect(&c,1,8);before=c;quickstep_slot_effects(&c);assert(!memcmp(&c,&before,sizeof c));resolved=DATA;
    ST8(VIEW+0x150,8);quickstep_slot_adopt(DATA,1,&snapshot);assert(!quickstep_slot_get(&c,CONTEXT,1,&item));
    assign(&c,14,1);assign(&c,15,2);assert(!quickstep_slot_set_checked(&c,CONTEXT,2,0,NULL));
    quickstep_slot_snapshot(&snapshot);assert(snapshot.story==1&&snapshot.count==2);
    // Generation changes during a native refresh do not resume an obsolete pass.
    reset_during_call=true;quickstep_slot_adopt(DATA,1,&snapshot);quickstep_slot_tick(&c,CONTEXT);
    assert(!quickstep_slot_list(&c,CONTEXT,14,NULL,0));
    assert(decoded==85&&merged==44);
    // Dynamically crafted gems are real type3 pool instances even when their
    // static definition is zero. Each rank keeps serial/payload ownership and
    // save identity; cylinders and zero-serial records remain ineligible.
    memset((void*)(DATA+0x41c0),0,0x42870);ST8(VIEW+0x150,0);
    g_settings.quickstep_slot=true;c=cpu();
    for(unsigned rank=1;rank<=6;rank++) {
        unsigned n=rank-1;owned(n,2000+rank,rank==6?25:rank+5);
        ST16(ITEM(n)+4,0);ST8(ITEM(n)+0x16,rank);
    }
    ST8(ITEM(1)+0x11,1);quickstep_slot_adopt(DATA,0,NULL);
    assert(quickstep_slot_list(&c,CONTEXT,1,gems,8)==6);
    for(unsigned n=0;n<6;n++) {
        assert(gems[n].uid==UID(n)&&gems[n].rank==n+1&&gems[n].availability==QUICKSTEP_FREE);
        before=c;assert(quickstep_slot_set_checked(&c,CONTEXT,1,gems[n].uid,gems[n].fingerprint));
        assert(!memcmp(&c,&before,sizeof c)&&quickstep_slot_get(&c,CONTEXT,1,&item));
        assert(item.uid==UID(n)&&!memcmp(item.fingerprint,gems[n].fingerprint,32));
        quickstep_slot_snapshot(&snapshot);quickstep_slot_suspend();quickstep_slot_adopt(DATA,0,&snapshot);
        assert(quickstep_slot_get(&c,CONTEXT,1,&item)&&item.uid==UID(n));
        memcpy((void*)raw,(void*)(DATA+0x6b0),0x153f30);
        assert(quickstep_slot_snapshot_for_save(raw,&frozen)&&frozen.count==1&&frozen.entries[0].uid==UID(n));
    }
    // Presentation publication can revalidate without yielding or mutating an
    // association. Capture only after get() has resolved the current context.
    uint64_t token=0;unsigned calls_before=native_calls;
    assert(quickstep_slot_reference_matches(CONTEXT,1,&item,&token)&&token);
    assert(native_calls==calls_before);
    uint64_t invalid_token=0;
    assert(!quickstep_slot_reference_matches(0,1,&item,&invalid_token));
    assert(!quickstep_slot_reference_matches(CONTEXT+8,1,&item,&invalid_token));
    assert(!quickstep_slot_reference_matches(CONTEXT,2,&item,&invalid_token));
    assert(!quickstep_slot_reference_matches(CONTEXT,1,NULL,&invalid_token));
    assert(!quickstep_slot_reference_matches(CONTEXT,1,&item,NULL));
    assert(!invalid_token&&native_calls==calls_before);
    boots=gear(8,false,0,1);ST32(boots+0x18,item.uid);
    assert(!quickstep_slot_reference_matches(CONTEXT,1,&item,&token));
    quickstep_slot_snapshot(&frozen);assert(frozen.count==1); // Query did not revoke it.
    ST32(boots+0x18,0);assert(quickstep_slot_reference_matches(CONTEXT,1,&item,&token));
    quickstep_slot_snapshot(&snapshot);quickstep_slot_adopt(DATA,0,&snapshot);
    assert(quickstep_slot_get(&c,CONTEXT,1,&item));calls_before=native_calls;
    assert(!quickstep_slot_reference_matches(CONTEXT,1,&item,&token));
    uint64_t adopted=0;assert(quickstep_slot_reference_matches(CONTEXT,1,&item,&adopted)&&adopted!=token);
    assert(native_calls==calls_before);
    g_settings.quickstep_slot=false;
    assert(!quickstep_slot_reference_matches(CONTEXT,1,&item,&adopted));
    g_settings.quickstep_slot=true;
    // Two equally ranked native instances remain two owned rows. A read-only
    // presentation query identifies the exact assigned one without hiding it
    // from ordinary native transfer, cloning it or inventing a gear parent.
    owned(4,2050,20);owned(5,2051,20);assign(&c,1,5);
    QuickstepGem owner;uint64_t owner_epoch=0;calls_before=native_calls;
    unsigned char inventory_before[0x2c*6];memcpy(inventory_before,(void*)ITEM(0),sizeof inventory_before);
    assert(quickstep_slot_owner(CONTEXT,ITEM(5),&owner,&owner_epoch));
    assert(owner.assigned&&owner.character==1&&owner.uid==UID(5)&&owner_epoch);
    assert(!quickstep_slot_owner(CONTEXT,ITEM(4),&owner,&owner_epoch));
    assert(!quickstep_slot_owner(CONTEXT,ITEM(5)+1,&owner,&owner_epoch));
    assert(!quickstep_slot_owner(CONTEXT,ITEM(5)+0x400000,&owner,&owner_epoch));
    assert(!quickstep_slot_owner(CONTEXT+8,ITEM(5),&owner,&owner_epoch));
    assert(!quickstep_slot_owner(CONTEXT,ITEM(5),NULL,&owner_epoch));
    assert(!quickstep_slot_owner(CONTEXT,ITEM(5),&owner,NULL));
    assert(native_calls==calls_before&&!memcmp(inventory_before,(void*)ITEM(0),sizeof inventory_before));
    quickstep_slot_snapshot(&snapshot);quickstep_slot_adopt(DATA,0,&snapshot);
    assert(!quickstep_slot_owner(CONTEXT,ITEM(5),&owner,&owner_epoch));
    assert(quickstep_slot_get(&c,CONTEXT,1,&item));
    assert(!quickstep_slot_owner(CONTEXT,ITEM(5),&owner,&owner_epoch)); // Old epoch.
    owner_epoch=0;assert(quickstep_slot_owner(CONTEXT,ITEM(5),&owner,&owner_epoch));
    // Actual row binder uses the game's own E renderer: -1 none, 1 this
    // character, 0 another character, 3 attached to unworn ordinary gear.
    // Native status and ordinary ownership queries are never overridden.
    memset((void*)ITEM_VIEW,0,0x140);ST64(ITEM_VIEW+8,CONTEXT);
    ST32(ITEM_VIEW+0x28,3);ST64(ITEM_VIEW+0x138,ITEM(5));
    for(unsigned selected=0;selected<16;selected++) {
        ST32(OWNER_CACHE+8,selected);c=cpu();c.x[20]=ITEM_VIEW;c.x[0]=UINT32_MAX;before=c;
        quickstep_slot_native_marker(&c);before.x[0]=selected==1?1:0;
        assert(!memcmp(&c,&before,sizeof c));
    }
    // Item Details portraits have a separate native owning-character query.
    // Keep the real native owner intact and supply only a validated bonus
    // owner for native0. The original portrait comparison chooses the E mark.
    c=cpu();c.x[20]=ITEM_VIEW;c.x[0]=0;before=c;calls_before=native_calls;
    quickstep_slot_native_portrait_owner(&c);before.x[0]=1;
    assert(!memcmp(&c,&before,sizeof c)&&native_calls==calls_before);
    for(unsigned native_owner=1;native_owner<16;native_owner++) {
        c.x[0]=native_owner;before=c;quickstep_slot_native_portrait_owner(&c);
        assert(!memcmp(&c,&before,sizeof c)&&native_calls==calls_before);
    }
    c.x[0]=0;ST32(ITEM_VIEW+0x28,4);before=c;quickstep_slot_native_portrait_owner(&c);
    assert(!memcmp(&c,&before,sizeof c));ST32(ITEM_VIEW+0x28,3);
    ST64(ITEM_VIEW+0x138,ITEM(4));before=c;quickstep_slot_native_portrait_owner(&c);
    assert(!memcmp(&c,&before,sizeof c));ST64(ITEM_VIEW+0x138,ITEM(5));
    g_settings.quickstep_slot=false;before=c;quickstep_slot_native_portrait_owner(&c);
    assert(!memcmp(&c,&before,sizeof c));g_settings.quickstep_slot=true;
    ST64(ITEM_VIEW+8,CONTEXT+8);before=c;quickstep_slot_native_portrait_owner(&c);
    assert(!memcmp(&c,&before,sizeof c));ST64(ITEM_VIEW+8,CONTEXT);
    calls_before=native_calls;
    for(unsigned result=0;result<4;result++) {
        c=cpu();c.x[20]=ITEM_VIEW;c.x[0]=result;before=c;quickstep_slot_native_marker(&c);
        assert(!memcmp(&c,&before,sizeof c)&&native_calls==calls_before);
    }
    ST64(ITEM_VIEW+0x138,ITEM(4));c.x[0]=UINT32_MAX;before=c;
    quickstep_slot_native_marker(&c);assert(!memcmp(&c,&before,sizeof c)&&native_calls==calls_before);
    ST64(ITEM_VIEW+0x138,ITEM(5));g_settings.quickstep_slot=false;before=c;
    quickstep_slot_native_marker(&c);assert(!memcmp(&c,&before,sizeof c)&&native_calls==calls_before);
    assert(!quickstep_slot_owner(CONTEXT,ITEM(5),&owner,&owner_epoch));g_settings.quickstep_slot=true;
    marker_cache=0;before=c;quickstep_slot_native_marker(&c);assert(!memcmp(&c,&before,sizeof c));marker_cache=OWNER_CACHE;
    quickstep_slot_snapshot(&readopt_snapshot);readopt_after=0x7000174ed0ull;
    before=c;quickstep_slot_native_marker(&c);assert(!memcmp(&c,&before,sizeof c));
    assert(quickstep_slot_get(&c,CONTEXT,1,&item));
    marker_rebind=true;before=c;quickstep_slot_native_marker(&c);assert(!memcmp(&c,&before,sizeof c));
    ST64(ITEM_VIEW+0x138,ITEM(5));marker_replace=true;before=c;
    quickstep_slot_native_marker(&c);assert(!memcmp(&c,&before,sizeof c));
    assert(!quickstep_slot_get(&c,CONTEXT,1,&item));assign(&c,1,5);assert(quickstep_slot_get(&c,CONTEXT,1,&item));
    // A normal accepted write still transfers a displayed bonus gem. The read-
    // only marker cannot create a fake native parent or make transfer a reject.
    boots=gear(8,false,0,1);incoming(&c,boots,0,UID(5));before=c;quickstep_slot_native_equip(&c);
    assert(!memcmp(&c,&before,sizeof c));owner_epoch=0;
    assert(!quickstep_slot_owner(CONTEXT,ITEM(5),&owner,&owner_epoch));
    assert(!quickstep_slot_get(&c,CONTEXT,1,&item));assign(&c,1,5);assert(quickstep_slot_get(&c,CONTEXT,1,&item));
    owner_epoch=0;ST32(boots+0x18,UID(5));
    assert(!quickstep_slot_owner(CONTEXT,ITEM(5),&owner,&owner_epoch));
    c.x[20]=ITEM_VIEW;c.x[0]=0;before=c;quickstep_slot_native_portrait_owner(&c);
    assert(!memcmp(&c,&before,sizeof c));ST32(boots+0x18,0);
    assert(!memcmp(inventory_before,(void*)ITEM(0),5*0x2c));
    // A crafted UID reused for an identical-looking new acquisition is distinct.
    QuickstepGem stale=item;ST32(ITEM(5)+0xc,9000);
    assert(!quickstep_slot_reference_matches(CONTEXT,1,&item,&adopted));
    assert(!quickstep_slot_get(&c,CONTEXT,1,&item));
    assert(!quickstep_slot_set_checked(&c,CONTEXT,1,stale.uid,stale.fingerprint));
    assert(quickstep_slot_list(&c,CONTEXT,1,gems,8)==6);
    // A real ordinary attachment excludes a crafted gem, with no fake item,
    // refund, equipment write or changed inventory record by this adapter.
    boots=gear(8,false,0,1);ST32(boots+0x18,UID(0));
    assert(!quickstep_slot_set_checked(&c,CONTEXT,1,gems[0].uid,gems[0].fingerprint));
    assert(LD32(boots+0x18)==UID(0)&&LD16(ITEM(0)+4)==0);
    ST16(ITEM(0)+6,9);ST32(ITEM(1)+0xc,0);
    assert(quickstep_slot_list(&c,CONTEXT,1,gems,8)==4);
    // Display lookup follows every supported character/story and fails closed
    // when the current native story changes even before the next UI tick.
    memset((void*)(DATA+0x41c0),0,0x42870);owned(0,2200,20);
    for(unsigned story=0;story<2;story++)for(unsigned ch=1;ch<16;ch++) {
        bool allowed=story==0?ch<=8:(ch==1||ch==7||ch==14||ch==15);
        if(!allowed)continue;
        ST8(VIEW+0x150,story?8:0);quickstep_slot_adopt(DATA,story,NULL);assign(&c,ch,0);
        owner_epoch=0;assert(quickstep_slot_owner(CONTEXT,ITEM(0),&owner,&owner_epoch));
        assert(owner.character==ch&&owner.assigned);
        ST64(ITEM_VIEW+0x138,ITEM(0));ST32(OWNER_CACHE+8,ch);
        c=cpu();c.x[20]=ITEM_VIEW;c.x[0]=UINT32_MAX;before=c;
        quickstep_slot_native_marker(&c);before.x[0]=1;assert(!memcmp(&c,&before,sizeof c));
        c.x[0]=0;before=c;quickstep_slot_native_portrait_owner(&c);before.x[0]=ch;
        assert(!memcmp(&c,&before,sizeof c));
        ST8(VIEW+0x150,story?0:8);calls_before=native_calls;c.x[0]=UINT32_MAX;before=c;
        assert(!quickstep_slot_owner(CONTEXT,ITEM(0),&owner,&owner_epoch));
        quickstep_slot_native_marker(&c);assert(!memcmp(&c,&before,sizeof c)&&native_calls==calls_before);
        c.x[0]=0;before=c;quickstep_slot_native_portrait_owner(&c);
        assert(!memcmp(&c,&before,sizeof c)&&native_calls==calls_before);
    }
    puts("quickstep owned gem, stale serial, active story, native merge, lease, OFF, native E marker, portrait owner and mutation contracts: PASS");
}
