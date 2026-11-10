// LINUX-046: authored shader/control bytes; no game input or GPU required.
#define _GNU_SOURCE
#include <assert.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
static bool fail_malloc, fail_calloc;
static void *fixture_malloc(size_t n) { if (fail_malloc) { fail_malloc=false; return NULL; } return malloc(n); }
static void *fixture_calloc(size_t n,size_t z) { if (fail_calloc) { fail_calloc=false; return NULL; } return calloc(n,z); }
#define malloc fixture_malloc
#define calloc fixture_calloc
#ifndef NVN_OBJECTS_SOURCE
#define NVN_OBJECTS_SOURCE "../../runtime/nvn/objects.c"
#endif
#include NVN_OBJECTS_SOURCE
#undef malloc
#undef calloc

#define BASE (HEAP_BASE+0x10000000)
#define CODE (BASE+0x1000)
#define CONTROL (BASE+0x20000)
#define DATA (BASE+0x30000)
#define HANDLE (BASE+0x31000)
static jmp_buf fatal_jump;
static bool expect_fatal;
static unsigned cases, registered;
Settings g_settings;
uint64_t kernel_heap_size(void) { return HEAP_MAX; }
void rt_fatal_trace(void) { assert(expect_fatal); longjmp(fatal_jump,1); }
void vk_program_registered(const NvnProgram *p) { assert(p->frozen); ++registered; }

static void prepare(unsigned stage, uint32_t off, uint32_t size) {
    uint8_t *code=(void *)(uintptr_t)CODE, *control=(void *)(uintptr_t)CONTROL;
    for (size_t i=0;i<0x10000;i++) code[i]=(uint8_t)(i*13+7);
    memset(control,0,0x1000);
    *(uint32_t *)code=stage==5?0:0x12345678;
    static const unsigned sph[]={1,5,4,2,3,0};
    *(uint32_t *)(code+0x30)=sph[stage]<<10;
    *(uint32_t *)(control+0x700)=off; *(uint32_t *)(control+0x6fc)=size;
    *(uint32_t *)(control+0x6e8)=17;
    *(uint32_t *)(control+0x748)=7; *(uint32_t *)(control+0x74c)=11; *(uint32_t *)(control+0x750)=13;
    *(uint64_t *)(uintptr_t)DATA=CODE; *(uint64_t *)(uintptr_t)(DATA+8)=CONTROL;
}
static void registration(NvnProgram *p) {
    *(uint64_t *)(uintptr_t)HANDLE=(uintptr_t)p;
    Cpu c={0}; c.x[0]=HANDLE; c.x[1]=1; c.x[2]=DATA;
    hle_nvnProgramSetShaders(&c); assert(c.x[0]==1);
}
#define EXPECT_FATAL(call) do { expect_fatal=true; if (!setjmp(fatal_jump)) { call; assert(!"expected fatal"); } expect_fatal=false; assert(!pthread_mutex_trylock(&snap_lock)); pthread_mutex_unlock(&snap_lock); } while(0)

