// Production C glue exercised by the authored offscreen pipeline fixture.
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include "../../runtime/nvn/vulkan/vk.h"
#include "../../runtime/nvn/vulkan/temporal.h"
#include "../../runtime/nvn/vulkan/fsr4_policy.h"
#include "../../runtime/nvn/vulkan/fsr4_api.h"
#include "../../runtime/paths.h"
#include "../../runtime/settings.h"
VkG g_vk;Settings g_settings;
typedef struct {TexBackend b;VkImageView v;}Img;
enum {F_LINEAR=VK_FORMAT_R16G16B16A16_SFLOAT,F_MV=VK_FORMAT_R16G16_SFLOAT,F_DEPTH=VK_FORMAT_R32_SFLOAT};
static char component[4096];static uint64_t fixture_pending,fixture_completed;
void rt_fatal_trace(void){abort();}
uint64_t vk_pending_seq(void){return fixture_pending;}uint64_t vk_completed_seq(void){return fixture_completed;}
void vk_wait_seq(uint64_t seq){assert(seq<=fixture_completed);}
bool rt_resource(const char *name,const char *relative,char *out,size_t size){assert(!strcmp(name,"libmonado-fsr4.so")&&!relative);return snprintf(out,size,"%s",component)>0 && strlen(component)<size;}
#include "../../runtime/nvn/vulkan/temporal_fsr4_linux.inc"
static int active_provider=-1,active_upscaler=-1,active_quality=-1;static unsigned jit_index,jit_phases;static uint64_t snap_frame;
static void history_invalidate(void){}
static bool diag_deterministic(void){return false;}
static int env_int(const char *name){(void)name;return 0;}
static bool ffx_load(void){return false;}
static bool dlss_available(void){return false;}
// GENERATED_TEMPORAL_SELECTION
// GENERATED_DEVICE_PREPARATION
int fsr4_bridge_init(VkPhysicalDevice physical,VkDevice device,const char *library,const char *assets){
 assert(strlen(library)<sizeof component);snprintf(component,sizeof component,"%s",library);g_vk.phys=physical;g_vk.dev=device;vkGetPhysicalDeviceProperties(physical,&g_vk.props);
 VkPhysicalDeviceVulkan13Features f13={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};VkPhysicalDeviceVulkan12Features f12={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,.pNext=&f13};VkPhysicalDeviceFeatures2 have={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,.pNext=&f12};vkGetPhysicalDeviceFeatures2(physical,&have);
 uint32_t n=0;assert(vkEnumerateDeviceExtensionProperties(physical,NULL,&n,NULL)==VK_SUCCESS);VkExtensionProperties *extensions=calloc(n,sizeof *extensions);assert(extensions);assert(vkEnumerateDeviceExtensionProperties(physical,NULL,&n,extensions)==VK_SUCCESS);bool push=false;for(unsigned i=0;i<n;i++)push|=!strcmp(extensions[i].extensionName,VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);free(extensions);const char *names[2];uint32_t count=0;VkPhysicalDeviceVulkan13Features enabled={0};fsr4_device_prepare(&have,&f12,&f13,push,names,&count,&enabled);
 g_settings.upscaler=UPSCALE_FSR4_LITE;return g_vk.fsr4_lite && enabled.shaderIntegerDotProduct && count==1 && tmp_fsr4_set_asset_dir(assets) && tmp_fsr4_supported();
}
int fsr4_bridge_record(const MonadoFsr4Frame *frame,uint64_t pending,uint64_t completed){
 fixture_pending=pending;fixture_completed=completed;g_vk.sc_extent=(VkExtent2D){frame->output.width,frame->output.height};g_settings.upscale_quality=frame->ultra?UQ_ULTRA:UQ_NATIVE;g_settings.upscale_sharpness=frame->sharpness;
 if(!tmp_enabled() || active_provider!=3 || !tmp_linear_output())return 0;
 const MonadoFsr4Image *inputs[]={&frame->color,&frame->depth,&frame->motion,&frame->output};VkFormat formats[]={F_LINEAR,F_DEPTH,F_MV,F_LINEAR};Img images[4];memset(images,0,sizeof images);
 for(unsigned i=0;i<4;i++){images[i].b.image=inputs[i]->image;images[i].b.width=inputs[i]->width;images[i].b.height=inputs[i]->height;images[i].b.format=formats[i];images[i].v=inputs[i]->view;}
 return fsr4_evaluate(frame->cmd,&images[0],&images[2],&images[1],&images[3],frame->jitter,frame->reset!=0);
}
const char *fsr4_bridge_status(void){return tmp_fsr4_status();}
void fsr4_bridge_reset(void){fsr4_reset();}
