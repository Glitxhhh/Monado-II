// Production functions extracted by the runner; SDK dispatch/context boundary mocked.
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdio.h>
#include <setjmp.h>
#include <math.h>
#include "nvn/vulkan/temporal.h"
#include "nvn/framegen_policy.h"
#include "ffx_api/ffx_framegeneration.h"
#define LOG(...) ((void)0)
static jmp_buf fatal_environment;static unsigned fatal_calls;
#define FATAL(...) do{fatal_calls++;longjmp(fatal_environment,1);}while(0)
VkG g_vk;
typedef struct {TexBackend b;VkImageView v;} Img;
static Img img[6],fg_hl;
static bool fg_active,fg_inputs_ready,fg_hl_ok;
static uint64_t fg_frame_id;
static atomic_uint_fast64_t fg_generation_floor;
static atomic_bool fg_generated_ready;
static atomic_uint_fast64_t fg_reset_serial,fg_reset_ack;
static atomic_uint_fast64_t fg_last_generated_frame;
static ffxContext fg_ctx=(void*)1;
static ffxReturnCode_t dispatch_result,prepare_result,configure_result;
static unsigned calls,prepares,configures,configure_failures;
static bool context_available=true,generation_on=true;
static uint64_t (*fg_present_count)(VkSwapchainKHR);
static bool fg_context(uint32_t w,uint32_t h){assert(w&&h);return context_available;}
static bool fg_on(void){return generation_on;}
static int env_int(const char *s){(void)s;return 0;}
static struct FfxApiResource res(Img *i,uint32_t fmt,uint32_t use){(void)fmt;(void)use;struct FfxApiResource r={.resource=i};return r;}
static ffxReturnCode_t ffx_configure(ffxContext *ctx,const ffxConfigureDescHeader *p){assert(ctx&&p);configures++;if(configure_failures){configure_failures--;return FFX_API_RETURN_ERROR;}return configure_result;}
static ffxReturnCode_t ffx_dispatch(ffxContext *ctx,const ffxDispatchDescHeader *p){assert(ctx&&p);calls++;if(p->type==FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE){const struct ffxDispatchDescFrameGenerationPrepare *d=(const void*)p;assert(d->depth.resource==&img[2]&&d->motionVectors.resource==&img[1]);assert(d->renderSize.width==640&&d->renderSize.height==360);assert(d->frameTimeDelta==16.f);prepares++;return prepare_result;}return dispatch_result;}
#include "fg_qualification_source.h"
int main(void){
 ffxContext context=(void*)1;ffxDispatchDescFrameGeneration p={.frameID=5,.numGeneratedFrames=1};
 assert(!tmp_fg_inputs_ready());fg_active=fg_inputs_ready=true;fg_frame_id=6;
 assert(!tmp_fg_inputs_ready());assert(fg_dispatch_cb(&p,&context)==FFX_API_RETURN_OK&&calls==1);assert(tmp_fg_inputs_ready());
 fg_frame_id=8;assert(tmp_fg_inputs_ready());fg_frame_id=9;assert(!tmp_fg_inputs_ready());
 fg_frame_id=5;assert(!tmp_fg_inputs_ready());fg_frame_id=6;
 fg_reset();assert(!fg_inputs_ready&&!fg_hl_ok&&!tmp_fg_inputs_ready());
 p.frameID=fg_generation_floor-1;assert(fg_dispatch_cb(&p,&context)==0);fg_inputs_ready=true;assert(!tmp_fg_inputs_ready());
 p.frameID=fg_frame_id;p.reset=false;assert(fg_dispatch_cb(&p,&context)==FFX_API_RETURN_OK&&p.reset);
 fg_inputs_ready=true;fg_frame_id++;assert(tmp_fg_inputs_ready());
 dispatch_result=FFX_API_RETURN_ERROR_RUNTIME_ERROR;p.reset=false;assert(fg_dispatch_cb(&p,&context)==dispatch_result&&!tmp_fg_inputs_ready());
 dispatch_result=FFX_API_RETURN_OK;p.numGeneratedFrames=0;p.reset=false;assert(fg_dispatch_cb(&p,&context)==0&&p.reset&&!tmp_fg_inputs_ready());
 p.numGeneratedFrames=1;p.reset=false;p.frameID=fg_frame_id;assert(fg_dispatch_cb(&p,&context)==0&&!p.reset);fg_frame_id++;assert(tmp_fg_inputs_ready());
 fg_active=false;assert(!tmp_fg_inputs_ready());
 TmpCamera camera={0};camera.proj[5]=1.f;camera.proj[11]=.1f;float jitter[2]={0};g_vk.sc_extent=(VkExtent2D){1280,720};fg_active=true;
 fg_frame(VK_NULL_HANDLE,true,&camera,jitter,640,360,16.f,true);assert(prepares==1&&!fg_inputs_ready&&!tmp_fg_inputs_ready());
 fg_frame(VK_NULL_HANDLE,true,&camera,jitter,640,360,16.f,false);assert(prepares==2&&fg_inputs_ready&&!tmp_fg_inputs_ready());
 p.frameID=fg_frame_id-1;assert(fg_dispatch_cb(&p,&context)==0&&tmp_fg_inputs_ready());
 configure_failures=1;unsigned before=prepares;fg_frame(VK_NULL_HANDLE,true,&camera,jitter,640,360,16.f,false);assert(prepares==before&&!tmp_fg_inputs_ready());
 configure_result=FFX_API_RETURN_OK;prepare_result=FFX_API_RETURN_ERROR;fg_frame(VK_NULL_HANDLE,true,&camera,jitter,640,360,16.f,false);assert(!fg_inputs_ready&&!tmp_fg_inputs_ready());
 prepare_result=FFX_API_RETURN_OK;fg_frame(VK_NULL_HANDLE,false,NULL,NULL,0,0,0.f,false);assert(!fg_inputs_ready&&!tmp_fg_inputs_ready());
 context_available=false;fg_frame(VK_NULL_HANDLE,true,&camera,jitter,640,360,16.f,false);assert(!fg_inputs_ready&&!tmp_fg_inputs_ready());
 context_available=true;configure_result=FFX_API_RETURN_ERROR;
 if(!setjmp(fatal_environment)){fg_frame(VK_NULL_HANDLE,true,&camera,jitter,640,360,16.f,false);assert(!"failed provider disable must be fatal");}
 assert(fatal_calls==1&&configures>=5);
 assert(fabsf(fg_policy_base_fps(120,179.88f,true)-89.94f)<.001f);
 assert(fg_policy_base_fps(30,179.88f,true)==30.f);
 assert(fg_policy_base_fps(0,60.f,true)==30.f);
 assert(fg_policy_base_fps(120,0.f,true)==120.f);
 assert(fg_policy_base_fps(120,NAN,true)==120.f);
 assert(fg_policy_base_fps(120,179.88f,false)==120.f);puts("PASS actual native FG configure/prepare/dispatch qualification, descriptor identity, reset/error/warmup/inactive/stale-frame eligibility; no output FPS inference");return 0;}
