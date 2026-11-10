// Authored native-summary contracts. Synthetic records only; no game assets.
#define _GNU_SOURCE
#include "hle.h"
#include "settings.h"
#include "hle/quickstep_stats.h"
#include "hle/quickstep_stats_ui.h"
#include "hle/quickstep_slot.h"
#include <assert.h>
#include <sys/mman.h>
#define BASE 0x7200000000ull
#define SEQ (BASE+0x10000)
#define PANEL (BASE+0x20000)
#define LIST (BASE+0x30000)
#define CONTEXT (BASE+0x40000)
#define CACHE (BASE+0x50000)
#define TABLE (BASE+0x52000)
#define FIELD (BASE+0x54000)
#define RAW (BASE+0x55000)
#define MESSAGE (BASE+0x56000)
#define CACHE_DATA (BASE+0x58000)
#define OLD (BASE+0x60000)
#define NEW (BASE+0x61000)
#define STACK (BASE+0x400000)
Settings g_settings;
int g_rt_unwind;
static uint64_t owner_generation=1,epoch=1,allocation=BASE+0x100000,invalidate_at;
static uint32_t character=1;
static bool owner=true,assigned=true,bonus_picker,replace_source,replace_identity,invalidate_get;
static bool fail_ensure,invalidate_ensure,invalidate_layout;
static unsigned native_calls,baseline_calls,ensure_calls,layout_on,layout_off;
static QuickstepGem gem;
void rt_fatal_trace(void) { abort(); }
void rt_unwind_point(void) {}
void *hle_alloc(uint64_t n) {assert(n==0x80);uint64_t p=allocation;allocation+=n;return(void*)p;}
bool quickstep_native_stats_context(uint64_t panel,uint32_t ch,uint64_t *seq,uint64_t *ctx,uint64_t *gen) {
    if(!owner||panel!=PANEL||ch!=character)return false;
    *seq=SEQ;*ctx=CONTEXT;*gen=owner_generation;return true;
}
bool quickstep_native_picker_active(uint64_t seq) {assert(seq==SEQ);return bonus_picker;}
bool quickstep_stats_ui_ensure(Cpu*c,uint64_t panel,uint32_t ch) {
    (void)c;assert(panel==PANEL&&ch==character);++ensure_calls;
    if(fail_ensure)return false;
    ST64(PANEL+0x1948,20);
    if(invalidate_ensure){invalidate_ensure=false;++owner_generation;}
    return true;
}
void quickstep_stats_ui_layout(Cpu*c,uint64_t panel,bool ninth) {
    (void)c;if(panel!=PANEL)return;
    if(ninth){assert(LD64(PANEL+0x1948)==20);++layout_on;
        if(invalidate_layout){invalidate_layout=false;++owner_generation;}}
    else ++layout_off;
}
bool quickstep_slot_get(Cpu*c,uint64_t ctx,uint32_t ch,QuickstepGem*out) {
    (void)c;assert(ctx==CONTEXT&&ch==character);*out=gem;
    if(invalidate_get){invalidate_get=false;++owner_generation;}
    return assigned;
}
bool quickstep_slot_reference_matches(uint64_t ctx,uint32_t ch,const QuickstepGem *item,uint64_t *token) {
    if(!g_settings.quickstep_slot||!assigned||ctx!=CONTEXT||ch!=character||!item||!token
       ||item->uid!=gem.uid||memcmp(item->fingerprint,gem.fingerprint,32)||(*token&&*token!=epoch))return false;
    *token=epoch;return true;
}
void guest_call(Cpu*c,uint64_t fn) {
    uint64_t a=c->x[0],b=c->x[1],d=c->x[2],e=c->x[3],f=c->x[4],x0=0,x1=0;
    ++native_calls;
    switch(fn) {
    case 0x70000a8290ull:
        assert(a==CONTEXT&&b==gem.uid&&!d&&e==f+4);
        memset((void*)f,0,0x2c);ST16(f,77);ST16(f+0xa,3);ST16(f+0xe,gem.value);ST16(f+0x16,25);x0=1;break;
    case 0x70000ae8e0ull:assert(a==CONTEXT);x0=CACHE;break;
    case 0x700006b270ull:assert(a==CACHE&&b==0x14);x0=TABLE;break;
    case 0x7000062310ull:assert(a==TABLE&&!strcmp((char*)b,"name"));x0=FIELD;break;
    case 0x7000069320ull:assert(a==CACHE_DATA&&b==2);x0=RAW;break;
    case 0x7000510630ull:assert(a==RAW&&b==FIELD&&d==77&&e==2);x0=229;break;
    case 0x7000062620ull:assert(a==TABLE&&b==229);x0=MESSAGE;x1=229;break;
    case 0x700023b150ull:
        assert(a==CONTEXT&&b==character&&!d&&!e&&f==PANEL&&!c->x[5]&&!c->x[6]);++baseline_calls;break;
    default:fprintf(stderr,"unexpected native mutation/helper %llx\n",(unsigned long long)fn);abort();
    }
    if(fn==invalidate_at) {
        invalidate_at=0;
        if(replace_source)memset((void*)NEW,0xa5,0x178);
        else if(replace_identity)gem.fingerprint[0]^=1;
        else ++owner_generation;
    }
    memset(c,0xcd,sizeof*c);c->x[0]=x0;c->x[1]=x1;
}
static void row(uint64_t p,unsigned i,uint16_t id,unsigned value) {
    uint64_t at=p+0x30+i*0x28;memset((void*)at,0,0x28);ST16(at,id);
    ST64(at+8,MESSAGE);ST16(at+0x10,2);ST32(at+0x18,value);ST32(at+0x1c,25);ST8(at+0x20,3);ST8(at+0x21,2);
}
static Cpu cpu(void) {Cpu c;memset(&c,0x5a,sizeof c);c.sp=STACK;c.x[0]=PANEL;c.x[1]=character;c.x[2]=OLD;c.x[3]=NEW;c.x[4]=0;return c;}
static void initialize(unsigned count) {
    owner=true;assigned=true;bonus_picker=false;character=1;++owner_generation;++epoch;
    invalidate_at=0;replace_source=false;replace_identity=false;invalidate_get=false;
    fail_ensure=false;invalidate_ensure=false;invalidate_layout=false;
    memset((void*)OLD,0,0x178);memset((void*)NEW,0,0x178);
    memset(&gem,0,sizeof gem);gem.uid=0x30002;gem.character=character;gem.rank=6;gem.value=22;memset(gem.fingerprint,0x71,32);
    for(unsigned i=0;i<count;i++){row(OLD,i,10+i,1+i);row(NEW,i,10+i,1+i);}
    ST64(OLD+0x170,count);ST64(NEW+0x170,count);
    ST64(SEQ+0x4c10,LIST);ST64(SEQ+0x8440,PANEL);ST32(SEQ+0x18b8,character);ST8(LIST+0x19,0);
    ST64(TABLE+8,CACHE_DATA);ST32(TABLE+0x10,2);g_settings.quickstep_slot=true;
    // The positive9throw case explicitly owns a synthetic20th native widget.
    // The selected original game resource has only19; its negativecase below
    // must not be confused with proof that a newvisualrow already exists.
    ST64(PANEL+0x1948,20);
}
static void begin(Cpu*c) {Cpu saved=*c;quickstep_stats_begin(c);assert(!memcmp(&saved,c,sizeof*c));}
static void frame(Cpu*c) {c->sp=STACK-0x2b0;c->x[19]=PANEL;c->x[21]=NEW;c->x[22]=OLD;}
static uint64_t count(Cpu*c) {Cpu expected=*c;c->x[8]=LD64(NEW+0x170);expected.x[8]=c->x[8];quickstep_stats_count(c);expected.x[8]=c->x[8];assert(!memcmp(&expected,c,sizeof*c));return c->x[8];}
static void end(Cpu*c) {Cpu before=*c;quickstep_stats_end(c);assert(!memcmp(&before,c,sizeof*c));}
static void compare(Cpu*c,uint32_t delta) {
    Cpu before=*c;quickstep_stats_compare(c);before.x[24]=delta;before.x[26]=0;assert(!memcmp(c,&before,sizeof*c));
}
int main(void) {
    assert(mmap((void*)BASE,0x500000,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0)==(void*)BASE);
    initialize(8);Cpu c=cpu();uint8_t old[0x178],new_[0x178];memcpy(old,(void*)OLD,sizeof old);memcpy(new_,(void*)NEW,sizeof new_);
    begin(&c);frame(&c);assert(count(&c)==9);
    for(unsigned i=0;i<9;i++) {
        c.x[20]=i;Cpu expected=c;assert(quickstep_stats_row(&c)==1);
        expected.x[8]=c.sp+0x150;expected.x[9]=LD64(c.sp+0x170);
        memcpy(&expected.v[0],(void*)(c.sp+0x150),16);memcpy(&expected.v[1],(void*)(c.sp+0x160),16);
        assert(!memcmp(&c,&expected,sizeof c));
        if(i<8)assert(!memcmp((void*)(c.sp+0x150),new_+0x30+i*0x28,0x28));
        else {assert(LD16(c.sp+0x150)==77&&LD32(c.sp+0x168)==22&&LD32(c.sp+0x16c)==25);
              assert(LD64(c.sp+0x158)==MESSAGE&&LD16(c.sp+0x160)==229&&LD8(c.sp+0x171)==6);compare(&c,0);}
    }
    assert(!memcmp(old,(void*)OLD,sizeof old)&&!memcmp(new_,(void*)NEW,sizeof new_));end(&c);
    assert(quickstep_stats_row(&c)==0);
    // Existing native Quick Step merges/caps in the copied row; baseline also
    // includes the same bonus, so this accurately retains neutral presentation.
    initialize(8);row(OLD,3,77,10);row(NEW,3,77,10);c=cpu();begin(&c);frame(&c);assert(count(&c)==8);c.x[20]=3;
    assert(quickstep_stats_row(&c)==1&&LD32(c.sp+0x168)==25);compare(&c,0);assert(LD32(NEW+0x30+3*0x28+0x18)==10);end(&c);
    // Different legitimate baseline/current values use the native capped delta.
    initialize(1);gem.value=4;row(OLD,0,77,10);row(NEW,0,77,20);c=cpu();begin(&c);frame(&c);c.x[20]=0;
    assert(quickstep_stats_row(&c)==1&&LD32(c.sp+0x168)==24);compare(&c,10);end(&c);
    // Empty summary obtains a separate native row without creating save/items.
    initialize(0);c=cpu();begin(&c);frame(&c);assert(count(&c)==1);c.x[20]=0;assert(quickstep_stats_row(&c)==1);compare(&c,0);end(&c);
    // Wrong panel/character, OFF, no association and ordinary inventory preview
    // leave the native source/count/copy behavior entirely unchanged.
    for(unsigned test=0;test<6;test++) {
        initialize(8);c=cpu();
        if(test==0)c.x[0]=PANEL+8;if(test==1)c.x[1]=8;if(test==2)g_settings.quickstep_slot=false;
        if(test==3)assigned=false;if(test==4){ST8(LIST+0x19,1);bonus_picker=false;}if(test==5)ST64(NEW+0x170,9);
        unsigned before=native_calls;begin(&c);frame(&c);assert(count(&c)==LD64(NEW+0x170)&&quickstep_stats_row(&c)==0&&native_calls==before);end(&c);
    }
    // Bonus picker uses a zero-selected baseline and is permitted to display.
    initialize(8);bonus_picker=true;ST8(LIST+0x19,1);c=cpu();begin(&c);frame(&c);assert(count(&c)==9);end(&c);
    initialize(8);ST64(PANEL+0x1948,19);fail_ensure=true;c=cpu();unsigned no_row_calls=native_calls;
    begin(&c);frame(&c);assert(count(&c)==8&&quickstep_stats_row(&c)==0&&native_calls==no_row_calls);end(&c);
    initialize(8);ST64(PANEL+0x1948,19);c=cpu();unsigned ensured=ensure_calls,positioned=layout_on;
    begin(&c);frame(&c);assert(count(&c)==9&&LD64(PANEL+0x1948)==20&&ensure_calls==ensured+1&&layout_on==positioned+1);end(&c);
    initialize(7);ST64(PANEL+0x1948,19);c=cpu();begin(&c);frame(&c);assert(count(&c)==8);end(&c);
    initialize(8);invalidate_get=true;c=cpu();unsigned prior=native_calls;begin(&c);frame(&c);
    assert(native_calls==prior&&count(&c)==8&&quickstep_stats_row(&c)==0);end(&c);
    initialize(8);invalidate_ensure=true;c=cpu();prior=native_calls;begin(&c);frame(&c);
    assert(native_calls==prior&&count(&c)==8&&quickstep_stats_row(&c)==0);end(&c);
    initialize(8);invalidate_layout=true;c=cpu();begin(&c);frame(&c);
    assert(count(&c)==8&&quickstep_stats_row(&c)==0);end(&c);
    // A yielded decoder/table/name helper cannot publish a replacement owner's
    // row, mutate its source, or leak bounded leases across repeated attempts.
    const uint64_t helpers[]={0x70000a8290,0x70000ae8e0,0x700006b270,0x7000062310,0x7000069320,0x7000510630,0x7000062620};
    for(unsigned j=0;j<80;j++) {
        initialize(8);invalidate_at=helpers[j%7];c=cpu();begin(&c);frame(&c);assert(count(&c)==8&&quickstep_stats_row(&c)==0);end(&c);
    }
    for(unsigned test=0;test<2;test++) {
        initialize(8);invalidate_at=0x7000062620;replace_source=test==0;replace_identity=test==1;c=cpu();begin(&c);frame(&c);assert(quickstep_stats_row(&c)==0);end(&c);
        if(test==0)for(unsigned i=0;i<0x178;i++)assert(LD8(NEW+i)==0xa5);
    }
    // If ownership/adoption/item/source changes after virtual count publication,
    // explicitly terminate extra rows rather than reading a ninth source record.
    for(unsigned test=0;test<5;test++) {
        initialize(8);c=cpu();begin(&c);frame(&c);assert(count(&c)==9);c.x[20]=8;
        if(test==0)++owner_generation;if(test==1)++epoch;if(test==2)gem.fingerprint[0]^=1;if(test==3)ST32(NEW,99);if(test==4)ST64(PANEL+0x1948,19);
        memset((void*)(c.sp+0x150),0xac,0x28);Cpu before=c;assert(quickstep_stats_row(&c)==-1&&!memcmp(&before,&c,sizeof c));
        for(unsigned i=0;i<0x28;i++)assert(LD8(c.sp+0x150+i)==0xac);end(&c);
    }
    // Native baseline refresh preserves full CPU and never requests an ordinary
    // equipment selection or socket replacement, for both Shulk/Fiora records.
    initialize(8);c=cpu();Cpu before=c;quickstep_stats_refresh(&c,SEQ);assert(baseline_calls==1&&!memcmp(&before,&c,sizeof c));
    character=8;ST32(SEQ+0x18b8,8);quickstep_stats_refresh(&c,SEQ);assert(baseline_calls==2&&!memcmp(&before,&c,sizeof c));
    ST8(LIST+0x19,1);unsigned preview_calls=native_calls;
    quickstep_stats_refresh(&c,SEQ);assert(native_calls==preview_calls&&baseline_calls==2);
    bonus_picker=true;quickstep_stats_refresh(&c,SEQ);assert(baseline_calls==3&&!memcmp(&before,&c,sizeof c));
    bonus_picker=false;ST8(LIST+0x19,0);
    owner=false;unsigned calls=native_calls;quickstep_stats_refresh(&c,SEQ);assert(native_calls==calls);
    assert(layout_off>layout_on&&ensure_calls>=layout_on);
    puts("quickstep native stats contracts: PASS");return 0;
}