static void registration_snapshot(void) {
    NvnProgram p={0}; prepare(1,0x1000,37);
    uint8_t expected[37]; memcpy(expected,(void *)(uintptr_t)(CODE+0x1000),sizeof expected);
    registration(&p);
    const uint8_t *constants=NULL; uint64_t size=0;
    if (!nvn_stage_constants(&p.stages[1],&constants,&size)) {
        fputs("FAIL: compiler constants are not owned by the registration\n",stderr); exit(1);
    }
    assert(size==sizeof expected && !memcmp(constants,expected,sizeof expected));
    assert(p.stages[1].control!=(void *)(uintptr_t)CONTROL);
    assert(p.stages[1].code_copy_size>=0x1000+sizeof expected);
    const NvnProgram *frozen=p.frozen;
    memset((void *)(uintptr_t)CODE,0,0x10000); memset((void *)(uintptr_t)CONTROL,0,0x1000);
    memset(&p,0,sizeof p); // original program and streaming memory are recycled
    assert(nvn_stage_constants(&frozen->stages[1],&constants,&size));
    assert(size==sizeof expected && !memcmp(constants,expected,sizeof expected));
    assert(*(const uint32_t *)(frozen->stages[1].control+0x6e8)==17);
    ++cases;
}
static void dedup_and_tail(void) {
    NvnProgram a={0},b={0},d={0}; prepare(0,0x1000,37); registration(&a); registration(&b);
    assert(a.frozen==b.frozen && a.stages[0].code_copy==b.stages[0].code_copy);
    ((uint8_t *)(uintptr_t)CODE)[0x1000+36]^=0x80; registration(&d);
    assert(a.stages[0].code_copy!=d.stages[0].code_copy);
    const uint8_t *data; uint64_t size; assert(nvn_stage_constants(&d.stages[0],&data,&size));
    assert(data[36]!=a.stages[0].code_copy[0x1000+36]);
    // Preserve authored feature hashes even though the snapshot dedup also
    // distinguishes the legacy hash's trailing non-word-aligned bytes.
    assert(a.stages[0].code_hash==d.stages[0].code_hash);
    ++cases;
}
static void compute_control(void) {
    NvnProgram p={0}; prepare(5,0,0); registration(&p);
    const uint8_t *ctl=p.frozen->stages[5].control;
    memset((void *)(uintptr_t)CONTROL,0,0x1000);
    assert(*(const uint32_t *)(ctl+0x748)==7 && *(const uint32_t *)(ctl+0x74c)==11 && *(const uint32_t *)(ctl+0x750)==13);
    const uint8_t *data; uint64_t size=99;
    assert(nvn_stage_constants(&p.stages[5],&data,&size) && size==0);
    assert(p.stages[5].code_copy_size==0x10000); ++cases;
}
static void accessor_rejects(void) {
    uint8_t bytes[16]={0}; NvnShaderStage s={0}; const uint8_t *p=NULL; uint64_t size=0;
    assert(!nvn_stage_constants(&s,&p,&size));
    s.code_copy=bytes; s.code_copy_size=sizeof bytes; s.const_offset=16; s.const_size=1;
    assert(!nvn_stage_constants(&s,&p,&size));
    s.const_offset=UINT32_MAX; s.const_size=UINT32_MAX;
    assert(!nvn_stage_constants(&s,&p,&size));
    s.const_offset=16; s.const_size=0; assert(nvn_stage_constants(&s,&p,&size) && !size); ++cases;
}
#ifndef BASELINE_SNAPSHOTS
static void bounds_and_transaction(void) {
    NvnProgram p={0}; prepare(1,0x1000,37); registration(&p); NvnProgram old=p;
    prepare(1,UINT32_MAX,4096); EXPECT_FATAL(registration(&p)); assert(!memcmp(&old,&p,sizeof p));
    prepare(1,0x1000,37); *(uint64_t *)(uintptr_t)(DATA+8)=GUEST_HI-16;
    EXPECT_FATAL(registration(&p)); assert(!memcmp(&old,&p,sizeof p));
    *(uint64_t *)(uintptr_t)DATA=0;
    EXPECT_FATAL(registration(&p)); assert(!memcmp(&old,&p,sizeof p));
    assert(!shader_guest_span(GUEST_HI-8,16)); assert(!shader_guest_span(HEAP_BASE+HEAP_MAX-8,16));
    // Graphics controls need only their actual metadata extent; do not read the
    // compute-only extension beyond a tight allocation at the heap boundary.
    uint64_t last_page=HEAP_BASE+HEAP_MAX-0x1000;
    assert(mmap((void *)(uintptr_t)last_page,0x1000,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0)==(void *)(uintptr_t)last_page);
    uint64_t tight=HEAP_BASE+HEAP_MAX-SHADER_GRAPHICS_CONTROL_SIZE;
    prepare(1,0x1000,37); memcpy((void *)(uintptr_t)tight,(void *)(uintptr_t)CONTROL,SHADER_GRAPHICS_CONTROL_SIZE);
    *(uint64_t *)(uintptr_t)(DATA+8)=tight; registration(&p); old=p;
    prepare(5,0x1000,37); *(uint64_t *)(uintptr_t)(DATA+8)=tight;
    EXPECT_FATAL(registration(&p)); assert(!memcmp(&old,&p,sizeof p));
    assert(!munmap((void *)(uintptr_t)last_page,0x1000));
    ++cases;
}
static void allocation_failures(void) {
    NvnProgram p={0}; prepare(1,0x1000,37); registration(&p); NvnProgram old=p;
    Cpu c={0}; c.x[0]=HANDLE; fail_calloc=true;
    EXPECT_FATAL(hle_nvnProgramInitialize(&c)); assert(HOST(NvnProgram,HANDLE)==&p);
    fail_malloc=true; EXPECT_FATAL(registration(&p)); assert(!memcmp(&old,&p,sizeof p));
    // Failed registry growth preserves existing owned snapshots and releases its lock.
    uint64_t serial=0x800000;
    while (snap_n<snap_cap/2) {
        *(uint64_t *)(uintptr_t)CODE=++serial; (void)shader_snapshot(CODE,8,"fixture");
    }
    size_t cap=snap_cap; *(uint64_t *)(uintptr_t)CODE=++serial;
    fail_calloc=true; EXPECT_FATAL((void)shader_snapshot(CODE,8,"fixture")); assert(snap_cap==cap);
    NvnProgram unique=old;
    while (frozen_n<frozen_cap/2) { unique.stages[1].code_hash=++serial; (void)program_freeze(&unique); }
    cap=frozen_cap; unique.stages[1].code_hash=++serial;
    fail_calloc=true; EXPECT_FATAL((void)program_freeze(&unique)); assert(frozen_cap==cap);
    // Grow successfully, then independently fail allocation of the metadata.
    (void)program_freeze(&unique); unique.stages[1].code_hash=++serial;
    fail_malloc=true; EXPECT_FATAL((void)program_freeze(&unique));
    assert(nvn_stage_code(&old.stages[1])==(uintptr_t)old.stages[1].code_copy); ++cases;
}
static void former_capacity(void) {
    NvnProgram p={0}; p.stage_mask=2; p.stages[1].stage=1;
    const NvnProgram *first=NULL;
    for (uint64_t k=0;k<32769;k++) {
        p.stages[1].code_addr=GUEST_LO+k*8; p.stages[1].code_hash=k;
        const NvnProgram *f=program_freeze(&p); assert(f && f->frozen==f);
        if (!k) first=f;
    }
    assert(frozen_n>32768 && frozen_cap>32768 && first->stages[1].code_addr==GUEST_LO);
    p.stages[1].code_addr=GUEST_LO; p.stages[1].code_hash=0;
    assert(program_freeze(&p)==first);
    for (uint64_t k=0;k<32769;k++) {
        *(uint64_t *)(uintptr_t)CODE=k;
        const uint8_t *v=shader_snapshot(CODE,8,"fixture"); assert(*(const uint64_t *)v==k);
    }
    assert(snap_n>32768 && snap_cap>32768); ++cases;
}
static NvnProgram threaded_program;
static const NvnProgram *threaded_result[2];
static void *freeze_thread(void *arg) { size_t n=(size_t)arg; threaded_result[n]=program_freeze(&threaded_program); return NULL; }
static void concurrent_dedup(void) {
    threaded_program.stage_mask=1; threaded_program.stages[0].code_hash=0xabcdef1234;
    pthread_t a,b; assert(!pthread_create(&a,NULL,freeze_thread,(void *)0)); assert(!pthread_create(&b,NULL,freeze_thread,(void *)1));
    assert(!pthread_join(a,NULL)); assert(!pthread_join(b,NULL)); assert(threaded_result[0]==threaded_result[1]); ++cases;
}
#endif
int main(void) {
    assert(mmap((void *)(uintptr_t)BASE,0x40000,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0)==(void *)(uintptr_t)BASE);
    // Historical diagnostic switches cannot silently restore mutable pointers.
#ifndef BASELINE_SNAPSHOTS
    assert(!setenv("XCDE_NO_CODE_SNAPSHOT","1",1)); assert(!setenv("XCDE_NO_PROGRAM_FREEZE","1",1));
#endif
    registration_snapshot(); dedup_and_tail(); compute_control(); accessor_rejects();
#ifndef BASELINE_SNAPSHOTS
    bounds_and_transaction(); allocation_failures(); former_capacity(); concurrent_dedup();
#endif
    assert(registered>=4); printf("immutable shader snapshots: PASS (%u contract groups)\n",cases); return 0;
}
