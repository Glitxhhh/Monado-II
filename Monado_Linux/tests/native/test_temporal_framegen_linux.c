// [XCDE] LINUX-020: actual Linux temporal adapter, authored image/API boundary mocks; no GPU/SDK/game/UI.
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../runtime/settings.h"
#include "../../runtime/nvn/vulkan/framegen_linux.h"

Settings g_settings;
typedef struct {struct {VkImage image;VkFormat format;uint32_t width,height;} b;VkImageView v;} Img;
typedef struct {float view[12],proj[16],viewproj[16];bool ok;} TmpCamera;
static struct {VkPhysicalDevice phys;VkDevice dev;VkExtent2D sc_extent;VkFormat sc_format;} g_vk;
static Img img[6];
static bool cutscene,deterministic,provider_present,fail_dispatch,no_hud,tmp_off;
static unsigned calls,opens,resource_queries,errors,copies;
static uintptr_t next_image;
static uint64_t real_frame=1;
static FgLinuxFrame observed;
struct FgLinux {bool requested,fault,history;uint64_t frame;};
static struct FgLinux provider;
static void test_log(const char *format,...){if(strstr(format,"generation disabled"))errors++;}
#define LOG(...) test_log(__VA_ARGS__)
static int env_int(const char *name){return !strcmp(name,"XCDE_FG_NO_HUDLESS")?no_hud:!strcmp(name,"XCDE_TMP_OFF")?tmp_off:0;}
static bool diag_deterministic(void){return deterministic;}
bool xcde_cutscene_30(void){return cutscene;}
bool rt_resource(const char *name,const char *dev,char *out,size_t size){assert(!strcmp(name,"libmonado-fidelityfx.so")&&!dev);resource_queries++;snprintf(out,size,"/authored/module.so");return provider_present;}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device,const char *name){(void)device;(void)name;return NULL;}
FgLinux *fg_linux_open(const char *path,VkPhysicalDevice physical,VkDevice device,PFN_vkGetDeviceProcAddr proc,char *error,size_t size){
 assert(!strcmp(path,"/authored/module.so")&&physical==g_vk.phys&&device==g_vk.dev&&proc==vkGetDeviceProcAddr);(void)error;(void)size;opens++;return &provider;}
void fg_linux_reset(FgLinux *fg){assert(fg==&provider);fg->history=false;}
void fg_linux_collect(FgLinux *fg,uint64_t done){assert(fg==&provider&&done==0);}
void fg_linux_set_enabled(FgLinux *fg,bool enabled){assert(fg==&provider);if(enabled&&!fg->requested)fg->fault=false;if(!enabled)fg->history=false;fg->requested=enabled;}
const char *fg_linux_error(const FgLinux *fg){assert(fg==&provider);return "authored storage error";}
FgLinuxResult fg_linux_dispatch(FgLinux *fg,const FgLinuxFrame *frame){
 assert(fg==&provider&&frame&&frame->commands&&frame->scene.image&&frame->output.image&&frame->depth.image&&frame->motion.image);
 assert(frame->recording_seq>frame->completed_seq&&frame->real_frame_id==real_frame);observed=*frame;calls++;
 if(!fg->requested)return FG_LINUX_DISABLED;
 if(fail_dispatch||fg->fault){fg->fault=true;fg->history=false;return FG_LINUX_ERROR;}
 bool reset=frame->reset||!fg->history||frame->real_frame_id!=fg->frame+1;
 fg->history=true;fg->frame=frame->real_frame_id;return reset?FG_LINUX_WARMUP:FG_LINUX_GENERATED;
}
static uint64_t renderer_present_count(void){return real_frame;}
static uint64_t vk_pending_seq(void){return real_frame;}
static uint64_t vk_completed_seq(void){return 0;}
static VkCommandBuffer command=(VkCommandBuffer)(uintptr_t)0x55;
static void vk_full_barrier(VkCommandBuffer cb){assert(cb==command);}
VKAPI_ATTR void VKAPI_CALL vkCmdCopyImage(VkCommandBuffer cb,VkImage src,VkImageLayout sl,VkImage dst,VkImageLayout dl,uint32_t n,const VkImageCopy *copy){
 assert(cb==command&&src&&dst&&src!=dst&&sl==VK_IMAGE_LAYOUT_GENERAL&&dl==VK_IMAGE_LAYOUT_GENERAL&&n==1);
 assert(copy->extent.width==g_vk.sc_extent.width&&copy->extent.height==g_vk.sc_extent.height);copies++;
}
static void img_make(Img *image,VkFormat format,uint32_t width,uint32_t height,VkImageUsageFlags usage){
 assert(width&&height&&usage);if(image->b.image&&image->b.format==format&&image->b.width==width&&image->b.height==height)return;
 image->b.image=(VkImage)next_image++;image->b.width=width;image->b.height=height;image->b.format=format;image->v=(VkImageView)next_image++;
}
#include "../../runtime/nvn/vulkan/temporal_fg_linux.inc"
static TmpCamera camera;
static const float jitter[2]={.125f,-.25f};
static void reset(void){
 memset(&g_settings,0,sizeof g_settings);g_settings.upscaler=UPSCALE_DLSS;g_settings.frame_gen=true;g_settings.upscale_quality=UQ_QUALITY;
 memset(&provider,0,sizeof provider);memset(&linux_fg_frame,0,sizeof linux_fg_frame);memset(img,0,sizeof img);
 memset(&linux_fg_real,0,sizeof linux_fg_real);memset(&linux_fg_output,0,sizeof linux_fg_output);memset(&linux_fg_hud,0,sizeof linux_fg_hud);
 linux_fg=NULL;linux_fg_tried=linux_fg_pending=linux_fg_hudless=linux_fg_generated=linux_fg_error_logged=linux_fg_rect_valid=false;
 memset(linux_fg_rect,0,sizeof linux_fg_rect);linux_fg_quality=0;
 cutscene=deterministic=fail_dispatch=no_hud=tmp_off=false;provider_present=true;calls=opens=resource_queries=errors=copies=0;next_image=0x800;real_frame=1;
 g_vk.phys=(VkPhysicalDevice)(uintptr_t)0x11;g_vk.dev=(VkDevice)(uintptr_t)0x22;g_vk.sc_extent=(VkExtent2D){64,32};g_vk.sc_format=VK_FORMAT_R8G8B8A8_UNORM;
 img[2].b.image=(VkImage)(uintptr_t)0x100;img[2].b.format=VK_FORMAT_R32_SFLOAT;img[2].b.width=32;img[2].b.height=16;
 img[1].b.image=(VkImage)(uintptr_t)0x101;img[1].b.format=VK_FORMAT_R16G16_SFLOAT;img[1].b.width=32;img[1].b.height=16;
 memset(&camera,0,sizeof camera);camera.proj[11]=-.2f;camera.proj[5]=2.f;camera.ok=true;unsetenv("XCDE_PRECOMPILING");
}
static void prepare_rect(float x,float y,float width,float height){linux_fg_prepare(&camera,jitter,32,16,16.f,false);tmp_fg_set_rect(x,y,width,height);}
static void prepare(void){prepare_rect(0,0,64,32);}
static bool record(void){VkImage base=VK_NULL_HANDLE;float ms=0;bool ok=tmp_fg_record_present(command,(VkImage)(uintptr_t)0x700,&base,&ms);
 if(ok)assert(base&&ms==16.f);else assert(!base&&ms==0);return ok;}
