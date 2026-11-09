// [XCDE] LINUX-035: authored native-job fixtures; no game files or generated code.
#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <string.h>
#include "hle/expanded_save_scan.h"
#include "hle.h"
#define BASE 0x7200000000ull
#define SEQ (BASE+0x1000)
#define CHECK (BASE+0x900000)
#define STORY (BASE+0xd00000)
#define HOLDER (BASE+0xe00000)
#define CONTEXT (BASE+0xe10000)
#define BUFFER (BASE+0x1000000)
#define BYTES 0x153f30
static uint64_t stamps[2][99];
static bool present[2][99], cleared[2][99];
static unsigned queued, copied, decoded, base_calls, remembered_story, remembered_id;
static bool remembered_auto;
static struct { uint64_t task; unsigned story, id, polls; bool inflight; } job;
static uint64_t queue_history[512], reset_during_call;
int g_rt_unwind;
void rt_fatal_trace(void) { abort(); }
void rt_unwind_point(void) {}
void xcde_expanded_saves_remember(uint32_t story,uint16_t id,bool autosave) {
    remembered_story=story; remembered_id=id; remembered_auto=autosave;
}
void guest_call(Cpu *c, uint64_t fn) {
    uint64_t a=c->x[0],b=c->x[1],d=c->x[2],ret=0;
    switch(fn) {
    case 0x7000592a30ull:
        assert(a==SEQ+0xa0+8 || a==CHECK+0x10+8); ret=STORY; break;
    case 0x7000087040ull:
        assert(!job.inflight);assert(b>=3&&b<=98); assert(!d&&!c->x[3]);
        job.task=a;job.story=(LD64(STORY+0x150)>>3)&1;job.id=b;job.polls=2;job.inflight=true;
        queue_history[queued++]=(job.story<<16)|job.id;
        assert(queued<512);
        memset((void*)BUFFER,0,BYTES); ST64(BUFFER+8,stamps[job.story][b]);
        ST64(BUFFER+16,b);ST8(BUFFER+24,cleared[job.story][b]);break;
    case 0x70000863f0ull:
        assert(a==job.task);
        if(job.polls) { --job.polls;ret=1; } else job.inflight=false;
        break;
    case 0x7000086400ull: assert(!job.inflight);ret=present[job.story][job.id];break;
    case 0x7000086e70ull: assert(!job.inflight);ret=BUFFER;break;
    case 0x700039a100ull:
        assert(b==BUFFER);assert(a>=SEQ+0x154140&&a<SEQ+0x6a3e00);++copied;
        ST64(a+8,LD64(b+8));ST64(a+16,LD64(b+16));break;
    case 0x70003c2880ull:
        assert(a==CHECK&&!job.inflight);++base_calls;
        ST32(CHECK+0x304030,-1);ST32(CHECK+0x304038,LD32(CHECK+0x304038)|3);break;
    case 0x7000073f00ull:
        assert(a==CHECK+0x1540a0&&b==LD64(CHECK)&&d==BUFFER);++decoded;ST8(a,LD8(d+24));break;
    case 0x70000ccd80ull:assert(a==CHECK+0x1540a0);ret=LD8(a);break;
    default: fprintf(stderr,"unexpected native call %llx\n",(unsigned long long)fn);abort();
    }
    if (fn == reset_during_call) {
        reset_during_call=0;xcde_expanded_scan_presence_reset(CHECK);ST32(CHECK+0x304038,1);
    }
    // A helper is allowed to clobber every guest register/FP lane. Adapter must
    // restore the outer guest CPU exactly, including nested helper stack.
    memset(c,0xcc,sizeof *c);c->x[0]=ret;
}
static Cpu cpu(void) {
    Cpu c;memset(&c,0xa6,sizeof c);c.sp=BASE+0x1400000;c.x[19]=SEQ;c.x[0]=CHECK;return c;
}
static void clear_fixture(void) {
    xcde_expanded_scan_continue_drop(SEQ);xcde_expanded_scan_presence_drop(CHECK);
    memset((void*)SEQ,0,0x6a3e04);memset((void*)CHECK,0,0x304050);
    memset(stamps,0,sizeof stamps);memset(present,0,sizeof present);memset(cleared,0,sizeof cleared);
    memset(&job,0,sizeof job);queued=copied=decoded=base_calls=0;reset_during_call=0;
    remembered_story=remembered_id=999;remembered_auto=true;
    ST64(SEQ+0x10,HOLDER);ST64(HOLDER+0x28,CONTEXT);ST32(CHECK+0x304030,-1);ST64(CHECK,0x1234);
    ST64(STORY+0x150,0x123456789abcdef0ull);
}
static unsigned continue_until_complete(Cpu *c) {
    Cpu before=*c;unsigned steps=0;uint64_t story=LD64(STORY+0x150);
    while(xcde_expanded_scan_continue_wait(c)) {assert(++steps<500);assert(!memcmp(c,&before,sizeof *c));assert(LD64(STORY+0x150)==story);}
    assert(!memcmp(c,&before,sizeof *c));assert(LD64(STORY+0x150)==story);return steps;
}
static void presence_until_complete(Cpu *c) {
    Cpu before=*c;unsigned steps=0;uint64_t story=LD64(STORY+0x150);
    do {xcde_expanded_scan_presence(c);assert(++steps<1000);assert(!memcmp(c,&before,sizeof *c));assert(LD64(STORY+0x150)==story);}
    while(!(LD32(CHECK+0x304038)&2));
}
static void continue_tests(void) {
    clear_fixture();Cpu c=cpu();xcde_expanded_scan_continue_begin(SEQ);
    ST8(SEQ+0x6a3e00,1);ST64(SEQ+0x154140+8,100);
    ST8(SEQ+0x6a3e01,1);ST64(SEQ+0x154140+BYTES+8,200);
    present[0][3]=true;stamps[0][3]=250;present[0][98]=true;stamps[0][98]=999;
    continue_until_complete(&c);assert(queued==96&&copied==2);assert(!job.inflight);
    assert(LD64(SEQ+0x154140+BYTES+8)==999);assert(LD64(SEQ+0x154140+8)==100);
    for(unsigned i=0;i<96;++i)assert(queue_history[i]==i+3);
    c.x[9]=CONTEXT;c.x[8]=1;Cpu before=c;xcde_expanded_scan_continue_selected(&c);
    assert(!memcmp(&c,&before,sizeof c));assert(LD32(CONTEXT+0x18de0)==99);
    assert(remembered_story==0&&remembered_id==98&&!remembered_auto);
    clear_fixture();c=cpu();ST64(STORY+0x150,LD64(STORY+0x150)|8);
    ST8(SEQ+0x6a3e02,1);ST64(SEQ+0x154140+2*BYTES+8,500);
    present[1][5]=true;stamps[1][5]=500; // Equal timestamp keeps native first winner.
    continue_until_complete(&c);assert(queued==96&&!copied);
    c.x[9]=CONTEXT;c.x[8]=2;xcde_expanded_scan_continue_selected(&c);
    assert(remembered_story==1&&remembered_id==1&&!remembered_auto);
    clear_fixture();c=cpu();continue_until_complete(&c);assert(queued==96&&!copied);
    // Reuse/reset same owner: another complete scan starts at ID3.
    xcde_expanded_scan_continue_begin(SEQ);queued=0;continue_until_complete(&c);assert(queued==96);
}
static void presence_tests(void) {
    clear_fixture();Cpu c=cpu();present[0][98]=true;cleared[0][98]=true;present[1][97]=true;
    presence_until_complete(&c);assert(base_calls==1&&queued==192&&decoded==1);
    assert((LD32(CHECK+0x304038)&0x1f)==0x1f);assert(!job.inflight);
    for(unsigned i=0;i<192;++i)assert(queue_history[i]==(((i/96)<<16)|(i%96+3)));
    unsigned previous=queued;xcde_expanded_scan_presence(&c);assert(queued==previous&&base_calls==2);
    clear_fixture();c=cpu();xcde_expanded_scan_presence(&c);assert(queued==1&&job.inflight);
    xcde_expanded_scan_presence_reset(CHECK);ST32(CHECK+0x304038,1);
    xcde_expanded_scan_presence(&c);assert(base_calls==1&&queued==1); // Draining; no replacement IO.
    xcde_expanded_scan_presence(&c);assert(base_calls==1&&queued==1);
    xcde_expanded_scan_presence(&c);assert(base_calls==2&&queued==2&&queue_history[1]==3);
    presence_until_complete(&c);assert(queued==193&&!decoded); // Abandoned result was not decoded.
    clear_fixture();c=cpu();reset_during_call=0x7000087040ull;
    xcde_expanded_scan_presence(&c);assert(queued==1&&job.inflight);
    xcde_expanded_scan_presence(&c);assert(base_calls==1&&queued==1);
    xcde_expanded_scan_presence(&c);assert(base_calls==1&&queued==1);
    xcde_expanded_scan_presence(&c);assert(base_calls==2&&queued==2);
    presence_until_complete(&c);
    // Reset during decoding must discard old aggregate flags and restart.
    clear_fixture();c=cpu();present[0][3]=true;cleared[0][3]=true;
    reset_during_call=0x7000073f00ull;presence_until_complete(&c);
    assert(base_calls==2&&queued==193&&decoded==2);
    xcde_expanded_scan_presence_drop(CHECK); // Destructor cleanup is idempotent.
    xcde_expanded_scan_presence_drop(CHECK);
}
static bool fc_presence_policy_test(void) {
    clear_fixture();Cpu c=cpu();
    ST64(STORY+0x150,LD64(STORY+0x150)|8);
    present[0][98]=true;cleared[0][98]=true;present[1][97]=true;
    presence_until_complete(&c);
    // The native FC title path records main-save availability but deliberately
    // skips main-story reconstruction/clear checks while global story bit3 is set.
    uint32_t flags=LD32(CHECK+0x304038);
    if (decoded || (flags&4) || (flags&0x1b)!=0x1b || queued!=192) {
        fprintf(stderr,"FC native policy mismatch: decoded=%u flags=%x queued=%u\n",decoded,flags,queued);
        return false;
    }
    return true;
}
int main(void) {
    void *p=mmap((void*)BASE,0x2000000,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);
    assert(p==(void*)BASE);continue_tests();presence_tests();
    if(!fc_presence_policy_test()) return 1;
    puts("PASS: bounded Continue/availability scans, 99 slots, main/FC isolation, native copy/decoder calls, CPU/story restoration, stable ties, missing slots, reset drain and owner reuse");
    return 0;
}
