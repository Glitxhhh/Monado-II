// [XCDE] LINUX-018: actual extraction/orchestration on authored bytes and real pthreads; GPU/NGX calls forbidden.
#define _GNU_SOURCE
#include <assert.h>
#include <pthread.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdatomic.h>
static int test_thread_create(pthread_t *,const pthread_attr_t *,void *(*)(void *),void *);
#define pthread_create test_thread_create
#ifndef PRECOMPILE_SOURCE
#define PRECOMPILE_SOURCE "../../runtime/nvn/vulkan/precompile.c"
#endif
#include PRECOMPILE_SOURCE
#undef pthread_create

VkG g_vk;
uint64_t vk_spirv_hits, vk_spirv_misses;
static const char *scenario;
static _Atomic unsigned calls, variant_calls, thread_calls;
static unsigned ui_calls;
static VkPipelineCache saved_cache;
static uint8_t *container; static size_t container_size;
static int test_thread_create(pthread_t *t,const pthread_attr_t *a,void *(*fn)(void *),void *arg) {
    unsigned at=atomic_fetch_add(&thread_calls,1);
    if(!strcmp(scenario,"thread-fail") && !at) return EAGAIN;
    return pthread_create(t,a,fn,arg);
}
void plat_thread_background(void) {}
int plat_cpu_count(void) { return 3; }
int SDL_GetSystemRAM(void) { return 16384; }
void ui_window_init(void) {
    ui_calls++; g_vk.dev=(VkDevice)(uintptr_t)0x123;
    saved_cache=g_vk.pcache=(VkPipelineCache)(uintptr_t)0x456; g_vk.gpl=true;
    g_vk.props.vendorID=0x10de; g_vk.props.deviceID=0x2783; g_vk.props.driverVersion=123;
    for(unsigned i=0;i<VK_UUID_SIZE;i++) g_vk.props.pipelineCacheUUID[i]=(uint8_t)i;
}
static void cancel_file(const char *token) {
    FILE *f=fopen(PRECOMPILE_CANCEL_PATH,"w"); assert(f);
    fprintf(f,PRECOMPILE_CANCEL_MAGIC "\nsession %s\n",token); assert(!fclose(f));
}
int vk_precompile_program(const NvnProgram *p,const ShaderKey *v,const ShaderKey *f,uint32_t *attr) {
    assert(ui_calls==1 && g_vk.pcache==VK_NULL_HANDLE && p->stage_mask==3 && v->glsl && f->glsl);
    unsigned n=atomic_fetch_add(&calls,1);
    if(attr) *attr=2;
    if(!strcmp(scenario,"crash") && !n) _exit(3);
    if((!strcmp(scenario,"cancel") || !strcmp(scenario,"signal-cancel")) && !n) {
        if(!strcmp(scenario,"cancel")) cancel_file(session_id); else raise(SIGTERM);
        usleep(150000);
    }
    if(!strcmp(scenario,"extra-failure") && v->attr_kind[1]==2) return 0;
    if(!strcmp(scenario,"seed-failure") && v->attr_kind[2]==2) return 0;
    return 1;
}
size_t vk_precompile_variant_size(void) { return 32+2*sizeof(ShaderKey)+64; }
int vk_precompile_variant(const NvnProgram *p,const void *r,size_t n) {
    (void)r; assert(p->stage_mask==3 && g_vk.pcache==saved_cache && n==vk_precompile_variant_size());
    atomic_fetch_add(&variant_calls,1); return strcmp(scenario,"variant-failure")!=0;
}
bool vk_spirv_cache_flush(void) { assert(g_vk.pcache==saved_cache); return strcmp(scenario,"cache-failure")!=0; }
uint64_t vk_spirv_cache_errors(void) { return !strcmp(scenario,"cache-failure"); }
bool vk_pipeline_cache_save_checked(void) { assert(g_vk.pcache==saved_cache); return true; }
VKAPI_ATTR void VKAPI_CALL vkDestroyPipelineCache(VkDevice d,VkPipelineCache c,const VkAllocationCallbacks *a) {
    (void)d;(void)c;(void)a; abort(); // Linux must retain the initial handle instead.
}
static void wr32(size_t p,uint32_t v) { assert(p+4<=container_size); memcpy(container+p,&v,4); }
static uint64_t blob_hash(size_t off) {
    uint64_t h=0xcbf29ce484222325ull;
    for(size_t i=0;i<0x100;i+=8) { uint64_t w; memcpy(&w,container+off+i,8); h=(h^w)*0x100000001b3ull; }
    return h;
}
static void no_constants_container(bool short_blob) {
    free(container); container_size=0x2600; container=calloc(1,container_size); assert(container);
    uint32_t blob=short_blob?0x100:0x800;
    memcpy(container,"HCPS",4);wr32(4,0x3f2);wr32(0x20,0x100);wr32(0x24,2*blob);wr32(0x28,0x1200);wr32(0x2c,0x1000);
    for(unsigned i=0;i<2;i++) {
        size_t code=0x100+i*blob,ctl=0x1200+i*0x800;
        wr32(code,0x12345678);wr32(code+0x30,(i?5:1)<<10);
        wr32(ctl,0x98761234);wr32(ctl+0x704,blob);
    }
}
static void make_container(void) {
    container_size=0x1600; container=calloc(1,container_size); assert(container);
    memcpy(container,"HCPS",4); wr32(4,0x3f2);wr32(8,0x30);wr32(12,1);wr32(24,0x1400);
    wr32(0x20,0x100);wr32(0x24,0x200);wr32(0x28,0x400);wr32(0x2c,0x1000);
    for(unsigned i=0;i<2;i++) {
        size_t code=0x100+i*0x100,ctl=0x400+i*0x800;
        wr32(code,0x12345678);wr32(code+0x30,(i?5:1)<<10);
        wr32(ctl,0x98761234);wr32(ctl+0x700,0x100);wr32(ctl+0x704,0x100);
    }
    memcpy(container+0x1400,"SLCT",4);wr32(0x1410,1);wr32(0x1414,0x30);
    wr32(0x1430,0x40);wr32(0x1434,0x100);
    size_t pd=0x1440;wr32(pd+24,0x58);wr32(pd+28,1);wr32(pd+0x58+12,0x800);wr32(pd+0x58+16,0x800);
    wr32(pd+80,1);wr32(pd+84,0x70);wr32(pd+0x70,0x78);wr32(pd+0x74,0);
    memcpy(container+pd+0x78,"nWeight",8);container[pd+0x38]=1;wr32(pd+0x3c,0x80);
    wr32(pd+0x80,0x90);container[pd+0x88]=0;container[pd+0x89]=1;memcpy(container+pd+0x90,"gTStencil",10);
}
static void put_input(const char *path,const void *data,size_t n) {
    FILE *f=fopen(path,"wb");assert(f);assert(fwrite(data,1,n,f)==n);assert(!fclose(f));
}
static void local_variant(const char *path) {
    size_t n=vk_precompile_variant_size(); uint8_t *rec=calloc(1,n);assert(rec);
    uint64_t h[3]={blob_hash(0x100),blob_hash(0x200),0};memcpy(rec,h,sizeof h);
    if(!strcmp(scenario,"unsupported")) rec[24]=1;
    ShaderKey v={0},f={0};v.glsl=f.glsl=1;v.attr_kind[2]=2;
    memcpy(rec+32,&v,sizeof v);memcpy(rec+32+sizeof v,&f,sizeof f);
    FILE *out=fopen(path,"wb");assert(out);uint32_t hdr[2]={0x56435843,(uint32_t)n};
    assert(fwrite(hdr,4,2,out)==2 && fwrite(rec,1,n,out)==n && !fclose(out)); free(rec);
}
int main(int argc,char **argv) {
    assert(argc==2); scenario=argv[1]; make_container();
    if(!strcmp(scenario,"constants-fallback") || !strcmp(scenario,"constants-short")) {
        no_constants_container(!strcmp(scenario,"constants-short")); scan_buf(container,container_size);
        if(!strcmp(scenario,"constants-short")) assert(!nfound && !nprogs && n_unsupported==2 && !n_errors);
        else {
            assert(nfound==2 && nprogs==1 && !n_unsupported && !n_errors);
            assert(!found[0].named && !found[0].int_in && !found[0].coff && !found[0].csz);
            for(unsigned stage=0;stage<2;stage++) {
                uint64_t expected=0xcbf29ce484222325ull;
                for(size_t p=0;p<0x800;p+=8) { uint64_t word;memcpy(&word,container+0x100+stage*0x800+p,8);expected=(expected^word)*0x100000001b3ull; }
                assert(found[stage].hash==expected);
            }
        }
        assert(!ui_calls); puts("bounded no-constants runtime-compatible hash passed");return 0;
    }
    if(!strcmp(scenario,"allocator-fill")) {
        capfound=4; found=malloc(capfound*sizeof *found); assert(found);
        memset(found,0xa5,capfound*sizeof *found);
        // Valid nonzero metadata models reused allocator storage without an
        // undefined bool representation or any malformed input container.
        for(size_t i=0;i<capfound;i++) { found[i].named=false; found[i].int_in=0x40000000u; }
    }
    if(!strcmp(scenario,"incidental-magic")) {
        memcpy(container+0x1580,"HCPS",4);
        wr32(0x1584,0xa49a4da9);wr32(0x15a0,UINT32_MAX);wr32(0x15a8,UINT32_MAX);
    }
    if(!strcmp(scenario,"unsupported-version")) {
        wr32(4,0x3f3);scan_buf(container,container_size);
        assert(!nfound && !nprogs && n_unsupported==1 && !n_errors && !ui_calls);
        puts("trusted container beginning reports unsupported version");return 0;
    }
    if(!strcmp(scenario,"fuzz")) {
        uint32_t random=0x12345678; uint8_t *saved=malloc(container_size); assert(saved);memcpy(saved,container,container_size);
        for(unsigned iteration=0;iteration<256;iteration++) {
            memcpy(container,saved,container_size);
            for(unsigned j=0;j<8;j++) { random=random*1664525u+1013904223u;size_t off=random%(container_size-4);random=random*1664525u+1013904223u;memcpy(container+off,&random,4); }
            scan_buf(container,container_size);
        }
        assert(!ui_calls);free(saved);puts("256 authored parser mutations completed");return 0;
    }
    if(!strcmp(scenario,"parser") || !strcmp(scenario,"allocator-fill") || !strcmp(scenario,"incidental-magic") || !strcmp(scenario,"recognized-bad-sections") || !strcmp(scenario,"blob-bounds") || !strcmp(scenario,"constants-overflow") || !strcmp(scenario,"metadata-overflow")) {
        if(!strcmp(scenario,"recognized-bad-sections")) wr32(0x20,UINT32_MAX);
        if(!strcmp(scenario,"blob-bounds")) { wr32(0x400+0x700,UINT32_MAX);wr32(0x400+0x704,0x20); }
        if(!strcmp(scenario,"constants-overflow")) wr32(0x400+0x700,UINT32_MAX);
        if(!strcmp(scenario,"metadata-overflow")) wr32(0x1440+0x70,UINT32_MAX);
        scan_buf(container,container_size);
        if(!strcmp(scenario,"parser") || !strcmp(scenario,"allocator-fill") || !strcmp(scenario,"incidental-magic")) {
            assert(nfound==2 && nprogs==1 && found[0].named && found[0].int_in==1 && found[1].named && found[1].int_in==1 && !n_errors);
        } else assert(n_errors>0);
        assert(!ui_calls); puts("actual parser authored valid/malformed case passed");return 0;
    }
    mkdir("input",0700);mkdir("input/romfs",0700);
    if(!strcmp(scenario,"no-programs")) memset(container,0,container_size);
    put_input("input/romfs/authored.wishp",container,container_size);
    if(!strcmp(scenario,"compressed")) {
        uLongf n=compressBound(container_size);uint8_t *compressed=calloc(1,n+0x30);assert(compressed);
        assert(compress2(compressed+0x30,&n,container,container_size,Z_BEST_SPEED)==Z_OK);
        memcpy(compressed,"xbc1",4);uint32_t size=(uint32_t)container_size,bytes=(uint32_t)n;
        memcpy(compressed+8,&size,4);memcpy(compressed+12,&bytes,4);
        assert(!unlink("input/romfs/authored.wishp"));put_input("input/romfs/authored.wismt",compressed,n+0x30);free(compressed);
    }
    if(!strcmp(scenario,"seed-failure") || !strcmp(scenario,"variant-failure") || !strcmp(scenario,"unsupported")) { mkdir("cache",0700);local_variant("cache/variants.bin"); }
    if(!strcmp(scenario,"no-seed")) { mkdir("data",0700);local_variant("data/variants_seed.bin"); }
    if(!strcmp(scenario,"symlink")) assert(!symlink("..","input/romfs/cycle"));
    setenv(PRECOMPILE_ID_ENV,"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",1);
    setenv(PRECOMPILE_SESSION_ENV,"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",1);
    if(!strcmp(scenario,"invalid-identity")) setenv(PRECOMPILE_ID_ENV,"bad",1);
    setenv("XCDE_PRECOMPILE_THREADS","2",1);
    if(!strcmp(scenario,"stale-cancel")) { mkdir("cache",0700);cancel_file("cccccccccccccccccccccccccccccccc"); }
    if(!strcmp(scenario,"early-cancel")) { mkdir("cache",0700);cancel_file("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"); }
    int result=vk_precompile_main("input");
    // Success/partial/cancelled paths exit explicitly, never return through the renderer's forced-status0 atexit.
    assert(result==1);
    if(!strcmp(scenario,"invalid-identity")) assert(!ui_calls && access("cache",F_OK)!=0);
    return result;
}