int main(void){
 unsigned cases=0;
 reset();prepare();assert(tmp_fg_hudless_target());assert(!record()&&calls==1&&observed.reset&&observed.hudless.image&&linux_fg_rect_valid&&!tmp_fg_active());cases++;
 real_frame++;prepare();assert(tmp_fg_hudless_target());assert(record()&&calls==2&&!observed.reset&&tmp_fg_active());cases++;
 real_frame++;prepare_rect(4,2,56,28);assert(!record()&&observed.reset&&linux_fg_rect_valid&&!tmp_fg_active());cases++;
 real_frame++;prepare_rect(4,2,56,28);assert(record()&&!observed.reset&&tmp_fg_active());cases++;
 real_frame++;prepare();tmp_fg_set_rect(NAN,0,64,32);unsigned previous=calls;assert(!record()&&calls==previous&&!tmp_fg_active());cases++;
 real_frame++;prepare();assert(!record()&&observed.reset);cases++;
 tmp_fg_idle(command);assert(!tmp_fg_active()&&!linux_fg_rect_valid);real_frame++;prepare();assert(!record()&&observed.reset);cases++;
 real_frame++;prepare();g_settings.upscale_quality=UQ_PERFORMANCE;previous=calls;assert(!record()&&calls==previous&&!tmp_fg_active());cases++;
 reset();g_settings.frame_gen=false;g_settings.cutscene_mode=2;prepare();assert(!record()&&calls==0&&!tmp_fg_active());cases++;
 cutscene=true;real_frame++;prepare();assert(!record());real_frame++;prepare();assert(record());cutscene=false;real_frame++;prepare();previous=calls;assert(!record()&&calls==previous&&!tmp_fg_active());cases++;
 reset();deterministic=true;prepare();assert(!record()&&!calls&&!tmp_fg_active());cases++;
 reset();tmp_off=true;prepare();assert(!record()&&!calls&&!tmp_fg_active());cases++;
 reset();g_settings.upscaler=UPSCALE_FSR1;prepare();assert(!record()&&!calls&&!tmp_fg_active());cases++;
 reset();linux_fg_prepare(&camera,jitter,32,16,NAN,false);assert(!record()&&!calls);linux_fg_prepare(&camera,jitter,32,16,101.f,false);assert(!record()&&!calls);cases++;
 reset();fail_dispatch=true;prepare();assert(!record()&&errors==1);real_frame++;prepare();assert(!record()&&errors==1);cases++;
 g_settings.frame_gen=false;prepare();assert(!record());fail_dispatch=false;g_settings.frame_gen=true;real_frame++;prepare();assert(!record()&&errors==1);real_frame++;prepare();assert(record()&&errors==1);cases++;
 reset();prepare();real_frame++;assert(!record()&&!calls&&!tmp_fg_active());cases++;
 reset();provider_present=false;assert(!tmp_fg_supported());assert(!tmp_fg_supported());assert(!opens&&resource_queries==1);cases++;
 reset();setenv("XCDE_PRECOMPILING","1",1);prepare();assert(!record()&&!calls&&!opens);unsetenv("XCDE_PRECOMPILING");cases++;
 reset();no_hud=true;prepare();assert(!tmp_fg_hudless_target());assert(!record()&&!observed.hudless.image);cases++;
 reset();prepare();tmp_fg_set_rect(1000,1000,1,1);assert(!record()&&!calls&&!tmp_fg_active());cases++;
 printf("actual Linux temporal frame-generation adapter: %u cases passed; authored API/image mocks, no SDK/GPU\n",cases);
 return 0;
}
