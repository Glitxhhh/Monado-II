// Actual production temporal/history/dispatch/report functions. Authored SDK
// boundary responses verify arguments and failure transitions, not vendor pixels.
#include "nvn/vulkan/temporal.h"
#include "nvn/vulkan/temporal_camera.h"
#include "nvn/vulkan/temporal_evidence.h"
#include "settings.h"
#include "ffx_api/ffx_api.h"
#include "ffx_api/ffx_upscale.h"
#include <assert.h>
#include <math.h>
#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
Settings g_settings;VkG g_vk;
typedef struct{TexBackend b;VkImageView v;}Img;
static Img img[6];
enum{F_COLOR=VK_FORMAT_R8G8B8A8_UNORM,F_LINEAR=VK_FORMAT_R16G16B16A16_SFLOAT,F_MV=VK_FORMAT_R16G16_SFLOAT,F_DEPTH=VK_FORMAT_R32_SFLOAT,F_MASK=VK_FORMAT_R8_UNORM};
static TmpCamera previous;static uint32_t hist_rw,hist_rh,hist_ow,hist_oh;
static int hist_upscaler,hist_quality,hist_provider;static uint64_t hist_frame,frames,t_last;
static uint32_t jit_phases=8;static bool effective_linear;
static TexBackend *game_vel;static uint64_t game_vel_frame,snap_frame=~0ull;
static uint64_t fsr_ring[4];static unsigned fsr_slot;
static ffxContext fsr_ctx=(void*)1;
static int requested_provider=1;static bool context_ready=true;static ffxReturnCode_t sdk_result;
static uint64_t present_frame,pending=1,completed=0,now=1000000000;
static unsigned dispatched,prepared,waited,idled,normal_rows;
static bool last_reset;static float expected_jitter[2]={.25f,-.16666667f};
static char last_log[2048];
static void note(const char *format,...){va_list args;va_start(args,format);vsnprintf(last_log,sizeof last_log,format,args);va_end(args);if(!strncmp(last_log,"[temporal]",10))normal_rows++;}
#undef LOG
#define LOG(...) note(__VA_ARGS__)
uint64_t SDL_GetTicksNS(void){return now;}
uint64_t renderer_present_count(void){return present_frame;}
static int temporal_provider(uint32_t rw,uint32_t rh,uint32_t ow,uint32_t oh){(void)rw;(void)rh;(void)ow;(void)oh;return requested_provider;}
bool tmp_linear_output(void){return effective_linear;}
const char *tmp_fsr4_status(void){return "authored optional provider boundary";}
static bool ffx_load(void){return true;}
static bool fsr_context(uint32_t w,uint32_t h){assert(w==100&&h==100);fsr_ctx=context_ready?(void*)1:NULL;return context_ready;}
static void img_make(Img *image,VkFormat format,uint32_t w,uint32_t h,VkImageUsageFlags usage){(void)usage;image->b=(TexBackend){.image=(VkImage)(uintptr_t)(image-img+1),.width=w,.height=h,.depth=1,.levels=1,.format=format};image->v=(VkImageView)(uintptr_t)(image-img+1);}
static void prep(VkCommandBuffer cb,VkImageView a,VkImageView b,VkImageView v,TexBackend *depth,const double matrix[16],bool history,const float j[2],bool dm1,uint32_t rw,uint32_t rh){
    (void)cb;(void)a;(void)b;(void)v;(void)matrix;(void)history;(void)dm1;
    assert(depth&&depth->width>=70&&rw>=70&&rh>=70&&isfinite(j[0])&&isfinite(j[1]));prepared++;
}
static void vel_fit(VkCommandBuffer cb,uint32_t w,uint32_t h){(void)cb;(void)w;(void)h;}
static void mv_shot(VkCommandBuffer cb,uint32_t w,uint32_t h,const double matrix[16],bool history){(void)cb;(void)w;(void)h;(void)matrix;(void)history;}
void vk_full_barrier(VkCommandBuffer cb){(void)cb;}
uint64_t vk_completed_seq(void){return completed;}
uint64_t vk_pending_seq(void){return pending;}
void vk_wait_seq(uint64_t seq){waited++;completed=seq;}
void tmp_fg_idle(VkCommandBuffer cb){(void)cb;idled++;}
static void fg_frame(VkCommandBuffer cb,bool okay,const TmpCamera *camera,const float jitter[2],uint32_t rw,uint32_t rh,float dt,bool reset){(void)cb;(void)okay;(void)camera;(void)jitter;(void)rw;(void)rh;(void)dt;(void)reset;}
static bool fsr4_evaluate(VkCommandBuffer cb,Img *a,Img *b,Img *c,Img *d,const float jitter[2],bool reset){(void)cb;(void)a;(void)b;(void)c;(void)d;(void)jitter;(void)reset;return sdk_result==0;}
bool dlss_evaluate(VkCommandBuffer cb,const TexBackend *a,const TexBackend *b,const TexBackend *c,const TexBackend *d,const TexBackend *mask,const float j[2],bool reset,const TmpCamera *camera,const double matrix[16],uint64_t current,uint64_t done){(void)cb;(void)a;(void)b;(void)c;(void)d;(void)mask;(void)j;(void)reset;(void)camera;(void)matrix;(void)current;(void)done;return sdk_result==0;}
static ffxReturnCode_t ffx_dispatch(ffxContext *ctx,const ffxDispatchDescHeader *header){
    const struct ffxDispatchDescUpscale *d=(const void*)header;
    assert(ctx==&fsr_ctx&&*ctx&&header->type==FFX_API_DISPATCH_DESC_TYPE_UPSCALE);
    assert(d->color.resource==(void*)img[0].b.image&&d->motionVectors.resource==(void*)img[1].b.image&&d->depth.resource==(void*)img[2].b.image&&d->output.resource==(void*)img[3].b.image);
    assert(d->color.description.width==d->renderSize.width&&d->color.description.height==d->renderSize.height&&d->upscaleSize.width==100&&d->upscaleSize.height==100);
    assert(d->jitterOffset.x==expected_jitter[0]&&d->jitterOffset.y==expected_jitter[1]);
    assert(isfinite(d->frameTimeDelta)&&d->frameTimeDelta>=1&&d->frameTimeDelta<=100&&d->preExposure==1);
    assert(d->reactive.resource==(void*)img[5].b.image&&(d->flags&FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB));
    dispatched++;last_reset=d->reset;return sdk_result;
}
#include "temporal_dispatch_production.h"
static void advance(void){present_frame++;pending++;now+=16666667;}
int main(void){
    g_settings.upscaler=UPSCALE_DLSS;g_settings.upscale_quality=UQ_NATIVE;
    TmpCamera camera={.ok=true};camera.view[0]=camera.view[5]=camera.view[10]=1;
    camera.proj[0]=camera.proj[5]=camera.proj[10]=camera.proj[15]=1;memcpy(camera.viewproj,camera.proj,sizeof camera.proj);
    TexBackend color={.image=(VkImage)(uintptr_t)20,.width=100,.height=100,.aspect=VK_IMAGE_ASPECT_COLOR_BIT};
    TexBackend depth={.image=(VkImage)(uintptr_t)21,.width=70,.height=70,.aspect=VK_IMAGE_ASPECT_DEPTH_BIT};
    VkImageView view=(VkImageView)(uintptr_t)30;
    assert(tmp_upscale(NULL,&color,view,&depth,&camera,expected_jitter,true,100,100)&&last_reset&&strstr(tmp_provider_status(),"FSR 3.1: dispatch accepted"));
    advance();assert(tmp_upscale(NULL,&color,view,&depth,&camera,expected_jitter,true,100,100)&&!last_reset&&!strcmp(tmp_evidence.history,"keep"));
    advance();depth.width=depth.height=80;assert(tmp_upscale(NULL,&color,view,&depth,&camera,expected_jitter,true,100,100)&&last_reset&&!strcmp(tmp_evidence.history,"scene-depth-grid"));
    advance();color.width=color.height=80;assert(tmp_upscale(NULL,&color,view,&depth,&camera,expected_jitter,true,100,100)&&last_reset&&!strcmp(tmp_evidence.history,"input-grid"));
    advance();unsigned before=dispatched;assert(!tmp_upscale(NULL,&color,view,&depth,NULL,expected_jitter,true,100,100)&&!previous.ok&&dispatched==before&&strstr(tmp_provider_status(),"no valid scene"));
    advance();context_ready=false;assert(!tmp_upscale(NULL,&color,view,&depth,&camera,expected_jitter,true,100,100)&&strstr(tmp_provider_status(),"context unavailable"));
    advance();context_ready=true;sdk_result=FFX_API_RETURN_ERROR_RUNTIME_ERROR;assert(!tmp_upscale(NULL,&color,view,&depth,&camera,expected_jitter,true,100,100)&&!previous.ok&&strstr(tmp_provider_status(),"dispatch rejected")&&tmp_evidence.sdk_result==sdk_result);
    advance();requested_provider=0;assert(!tmp_upscale(NULL,&color,view,&depth,&camera,expected_jitter,true,100,100)&&strstr(tmp_provider_status(),"component unavailable"));
    advance();requested_provider=1;sdk_result=0;for(unsigned i=0;i<4;i++)fsr_ring[i]=pending;
    before=dispatched;assert(!tmp_upscale(NULL,&color,view,&depth,&camera,expected_jitter,true,100,100)&&dispatched==before&&strstr(tmp_provider_status(),"recording capacity"));
    g_settings.upscaler=UPSCALE_BILINEAR;tmp_note_spatial_frame();assert(!strcmp(tmp_provider_status(),"Spatial scaling selected."));
    TmpSceneEvidence scene={.frame=present_frame,.guest_depth_w=896,.guest_depth_h=504,.camera_valid=true,.jittered_draws=1};
    for(unsigned i=0;i<100;i++){atomic_store(&tmp_public_status,i%2?TMP_STATUS_SPATIAL:TMP_STATUS_FSR3);tmp_diagnostics_frame(&scene,100,100,100,100,expected_jitter);}
    assert(normal_rows==32&&strstr(last_log,"color 100x100")&&!strstr(last_log,"save/"));
    assert(!setenv("XCDE_TMP_LOG","1",1));for(unsigned i=0;i<300;i++){now+=3000000000ull;tmp_diagnostics_frame(&scene,100,100,100,100,expected_jitter);}
    assert(normal_rows==288&&prepared>=5&&idled>=2);
    puts("PASS production temporal descriptors/history/depth-grid/fallback/status/bounded diagnostics; SDK boundary authored");return 0;
}
