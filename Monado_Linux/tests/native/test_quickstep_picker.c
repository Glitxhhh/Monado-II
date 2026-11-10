// [XCDE] LINUX-040: authored native picker bridge fixtures; no game payload.
#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/mman.h>
#include "hle.h"
#include "settings.h"
#include "hle/quickstep_native_ui.h"
#include "hle/quickstep_picker.h"
#include "hle/quickstep_slot.h"
#define BASE 0x7200000000ull
#define SEQ (BASE+0x10000)
#define UI (BASE+0x20000)
#define LIST (BASE+0x30000)
#define DETAIL (BASE+0x40000)
#define TOOLTIP (BASE+0x50000)
#define CALLBACK (BASE+0x60000)
#define EVENT (BASE+0x61000)
#define ACTION (BASE+0x62000)
#define ROW_WIDGET (BASE+0x63000)
#define PANEL (BASE+0x64000)
#define DIALOG (CALLBACK-0x188)
#define ITEM(n) (BASE+0x100000+(n)*0x2c)
#define STACK (BASE+0x400000)
#define UID(n) (0x30000u|(n))
Settings g_settings;
int g_rt_unwind;
static bool focus=true, empty_visible, invalidate_list, invalidate_get, raw_owner=true;
static uint64_t generation=1, invalidate_at;
static uint64_t context=BASE+0x1000;
static unsigned closed, set_calls, list_calls, detail_calls, count_calls, stats_refreshes;
static unsigned sound_calls, close_calls, restore_calls;
static uint64_t reenter_at, replace_at;
static bool reject_set,cancel_after_set,migrate_tail,reset_during_close;
static uint32_t assigned, native_count, character=1;
static QuickstepGem gems[500];
static size_t gem_count;
static uint64_t native_rows[500];
static unsigned char staged[0x160], inventory[500*0x2c];
void rt_fatal_trace(void) { abort(); }
void rt_unwind_point(void) {}
bool quickstep_native_bonus_context(uint64_t seq,uint64_t *ctx,uint32_t *ch,uint64_t *gen) {
    if(seq!=SEQ||(!focus&&!quickstep_native_picker_context_menu_active(seq)))return false;
    *ctx=context;*ch=character;*gen=generation;return true;
}
bool quickstep_native_stats_context(uint64_t panel,uint32_t ch,uint64_t*seq,uint64_t*ctx,uint64_t*gen) {
    if(!raw_owner||!g_settings.quickstep_slot||panel!=PANEL||ch!=character)return false;
    *seq=SEQ;*ctx=context;*gen=generation;return true;
}
void quickstep_native_bonus_picker_closed(uint64_t seq) { assert(seq==SEQ);++closed; }
void quickstep_stats_refresh(Cpu*c,uint64_t seq) { assert(c&&seq==SEQ);++stats_refreshes; }
static void fp(unsigned n,uint8_t *out) {
    memcpy(out,(void*)(ITEM(n)+0xc),4);memcpy(out+4,(void*)(ITEM(n)+4),4);
    memcpy(out+8,(void*)(ITEM(n)+0x14),24);
}
size_t quickstep_slot_list(Cpu *c,uint64_t ctx,uint32_t ch,QuickstepGem*out,size_t cap) {
    assert(c&&ctx==context&&ch==character);++list_calls;
    if(out)memcpy(out,gems,(gem_count<cap?gem_count:cap)*sizeof*gems);
    if(invalidate_list){
        invalidate_list=false;++generation;
        // A different native owner reuses this same address while the helper
        // yields. Its entire vector/count must survive the old call untouched.
        memset((void*)(SEQ+0xe0),0xa5,500*8);ST64(SEQ+0x1080,0xfeedbeef);
    }
    return gem_count;
}
bool quickstep_slot_get(Cpu*c,uint64_t ctx,uint32_t ch,QuickstepGem*out) {
    assert(c&&ctx==context&&ch==character);
    for(size_t i=0;i<gem_count;i++)if(gems[i].uid==assigned){*out=gems[i];if(invalidate_get){invalidate_get=false;++generation;}return true;}
    return false;
}
bool quickstep_slot_set_checked(Cpu*c,uint64_t ctx,uint32_t ch,uint32_t uid,const uint8_t*identity) {
    assert(c&&ctx==context&&ch==character);++set_calls;
    if(reject_set)return false;
    if(!uid){assigned=0;return true;}
    for(size_t i=0;i<gem_count;i++)if(gems[i].uid==uid&&gems[i].availability==QUICKSTEP_FREE
        &&!memcmp(identity,gems[i].fingerprint,32)){
        uint8_t actual[32];fp(uid&0xffff,actual);
        if(memcmp(actual,identity,32))return false;
        assigned=uid;
        if(cancel_after_set){cancel_after_set=false;quickstep_native_picker_cancel(c,SEQ);}
        return true;
    }
    return false;
}
bool quickstep_slot_clear_checked(Cpu*c,uint64_t ctx,uint32_t ch,uint32_t uid,const uint8_t*identity) {
    assert(c&&ctx==context&&ch==character);++set_calls;
    for(size_t i=0;i<gem_count;i++)if(gems[i].uid==uid&&assigned==uid
        &&!memcmp(identity,gems[i].fingerprint,32)) {assigned=0;return true;}
    return false;
}
static void native_filter(Cpu*c) {
    Cpu saved=*c;c->x[0]=SEQ+0xe0;
    quickstep_native_picker_filter(c);*c=saved;
}
static void native_count_set(Cpu*c) {
    Cpu saved=*c;c->x[0]=LIST+0x58;c->x[1]=LD64(SEQ+0x1080);
    quickstep_native_picker_count(c);ST32(LIST+0x78,c->x[1]);*c=saved;
}
static void helper_reentry(Cpu*c,uint64_t function) {
    if(reenter_at==function) {
        reenter_at=0;
        unsigned sets=set_calls,sounds=sound_calls,closes=close_calls;
        Cpu saved=*c;c->x[0]=CALLBACK;
        assert(quickstep_native_picker_confirm(c)&&c->x[0]==1);
        quickstep_native_picker_cancel(c,SEQ);*c=saved;
        assert(set_calls==sets&&sound_calls==sounds&&close_calls==closes);
    }
    if(replace_at==function) {
        replace_at=0;
        quickstep_native_picker_forget(SEQ);++generation;
        ST32(SEQ+0x18c0,1);ST32(UI+0x18,1);ST32(UI+0x24,5);
        Cpu saved=*c;assert(quickstep_native_picker_open(c,SEQ));*c=saved;
    }
}
static void *worker_close_tail(void *argument) {
    quickstep_native_picker_closed(argument);return NULL;
}
void guest_call(Cpu*c,uint64_t function) {
    uint64_t a=c->x[0],b=c->x[1],d=c->x[2];
    switch(function) {
    case 0x70002fb9b0ull:
        assert(a==SEQ&&b==1&&LD32(UI+0x24)==13&&LD32(SEQ+0x18c0)==2);
        for(unsigned i=0;i<native_count;i++)ST64(SEQ+0xe0+i*8,native_rows[i]);
        ST64(SEQ+0x1080,native_count);native_filter(c);native_count_set(c);break;
    case 0x700020af90ull:assert(a==LIST);empty_visible=b;++count_calls;break;
    case 0x700020a510ull:assert(a==UI&&b==1);break;
    case 0x70002fec10ull:assert(a==SEQ);ST8(LIST+0x19,1);ST32(LIST+0x6c,0);break;
    case 0x7000240ef0ull:assert(a==LIST+0x58&&!d);ST32(LIST+0x6c,b);break;
    case 0x70002ff870ull:{assert(a==SEQ);Cpu saved=*c;assert(quickstep_native_picker_details(c));*c=saved;break;}
    case 0x70002fea30ull:assert(a==SEQ);break;
    case 0x70002fa290ull:assert(a==SEQ);ST64(SEQ+0x87b0,0x7000b4da30ull);break;
    case 0x7000300320ull:assert(a==SEQ);break;
    case 0x7000240cf0ull:assert(a==LIST+0x58);break;
    case 0x7000172c40ull:assert(b==context);memset((void*)a,0,0x140);ST64(a+8,b);break;
    case 0x7000172cf0ull:ST32(a,b);break;
    case 0x7000237a30ull:{
        assert(a==DETAIL);int32_t row=(int32_t)LD32(LIST+0x6c);
        uint32_t expect=row>=0&&(uint64_t)row<LD64(SEQ+0x1080)?LD32(LD64(SEQ+0xe0+(uint64_t)row*8)):0;
        if(expect){uint8_t actual[32];fp(expect&0xffff,actual);if(memcmp(actual,gems[expect&0xffff].fingerprint,32))expect=0;}
        assert(LD32(b)==expect);
        assert(LD32(d)==assigned);++detail_calls;break;}
    case 0x70002369c0ull:assert(a==DETAIL||a==TOOLTIP);++detail_calls;break;
    case 0x70001b75f0ull:{
        float volume;memcpy(&volume,&c->v[0],sizeof volume);
        assert(a==context&&b==0x14&&!d&&volume==1.f&&assigned);
        assert(quickstep_native_picker_active(SEQ)&&LD8(LIST+0x19));
        ++sound_calls;helper_reentry(c,function);break;
    }
    case 0x700020a610ull:
        assert(a==UI&&!b&&quickstep_native_picker_active(SEQ));
        ++restore_calls;helper_reentry(c,function);break;
    case 0x70002ff2e0ull:{
        assert(a==SEQ&&quickstep_native_picker_active(SEQ));++close_calls;
        ST8(LIST+0x19,0);helper_reentry(c,function);
        Cpu saved=*c;
        if(reset_during_close){
            reset_during_close=false;
            extern ResetFn __start_resets[],__stop_resets[];
            for(ResetFn*p=__start_resets;p<__stop_resets;++p)(*p)();
            ++generation;assert(quickstep_native_picker_open(c,SEQ));
            // Real reset stops the old guest thread; there is no pre-reset
            // native tail. Its old host-frame pop is exercised on return.
        }else if(migrate_tail){
            pthread_t worker;assert(!pthread_create(&worker,NULL,worker_close_tail,c));
            assert(!pthread_join(worker,NULL));
        }else quickstep_native_picker_closed(c);
        *c=saved;break;
    }
    default:fprintf(stderr,"unexpected native mutation/helper %llx\n",(unsigned long long)function);abort();
    }
    if(invalidate_at==function){invalidate_at=0;++generation;}
    memset(c,0xcd,sizeof*c);c->x[0]=0;
}
static Cpu cpu(void){Cpu c;memset(&c,0x57,sizeof c);c.sp=STACK;return c;}
static void make_gem(unsigned n,unsigned rank,unsigned availability) {
    memset((void*)ITEM(n),0,0x2c);ST32(ITEM(n),UID(n));ST16(ITEM(n)+4,100+n);
    ST16(ITEM(n)+6,3);ST32(ITEM(n)+0xc,1000+n);ST8(ITEM(n)+0x10,1);
    ST8(ITEM(n)+0x16,rank);ST8(ITEM(n)+0x1a,1);ST16(ITEM(n)+0x1c,77);ST8(ITEM(n)+0x1e,20);
    QuickstepGem*g=&gems[gem_count++];memset(g,0,sizeof*g);g->uid=UID(n);g->rank=rank;
    g->availability=availability;fp(n,g->fingerprint);
    native_rows[native_count++]=ITEM(n);
}
static void reset(void) {
    quickstep_native_picker_forget(SEQ);focus=true;generation++;character=1;context=BASE+0x1000;
    invalidate_get=false;raw_owner=true;reject_set=cancel_after_set=migrate_tail=reset_during_close=false;
    invalidate_at=0;reenter_at=replace_at=0;
    gem_count=0;native_count=0;assigned=0;set_calls=0;empty_visible=false;
    memset((void*)SEQ,0,0x8a20);memset((void*)UI,0,0x70);memset((void*)LIST,0,0xa00);
    ST64(SEQ+0x1c48,UI);ST64(SEQ+0x4c10,LIST);ST64(SEQ+0x8448,DETAIL);
    ST64(SEQ+0x5da0,TOOLTIP);ST64(SEQ+0x5e20,TOOLTIP);ST32(SEQ+0x18b8,1);
    ST64(SEQ+0x8440,PANEL);
    ST32(SEQ+0x18c0,2);ST32(UI+0x18,2);ST32(UI+0x24,12);ST64(CALLBACK+0x18,SEQ);
    ST64(CALLBACK+8,SEQ);ST32(EVENT+8,2);ST32(EVENT+0xc,13);
    memset((void*)(SEQ+0x8450),0x8b,sizeof staged);memcpy(staged,(void*)(SEQ+0x8450),sizeof staged);
    g_settings.quickstep_slot=true;
}
static void unchanged(void){assert(!memcmp(staged,(void*)(SEQ+0x8450),sizeof staged));}
static void style(Cpu*c,uint64_t expected){
    Cpu before=*c;quickstep_native_picker_row_style(c);before.x[1]=expected;
    assert(!memcmp(c,&before,sizeof*c));
}
static void menu(Cpu*c,uint8_t expected){
    Cpu before=*c;uint8_t frame[0x2d8];memcpy(frame,(void*)c->sp,sizeof frame);
    quickstep_native_picker_menu_item(c);frame[0x2d4]=expected;
    assert(!memcmp(c,&before,sizeof*c)&&!memcmp(frame,(void*)c->sp,sizeof frame));
}
static void native_row_frame(Cpu*c){
    *c=cpu();c->x[20]=SEQ;c->x[0]=ROW_WIDGET;c->x[1]=1;
    memset((void*)c->sp,0x93,0x2d8);ST64(c->sp+8,context);ST32(c->sp+0x28,3);ST64(c->sp+0x138,ITEM(0));
}
static void prepare_menu(Cpu*c){
    c->x[19]=SEQ;ST32(c->sp+0x2d0,0);ST8(c->sp+0x2d4,0);
    quickstep_native_picker_menu_item(c);
    ST64(DIALOG,0x7000b4d418ull);ST64(DIALOG+0x180,CALLBACK);
    ST64(CALLBACK,0x7000b4d888ull);ST64(CALLBACK+8,SEQ);
    ST64(SEQ+0x87a8,SEQ+0x87b0);ST64(SEQ+0x87b0,0x7000b4da00ull);
    c->x[21]=DIALOG;
}
static void publish_menu(Cpu*c){
    Cpu saved=*c;prepare_menu(c);quickstep_native_picker_menu_opened(c);
    assert(quickstep_native_picker_context_menu_active(SEQ));*c=saved;
}
int main(void) {
    assert(mmap((void*)BASE,0x500000,PROT_READ|PROT_WRITE,
        MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0)==(void*)BASE);
    assert(mmap((void*)(GUEST_HI-0x1000),0x1000,PROT_READ|PROT_WRITE,
        MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0)==(void*)(GUEST_HI-0x1000));
    Cpu c=cpu(),before=c;reset();
    assert(quickstep_native_picker_open(&c,SEQ));assert(empty_visible&&LD32(LIST+0x78)==0);
    assert((int32_t)LD32(LIST+0x6c)==-1&&!memcmp(&c,&before,sizeof c));
    quickstep_native_picker_cancel(&c,SEQ);assert(!quickstep_native_picker_active(SEQ));
    assert(LD32(UI+0x24)==12&&!memcmp(&c,&before,sizeof c));unchanged();
    // Callback bounds live in the runtime, not in generated cpu.h-only code.
    // Null and a truncated high callback cannot close or read an invalid owner.
    reset();assert(quickstep_native_picker_open(&c,SEQ));
    c.x[0]=0;before=c;unsigned closed_before=closed;
    quickstep_native_picker_cancel_callback(&c);
    assert(quickstep_native_picker_active(SEQ)&&closed==closed_before&&!memcmp(&c,&before,sizeof c));
    c.x[0]=GUEST_HI-0x1f;before=c;
    quickstep_native_picker_cancel_callback(&c);
    assert(quickstep_native_picker_active(SEQ)&&closed==closed_before&&!memcmp(&c,&before,sizeof c));
    ST64(GUEST_HI-8,SEQ);c.x[0]=GUEST_HI-0x20;before=c;
    quickstep_native_picker_cancel_callback(&c);
    assert(!quickstep_native_picker_active(SEQ)&&closed==closed_before+1&&!memcmp(&c,&before,sizeof c));
    assert(LD32(UI+0x24)==12);unchanged();c=cpu();before=c;
    reset();for(unsigned n=0;n<500;n++)make_gem(n,1+n%6,QUICKSTEP_FREE);
    // Real crafted gems carry no static definition; native payload + serial
    // remain the exact pooled identity shown by the regular native list.
    ST16(ITEM(0)+4,0);fp(0,gems[0].fingerprint);
    // Favourites remain eligible; real ordinary and other-character assignments do not.
    ST8(ITEM(0)+0x11,1);gems[1].availability=QUICKSTEP_ORDINARY_EQUIPPED;
    gems[2].availability=QUICKSTEP_ASSIGNED_ELSEWHERE;assigned=UID(3);
    memcpy(inventory,(void*)ITEM(0),sizeof inventory);
    assert(quickstep_native_picker_open(&c,SEQ));assert(LD64(SEQ+0x1080)==498);
    assert(!empty_visible&&LD32(LIST+0x78)==498&&LD32(LIST+0x6c)==1);
    assert(LD64(SEQ+0xe0)==ITEM(0)&&LD64(SEQ+0xe8)==ITEM(3));
    assert(!memcmp(&c,&before,sizeof c));unchanged();
    // Native201 must reach the bonus setter before ordinary boot/socket gates.
    // This fixture deliberately has no valid parent boot or ordinary socket;
    // any attempt to call a native socket/transfer helper aborts in the spy.
    ST32(LIST+0x6c,0);before=c;unsigned accepted_before=set_calls;
    unsigned stats_before=stats_refreshes;
    assert(quickstep_native_picker_input_event(&c,SEQ,0x201));
    assert(assigned==UID(0)&&set_calls==accepted_before+1&&!memcmp(&c,&before,sizeof c));
    assert(stats_refreshes==stats_before+1);
    assert(!quickstep_native_picker_active(SEQ)&&!LD8(LIST+0x19)&&LD32(UI+0x24)==12);
    unsigned sounds_before=sound_calls,closes_before=close_calls;
    c.x[0]=CALLBACK;assert(quickstep_native_picker_confirm(&c)&&c.x[0]==1);
    assert(sound_calls==sounds_before&&close_calls==closes_before&&set_calls==accepted_before+1);c=before;
    assert(!memcmp(inventory,(void*)ITEM(0),sizeof inventory));unchanged();
    uint32_t native_other[]={0x202,0x207,0x208,0x210,0x215};
    for(unsigned i=0;i<sizeof native_other/sizeof native_other[0];i++)
        assert(!quickstep_native_picker_input_event(&c,SEQ,native_other[i]));
    assert(!quickstep_native_picker_input_event(&c,SEQ+8,0x201));
    assert(set_calls==accepted_before+1&&!memcmp(&c,&before,sizeof c));
    // Change native category/sort enumeration independently: same restriction applies.
    assert(quickstep_native_picker_open(&c,SEQ));
    ST64(SEQ+0xe0,ITEM(2));ST64(SEQ+0xe8,ITEM(0));ST64(SEQ+0x1080,2);
    native_filter(&c);native_count_set(&c);assert(LD64(SEQ+0x1080)==1&&LD64(SEQ+0xe0)==ITEM(0));
    ST32(LIST+0x6c,0);c.x[0]=CALLBACK;before=c;
    assert(quickstep_native_picker_confirm(&c)&&c.x[0]==1&&assigned==UID(0));before.x[0]=1;
    assert(!memcmp(&c,&before,sizeof c));unchanged();
    assert(!memcmp(inventory,(void*)ITEM(0),sizeof inventory));
    // Reused instance identity cannot accept a selection from the old list.
    assert(!quickstep_native_picker_active(SEQ)&&quickstep_native_picker_open(&c,SEQ));
    ST32(ITEM(0)+0xc,99999);unsigned prior=set_calls;c.x[0]=CALLBACK;
    assert(quickstep_native_picker_confirm(&c)&&set_calls==prior&&assigned==UID(0));
    before=c;assert(quickstep_native_picker_input_event(&c,SEQ,0x201));
    assert(set_calls==prior&&assigned==UID(0)&&!memcmp(&c,&before,sizeof c));
    c.x[0]=SEQ;assert(quickstep_native_picker_commit(&c)&&quickstep_native_picker_preview(&c));
    c.x[0]=CALLBACK;c.x[1]=EVENT;stats_before=stats_refreshes;
    assert(quickstep_native_picker_remove(&c)&&!assigned&&stats_refreshes==stats_before+1);unchanged();
    // Native Plus bulk/swap actions close normally without any ordinary mutation.
    assigned=UID(3);c.x[0]=SEQ;c.x[1]=1;c.x[2]=13;
    assert(quickstep_native_picker_eligible(&c)&&c.x[0]==1);
    for(unsigned action=1;action<=6;action++) {
        publish_menu(&c);
        ST32(ACTION+0x20,action);c.x[0]=CALLBACK;c.x[1]=0;c.x[2]=ACTION;before=c;
        assert(quickstep_native_picker_submenu(&c)&&assigned==UID(3));
        assert(!memcmp(&c,&before,sizeof c));unchanged();
    }
    publish_menu(&c);ST32(ACTION+0x20,0);c.x[0]=CALLBACK;assert(quickstep_native_picker_submenu(&c)&&!assigned);unchanged();
    c=cpu();quickstep_native_picker_cancel(&c,SEQ);assert(LD32(UI+0x24)==12);unchanged();
    // A successful checked association closes exactly once with the genuine
    // equip sound. Nested Confirm/Cancel during each native helper cannot
    // assign again, play again or start a second close.
    const uint64_t feedback_helpers[]={0x70001b75f0ull,0x700020a610ull,0x70002ff2e0ull};
    for(unsigned point=0;point<3;++point){
        reset();make_gem(0,point+1,QUICKSTEP_FREE);ST32(SEQ+0x18c0,1);ST32(UI+0x18,1);ST32(UI+0x24,5);
        c=cpu();assert(quickstep_native_picker_open(&c,SEQ));before=c;
        unsigned sounds=sound_calls,closes=close_calls,restores=restore_calls,notifications=closed;
        uint64_t fpcr=rt_get_fpcr(),fpsr=rt_get_fpsr();reenter_at=feedback_helpers[point];migrate_tail=point==2;
        assert(quickstep_native_picker_input_event(&c,SEQ,0x201));
        assert(!memcmp(&c,&before,sizeof c)&&rt_get_fpcr()==fpcr&&rt_get_fpsr()==fpsr);
        assert(assigned==UID(0)&&set_calls==1&&sound_calls==sounds+1&&close_calls==closes+1);
        assert(restore_calls==restores+1&&closed==notifications+1&&!quickstep_native_picker_active(SEQ));
        assert(!LD8(LIST+0x19)&&LD32(SEQ+0x18c0)==1&&LD32(UI+0x18)==1&&LD32(UI+0x24)==5);
        c.x[0]=CALLBACK;assert(quickstep_native_picker_confirm(&c)&&c.x[0]==1);
        quickstep_native_picker_cancel(&c,SEQ);
        assert(set_calls==1&&sound_calls==sounds+1&&close_calls==closes+1);unchanged();
    }
    // Rejected/missing/reused selections and post-set identity invalidation
    // remain silent. No success close may write a stale owner's pane/index.
    for(unsigned bad=0;bad<4;++bad){
        reset();make_gem(0,6,QUICKSTEP_FREE);c=cpu();assert(quickstep_native_picker_open(&c,SEQ));
        if(bad==0)reject_set=true;
        if(bad==1)ST32(LIST+0x6c,500);
        if(bad==2)ST32(ITEM(0)+0xc,0xfeed);
        if(bad==3)invalidate_get=true;
        unsigned sounds=sound_calls,closes=close_calls,notifications=closed;before=c;
        assert(quickstep_native_picker_input_event(&c,SEQ,0x201));
        assert(!memcmp(&c,&before,sizeof c)&&sound_calls==sounds&&close_calls==closes&&closed==notifications);
        assert(quickstep_native_picker_active(SEQ)&&LD8(LIST+0x19)&&LD32(UI+0x24)==13);unchanged();
        quickstep_native_picker_forget(SEQ);
    }
    reset();make_gem(0,2,QUICKSTEP_FREE);c=cpu();assert(quickstep_native_picker_open(&c,SEQ));
    cancel_after_set=true;unsigned sounds=sound_calls,closes=close_calls;before=c;
    assert(quickstep_native_picker_input_event(&c,SEQ,0x201)&&!memcmp(&c,&before,sizeof c));
    assert(set_calls==1&&sound_calls==sounds&&close_calls==closes+1&&!quickstep_native_picker_active(SEQ));unchanged();
    // Owner invalidation after sound or UI restore stops the next guest call.
    for(unsigned point=0;point<2;++point){
        reset();make_gem(0,4,QUICKSTEP_FREE);c=cpu();assert(quickstep_native_picker_open(&c,SEQ));
        sounds=sound_calls;closes=close_calls;unsigned restores=restore_calls,notifications=closed;
        invalidate_at=feedback_helpers[point];before=c;
        assert(quickstep_native_picker_input_event(&c,SEQ,0x201)&&!memcmp(&c,&before,sizeof c));
        assert(sound_calls==sounds+1&&restore_calls==restores+point&&close_calls==closes&&closed==notifications);
        assert(!quickstep_native_picker_active(SEQ)&&LD32(UI+0x24)==13);unchanged();
    }
    // An old native-close tail/fallback must not clear a newly opened picker
    // reusing the same sequence address while the helper yields.
    for(unsigned point=0;point<3;++point){
        reset();make_gem(0,5,QUICKSTEP_FREE);c=cpu();assert(quickstep_native_picker_open(&c,SEQ));
        replace_at=feedback_helpers[point];migrate_tail=point==2;before=c;unsigned notifications=closed;
        assert(quickstep_native_picker_input_event(&c,SEQ,0x201)&&!memcmp(&c,&before,sizeof c));
        assert(quickstep_native_picker_active(SEQ)&&closed==notifications&&LD8(LIST+0x19));
        assert(LD32(UI+0x24)==13&&LD32(SEQ+0x18c0)==2);unchanged();
        quickstep_native_picker_cancel(&c,SEQ);assert(LD32(UI+0x24)==5&&LD32(SEQ+0x18c0)==1);
    }
    // Reset invalidates the old registry epoch. The returning old host frame
    // must not republish its former head or mask a new session's native tail.
    reset();make_gem(0,2,QUICKSTEP_FREE);c=cpu();assert(quickstep_native_picker_open(&c,SEQ));
    reset_during_close=true;before=c;
    assert(quickstep_native_picker_input_event(&c,SEQ,0x201)&&!memcmp(&c,&before,sizeof c));
    assert(quickstep_native_picker_active(SEQ));
    c.x[0]=SEQ;unsigned notifications=closed;quickstep_native_picker_closed(&c);
    assert(!quickstep_native_picker_active(SEQ)&&closed==notifications+1);unchanged();
    // Ordinary equipment callbacks pass through when bonus focus is absent.
    focus=false;c.x[0]=CALLBACK;assert(!quickstep_native_picker_confirm(&c));
    assert(!quickstep_native_picker_remove(&c));c.x[0]=SEQ;assert(!quickstep_native_picker_commit(&c));
    // A helper yield invalidating owner generation cannot publish old rows.
    reset();make_gem(0,6,QUICKSTEP_FREE);invalidate_list=true;c=cpu();
    assert(!quickstep_native_picker_open(&c,SEQ)&&!quickstep_native_picker_active(SEQ));
    assert(LD64(SEQ+0x1080)==0xfeedbeef);
    for(unsigned i=0;i<500*8;i++)assert(LD8(SEQ+0xe0+i)==0xa5);
    unchanged();
    // Setting disabled is transparent and does not query inventory/native UI.
    reset();g_settings.quickstep_slot=false;unsigned calls=list_calls;c=cpu();
    assert(!quickstep_native_picker_open(&c,SEQ)&&list_calls==calls);unchanged();
    // Every supported main/FC character has the same bonus-row availability,
    // independent of whether the ordinary parent is absent, has zero sockets,
    // is intrinsic/unique, or has an empty/filled normal gem. Native201 and the
    // checked Remove mutation remain the only assignment actions.
    const uint32_t characters[]={1,2,3,4,5,6,7,8,1,7,14,15};
    for(unsigned actor=0;actor<sizeof characters/sizeof characters[0];++actor)
        for(unsigned parent=0;parent<5;++parent){
            reset();character=characters[actor];if(actor>=8)context=BASE+0x2000;ST32(SEQ+0x18b8,character);
            ST32(SEQ+0x8450+5*4,parent==0?0:100+parent);
            ST32(SEQ+0x84f0+5*4,parent==0?0:0x50000+parent);
            ST32(SEQ+0x8590+5*4,parent<3?0:1);
            ST32(SEQ+0x8450+13*4,parent==4?101:0);
            ST32(SEQ+0x84f0+13*4,parent==4?0x30099:0);
            memcpy(staged,(void*)(SEQ+0x8450),sizeof staged);
            make_gem(0,1+actor%6,QUICKSTEP_FREE);ST8(ITEM(0)+0x11,1);
            memcpy(inventory,(void*)ITEM(0),sizeof inventory);
            c=cpu();assert(quickstep_native_picker_open(&c,SEQ));
            native_row_frame(&c);style(&c,0);assert(!set_calls);unchanged();
            c=cpu();c.x[19]=SEQ;c.x[20]=LIST;c.x[0]=1;before=c;
            quickstep_native_picker_menu_gate(&c);before.x[0]=0;
            assert(!memcmp(&c,&before,sizeof c));unchanged();
            // A separate row using the same native widget stays untouched.
            native_row_frame(&c);c.x[20]=SEQ+8;style(&c,1);
            native_row_frame(&c);ST64(c.sp+0x138,ITEM(1));style(&c,1);
            native_row_frame(&c);ST64(c.sp+8,context+8);style(&c,1);
            native_row_frame(&c);ST32(c.sp+0x28,4);style(&c,1);
            native_row_frame(&c);assert(quickstep_native_picker_input_event(&c,SEQ,0x201));
            assert(assigned==UID(0)&&set_calls==1);unchanged();
            quickstep_native_picker_cancel(&c,SEQ);
            // The original boots row may still be selected5 on no-socket or
            // unique gear. Bonus-focus Remove uses its association, not13's
            // ordinary staged fields, with all bulk/swap actions disabled.
            for(unsigned pane=1;pane<=2;++pane)
                for(unsigned selected=5;selected<=13;selected+=8){
                    ST32(SEQ+0x18c0,pane);ST32(UI+0x24,selected);c=cpu();c.x[19]=SEQ;
                    for(unsigned action=0;action<=6;++action){
                        ST32(c.sp+0x2d0,action);ST8(c.sp+0x2d4,action?1:0);
                        menu(&c,action?0:1);
                    }
                }
            publish_menu(&c);focus=false; // Popup hides the slot; the typed lease keeps only its logical owner.
            ST32(ACTION+0x20,0);c.x[0]=CALLBACK;c.x[1]=0;c.x[2]=ACTION;
            assert(quickstep_native_picker_submenu(&c)&&!assigned&&set_calls==2);
            assert(!quickstep_native_picker_context_menu_active(SEQ));focus=true;
            c=cpu();c.x[19]=SEQ;ST32(c.sp+0x2d0,0);ST8(c.sp+0x2d4,1);menu(&c,0);
            assert(!memcmp(inventory,(void*)ITEM(0),sizeof inventory));unchanged();
        }
    // Ineligible/reused instances cannot be ungreyed by a stale filtered row.
    reset();make_gem(0,6,QUICKSTEP_FREE);c=cpu();assert(quickstep_native_picker_open(&c,SEQ));
    gems[0].availability=QUICKSTEP_ORDINARY_EQUIPPED;native_row_frame(&c);style(&c,1);
    gems[0].availability=QUICKSTEP_ASSIGNED_ELSEWHERE;native_row_frame(&c);style(&c,1);
    gems[0].availability=QUICKSTEP_FREE;ST32(ITEM(0)+0xc,0xfeed);native_row_frame(&c);style(&c,1);
    fp(0,gems[0].fingerprint);native_row_frame(&c);style(&c,0);
    // A yielding eligibility lookup can replace the complete list owner. Its
    // style argument and all CPU/FP state remain unchanged after invalidation.
    native_row_frame(&c);invalidate_list=true;style(&c,1);
    assert(LD64(SEQ+0x1080)==0xfeedbeef);unchanged();
    reset();make_gem(0,3,QUICKSTEP_FREE);assigned=UID(0);ST32(UI+0x24,13);
    c=cpu();c.x[19]=SEQ;ST32(c.sp+0x2d0,0);ST8(c.sp+0x2d4,0);invalidate_get=true;menu(&c,0);
    // OFF, foreign selection/pane/action/context, invalid native frame and
    // absent focus all leave ordinary native menu records and CPU untouched.
    for(unsigned bad=0;bad<6;++bad){
        reset();make_gem(0,3,QUICKSTEP_FREE);assigned=UID(0);ST32(UI+0x24,13);
        c=cpu();c.x[19]=SEQ;ST32(c.sp+0x2d0,0);ST8(c.sp+0x2d4,1);
        if(bad==0)g_settings.quickstep_slot=false;
        if(bad==1)ST32(UI+0x24,12);
        if(bad==2)ST32(SEQ+0x18c0,3);
        if(bad==3)ST32(c.sp+0x2d0,7);
        if(bad==4)c.x[19]=SEQ+8;
        if(bad==5)focus=false;
        menu(&c,1);unchanged();
    }
    c=cpu();c.sp=GUEST_HI-0x2d4;c.x[19]=SEQ;before=c;
    quickstep_native_picker_menu_item(&c);assert(!memcmp(&c,&before,sizeof c));
    c=cpu();c.sp=GUEST_HI-0x13f;c.x[20]=SEQ;c.x[0]=ROW_WIDGET;before=c;
    quickstep_native_picker_row_style(&c);assert(!memcmp(&c,&before,sizeof c));
    // Empty or wrong native lists keep the genuine Plus gate. It is a single
    // parent/socket bypass, not a global change to native2FA520 or modal input.
    reset();c=cpu();assert(quickstep_native_picker_open(&c,SEQ));
    c.x[19]=SEQ;c.x[20]=LIST;c.x[0]=1;before=c;quickstep_native_picker_menu_gate(&c);assert(!memcmp(&c,&before,sizeof c));
    reset();make_gem(0,3,QUICKSTEP_FREE);c=cpu();assert(quickstep_native_picker_open(&c,SEQ));
    c.x[19]=SEQ;c.x[20]=LIST+8;c.x[0]=1;before=c;quickstep_native_picker_menu_gate(&c);assert(!memcmp(&c,&before,sizeof c));
    c.x[20]=LIST;invalidate_list=true;before=c;quickstep_native_picker_menu_gate(&c);assert(!memcmp(&c,&before,sizeof c));
    // Cancel/Remove close the exact typed popup lease even when its native
    // visual is hidden. Closing restores the native handler before checked
    // removal, so the temporary closing lease must span that helper call.
    reset();make_gem(0,3,QUICKSTEP_FREE);assigned=UID(0);ST32(UI+0x24,5);ST32(SEQ+0x18c0,1);c=cpu();publish_menu(&c);
    focus=false;c.x[0]=CALLBACK;c.x[1]=UINT32_MAX;c.x[2]=0;before=c;
    assert(quickstep_native_picker_submenu(&c)&&assigned==UID(0)&&!quickstep_native_picker_context_menu_active(SEQ));
    assert(!memcmp(&c,&before,sizeof c));unchanged();
    // Late old callbacks stay handled without detaching ordinary gear. A new
    // ordinary completed menu (same allocator address) clears that provenance.
    c.x[1]=0;c.x[2]=ACTION;ST32(ACTION+0x20,0);assert(quickstep_native_picker_submenu(&c)&&assigned==UID(0));
    assert(!quickstep_native_picker_input_event(&c,SEQ,0x20d));c.x[19]=SEQ;c.x[21]=DIALOG;before=c;
    quickstep_native_picker_menu_opened(&c);assert(!memcmp(&c,&before,sizeof c));
    c.x[0]=CALLBACK;assert(!quickstep_native_picker_submenu(&c));unchanged();
    // A failed builder never exposes an active lease, and its later ordinary
    // native20D attempt cannot inherit pending provenance or a sticky focus.
    reset();make_gem(0,3,QUICKSTEP_FREE);assigned=UID(0);ST32(UI+0x24,13);c=cpu();prepare_menu(&c);
    assert(!quickstep_native_picker_context_menu_active(SEQ));focus=false;
    assert(!quickstep_native_picker_input_event(&c,SEQ,0x20d));quickstep_native_picker_menu_opened(&c);
    c.x[0]=CALLBACK;c.x[1]=0;c.x[2]=ACTION;assert(!quickstep_native_picker_submenu(&c));unchanged();
    // Completed publication rejects forged dialog/callback/handler identities,
    // stale owner/character epochs and OFF. Its exact callback still cannot
    // fall through to native ordinary equipment removal after that rejection.
    for(unsigned bad=0;bad<7;++bad){
        reset();make_gem(0,3,QUICKSTEP_FREE);assigned=UID(0);ST32(UI+0x24,13);c=cpu();prepare_menu(&c);
        if(bad==0)ST64(DIALOG,0);
        if(bad==1)ST64(DIALOG+0x180,CALLBACK+0x40);
        if(bad==2)ST64(CALLBACK,0);
        if(bad==3)ST64(SEQ+0x87b0,0x7000b4da30ull);
        if(bad==4)++generation;
        if(bad==5)raw_owner=false;
        if(bad==6)g_settings.quickstep_slot=false;
        before=c;quickstep_native_picker_menu_opened(&c);assert(!memcmp(&c,&before,sizeof c));
        assert(!quickstep_native_picker_context_menu_active(SEQ));focus=false;
        c.x[0]=CALLBACK;c.x[1]=0;c.x[2]=ACTION;ST32(ACTION+0x20,0);before=c;
        assert(quickstep_native_picker_submenu(&c)&&assigned==UID(0)&&!set_calls&&!memcmp(&c,&before,sizeof c));unchanged();
    }
    // A second builder/open notification cannot replace a still valid popup.
    reset();make_gem(0,3,QUICKSTEP_FREE);assigned=UID(0);ST32(UI+0x24,13);c=cpu();publish_menu(&c);
    c.sp-=0x1000;c.x[19]=SEQ;ST32(c.sp+0x2d0,0);quickstep_native_picker_menu_item(&c);
    c.x[21]=DIALOG+0x1000;before=c;quickstep_native_picker_menu_opened(&c);
    assert(!memcmp(&c,&before,sizeof c)&&quickstep_native_picker_context_menu_active(SEQ));
    // Owner changes, explicit forget and process reset end the public lease.
    // Exact old callbacks remain safe across forget; reset runs only once all
    // guest threads stopped and guest storage is about to be wiped.
    for(unsigned stale=0;stale<4;++stale){
        reset();make_gem(0,3,QUICKSTEP_FREE);assigned=UID(0);ST32(UI+0x24,13);c=cpu();publish_menu(&c);focus=false;
        if(stale==0)++generation;
        if(stale==1)ST32(SEQ+0x18b8,2);
        if(stale==2)quickstep_native_picker_forget(SEQ);
        if(stale==3)g_settings.quickstep_slot=false;
        assert(!quickstep_native_picker_context_menu_active(SEQ));
        c.x[0]=CALLBACK;c.x[1]=0;c.x[2]=ACTION;assert(quickstep_native_picker_submenu(&c)&&assigned==UID(0));unchanged();
    }
    extern ResetFn __start_resets[],__stop_resets[];
    for(ResetFn*p=__start_resets;p<__stop_resets;++p)(*p)();
    assert(!quickstep_native_picker_context_menu_active(SEQ));c.x[0]=CALLBACK;assert(!quickstep_native_picker_submenu(&c));
    assert(detail_calls&&count_calls&&closed);
    puts("native picker ownership, filtering and state contracts: PASS");
}
