// [XCDE] LINUX-046: compile the real authored constant resolver and program caches
// against synthetic allocator/translator boundaries; no GPU, game or caches.
#include "nvn/program_binding.h"
#include "nvn/maxwell/shader.h"
#include <assert.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#undef FATAL
#undef LOG
static jmp_buf fatal_jump;
static char fatal_text[512];
static void test_fatal(const char *fmt,...){va_list ap;va_start(ap,fmt);vsnprintf(fatal_text,sizeof fatal_text,fmt,ap);va_end(ap);longjmp(fatal_jump,1);}
#define FATAL test_fatal
#define LOG test_log
static void test_log(const char *fmt,...){(void)fmt;}
typedef struct { uint64_t buf,off;void*ptr; } VkSpan;
typedef struct { uint64_t buffer,offset,range; } VkDescriptorBufferInfo;
typedef struct { uint64_t addr,size; } Range;
static struct { NvnProgram *program;Range sbuf[6][16],ubuf[6][18]; } st;
static struct { struct { struct { uint64_t maxUniformBufferRange; } limits; } props; } g_vk;
static int g_debug_frame,debug_pass_no;
static unsigned char uploaded[65536];
static VkSpan vk_transient(uint64_t size,uint64_t align){assert(size<=sizeof uploaded&&align==256);memset(uploaded,0xa5,sizeof uploaded);return(VkSpan){1,0,uploaded};}
static VkSpan vk_transient_copy(const void*data,uint64_t size,uint64_t align){VkSpan s=vk_transient(size,align);assert(data);memcpy(s.ptr,data,size);return s;}
static VkSpan guest_copy(uint64_t data,uint64_t size){return vk_transient_copy((void*)(uintptr_t)data,size,256);}
enum {DEDUPE_DRV_C,DEDUPE_DRV_F,DEDUPE_DRV_V};
static VkSpan copy_dedupe(int slot,const void*data,uint64_t size,uint64_t align){(void)slot;return vk_transient_copy(data,size,align);}
#include "program_cbuf_source.h"
// The real cache functions run with an authored tiny content table and fake
// asynchronous build handoff. Only the inputs they retain are under test.
typedef struct {
 ShaderInfo *vi,*fi;NvnShaderStage stages[6];
 uint64_t code[2],src[2],cmax[2],gcode,gsrc,gcmax;
 ShaderKey vkey,fkey,gkey;bool no_fs;uint64_t t_new;bool prewarmed;_Atomic int state;
} Prog;
typedef struct { uint64_t key;void*val; } HEnt;
static struct { unsigned n;HEnt entries[64]; } prog_tab;
static uint32_t prog_tab_gen;
static uint8_t g_draw_gs_in=1;
static bool g_prewarming;
static unsigned builds,compute_builds;
static HEnt *htab_slot(void*tab,uint64_t key){(void)tab;for(unsigned i=0;i<64;i++)if(!prog_tab.entries[i].key||prog_tab.entries[i].key==key)return&prog_tab.entries[i];abort();}
static uint32_t stencil_unit_mask(int stage){(void)stage;return 0;}
static uint64_t SDL_GetTicksNS(void){return 1;}
static void translate_program(void*p){(void)p;}
static void run_translate(void(*fn)(void*),void*p){(void)fn;(void)p;++builds;}
#include "program_cache_source.h"
typedef struct { Prog p;ShaderInfo*si;uint32_t local[3]; } CsProg;
static unsigned g_cs_n;
static void build_compute(void*p){(void)p;}
static void run_job(void(*fn)(void*),void*p){(void)fn;(void)p;++compute_builds;}
static void xlat_lock(void){}
static pthread_mutex_t translate_lock=PTHREAD_MUTEX_INITIALIZER;
static void save_glsl(const char*src,uint64_t at,const char*suffix){(void)src;(void)at;(void)suffix;}
uint64_t maxwell_gmem_base,maxwell_gmem_size;
ShaderInfo *maxwell_translate(const uint8_t*data,size_t size,int stage,const ShaderKey*key){
 assert(data&&size&&stage==STAGE_COMPUTE&&key->glsl);static ShaderInfo si;si.msl="authored";return&si;
}
#include "program_compute_source.h"
static NvnShaderStage stage(unsigned char*blob,unsigned offset,unsigned size){
 return(NvnShaderStage){.stage=1,.code_addr=(uint64_t)(uintptr_t)blob,.code_copy=blob,.code_copy_size=128,.const_offset=offset,.const_size=size,.code_hash=0x1234};
}
int main(void){
 unsigned char blob[128],live[128];for(unsigned i=0;i<128;i++)blob[i]=live[i]=(unsigned char)(i+1);
 g_vk.props.limits.maxUniformBufferRange=65536;
 NvnProgram mutable={0};mutable.stages[1]=stage(live,32,64);st.program=&mutable;
 NvnShaderStage captured=stage(blob,32,64);ShaderInfo si={0};si.cbuf_size[1]=64;
 memset(mutable.stages,0,sizeof mutable.stages);memset(live,0xcc,sizeof live);
 VkDescriptorBufferInfo info=checked_cbuf(1,&si,&captured,1);
 assert(info.range==64&&!memcmp(uploaded,blob+32,64));
 // A required tail beyond the declared range is zero, never bytes from the
 // adjacent snapshot region. Every declared prefix byte stays exact.
 captured=stage(blob,32,16);info=checked_cbuf(1,&si,&captured,1);
 assert(info.range==64&&!memcmp(uploaded,blob+32,16));for(unsigned i=16;i<64;i++)assert(!uploaded[i]);
 for(unsigned bad=0;bad<5;bad++){
  captured=stage(blob,32,64);
  if(bad==0)captured.code_copy=NULL;
  if(bad==1)captured.const_offset=UINT32_MAX;
  if(bad==2)captured.const_size=UINT32_MAX;
  if(bad==3)captured.const_size=0;
  if(bad==4)captured.code_addr=0;
  fatal_text[0]=0;
  if(!setjmp(fatal_jump)){(void)checked_cbuf(1,&si,&captured,1);assert(!"invalid required constants accepted");}
  assert(strstr(fatal_text,"invalid compiler constants")&&strstr(fatal_text,"stage=1"));
 }
 // The graphics pipeline owns stage metadata before a translator job starts;
 // changing a bound source object cannot change its later constant lookup.
 NvnProgram np={0};np.stage_mask=3;np.stages[0]=stage(blob,16,32);np.stages[1]=stage(blob,32,64);
 ShaderKey vk={0},fk={0};Prog*p=get_program(&np,&vk,&fk);assert(p&&builds==1);
 assert(p->stages[1].code_copy==blob&&p->stages[1].const_offset==32);
 assert(get_program(&np,&vk,&fk)==p&&builds==1);
 unsigned char different[128];memcpy(different,blob,sizeof different);different[95]^=0x7f;
 // Legacy hashes intentionally ignore the final partial machine word. The
 // interned complete blob identity still distinguishes its actual constants.
 np.stages[1].code_copy=different;
 Prog*tail=get_program(&np,&vk,&fk);assert(tail&&tail!=p&&builds==2);
 info=checked_cbuf(1,&si,&tail->stages[1],1);assert(uploaded[63]==different[95]&&uploaded[63]!=blob[95]);
 np.stages[1].code_copy_size=96;
 Prog*extent=get_program(&np,&vk,&fk);assert(extent&&extent!=tail&&builds==3);
 np.stages[1].const_offset=48;np.stages[1].const_size=48; // same bytes/hash/end, different boundary
 Prog*q=get_program(&np,&vk,&fk);assert(q&&q!=p&&q!=extent&&builds==4);
 assert(p->stages[1].const_offset==32&&q->stages[1].const_offset==48);
 memset(np.stages,0,sizeof np.stages);np.stage_mask=0;
 info=checked_cbuf(1,&si,&p->stages[1],1);assert(info.range==64&&!memcmp(uploaded,blob+32,64));
 // Compute cache re-use requires content, metadata and workgroup specialization,
 // not only a recycled guest address. Its queued pipeline owns the same stage.
 unsigned char control[0x754]={0};np=(NvnProgram){0};np.stage_mask=1u<<5;
 np.stages[5]=stage(blob,32,64);np.stages[5].control=control;
 CsProg*c=compute_program(&np);assert(c&&compute_builds==1&&c->p.stages[5].code_copy==blob);
 assert(compute_program(&np)==c&&compute_builds==1);
 np.stages[5].code_copy=different;assert(compute_program(&np)!=c&&compute_builds==2);
 np.stages[5].code_copy_size=96;assert(compute_program(&np)!=c&&compute_builds==3);
 ++np.stages[5].code_hash;assert(compute_program(&np)!=c&&compute_builds==4);
 np.stages[5].const_offset=48;np.stages[5].const_size=48;
 assert(compute_program(&np)!=c&&compute_builds==5);
 uint32_t group=4;memcpy(control+0x748,&group,4);assert(compute_program(&np)!=c&&compute_builds==6);
 puts("native immutable pipeline constants and graphics/compute identity contracts: PASS");return 0;
}
