// [XCDE] LINUX-103: execute actual pick_device with deterministic Vulkan queries.
#define _GNU_SOURCE
#include <assert.h>
#include <stdint.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vulkan/vulkan.h>
static VkResult fixture_enumerate(VkInstance,uint32_t *,VkPhysicalDevice *);
static void fixture_properties(VkPhysicalDevice,VkPhysicalDeviceProperties *);
static void fixture_properties2(VkPhysicalDevice,VkPhysicalDeviceProperties2 *);
static void fixture_features2(VkPhysicalDevice,VkPhysicalDeviceFeatures2 *);
static void fixture_queues(VkPhysicalDevice,uint32_t *,VkQueueFamilyProperties *);
static VkResult fixture_surface(VkPhysicalDevice,uint32_t,VkSurfaceKHR,VkBool32 *);
static VkResult fixture_extensions(VkPhysicalDevice,const char *,uint32_t *,VkExtensionProperties *);
#define vkEnumeratePhysicalDevices fixture_enumerate
#define vkGetPhysicalDeviceProperties fixture_properties
#define vkGetPhysicalDeviceProperties2 fixture_properties2
#define vkGetPhysicalDeviceFeatures2 fixture_features2
#define vkGetPhysicalDeviceQueueFamilyProperties fixture_queues
#define vkGetPhysicalDeviceSurfaceSupportKHR fixture_surface
#define vkEnumerateDeviceExtensionProperties fixture_extensions
#include "../../Monado_Linux/runtime/nvn/vulkan/device.c"
#undef vkEnumeratePhysicalDevices
#undef vkGetPhysicalDeviceProperties
#undef vkGetPhysicalDeviceProperties2
#undef vkGetPhysicalDeviceFeatures2
#undef vkGetPhysicalDeviceQueueFamilyProperties
#undef vkGetPhysicalDeviceSurfaceSupportKHR
#undef vkEnumerateDeviceExtensionProperties
#undef getenv
Settings g_settings;
static unsigned scenario;
void rt_fatal_trace(void){_Exit(100);}
static unsigned id(VkPhysicalDevice p){return (unsigned)(uintptr_t)p;}
static VkResult fixture_enumerate(VkInstance i,uint32_t *n,VkPhysicalDevice *p){
 assert(i==(VkInstance)1);if(scenario==14)return VK_ERROR_INITIALIZATION_FAILED;
 if(!p){*n=scenario==13?0:3;return VK_SUCCESS;}assert(*n>=3);
 p[0]=(VkPhysicalDevice)(uintptr_t)(scenario==2?2:1);p[1]=(VkPhysicalDevice)(uintptr_t)(scenario==2?1:2);p[2]=(VkPhysicalDevice)(uintptr_t)3;*n=3;return VK_SUCCESS;
}
static void fixture_properties(VkPhysicalDevice p,VkPhysicalDeviceProperties *v){
 memset(v,0,sizeof *v);unsigned index=id(p);snprintf(v->deviceName,sizeof v->deviceName,"Fixture GPU%u",index);
 v->apiVersion=index==3 || scenario==15 || (scenario==5 && index==2)?VK_API_VERSION_1_2:VK_API_VERSION_1_3;
 v->deviceType=index==2?VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:index==1?VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:VK_PHYSICAL_DEVICE_TYPE_CPU;
 v->limits.maxStorageBufferRange=((scenario==6 || scenario==7)&&index==2)?0x80000000:0xffffffffu;
}
static void fixture_properties2(VkPhysicalDevice p,VkPhysicalDeviceProperties2 *v){
 fixture_properties(p,&v->properties);if(v->pNext){VkPhysicalDeviceIDProperties *u=v->pNext;assert(u->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES);u->deviceUUID[15]=(uint8_t)id(p);}
}
static void fixture_features2(VkPhysicalDevice p,VkPhysicalDeviceFeatures2 *f){
 VkPhysicalDeviceVulkan11Features *f11=f->pNext;assert(f11 && f11->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES);f11->storageBuffer16BitAccess=true;
 VkPhysicalDeviceVulkan12Features *f12=f11->pNext;assert(f12 && f12->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES);f12->timelineSemaphore=f12->shaderInt8=f12->samplerMirrorClampToEdge=f12->uniformAndStorageBuffer8BitAccess=f12->storageBuffer8BitAccess=f12->scalarBlockLayout=true;
 VkPhysicalDeviceVulkan13Features *f13=f12->pNext;assert(f13 && f13->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES);f13->dynamicRendering=f13->synchronization2=f13->shaderDemoteToHelperInvocation=f13->shaderTerminateInvocation=f13->maintenance4=f13->pipelineCreationCacheControl=true;
 if(scenario==8 && id(p)==2)f12->timelineSemaphore=false;
}
static void fixture_queues(VkPhysicalDevice p,uint32_t *n,VkQueueFamilyProperties *v){
 if(!v){*n=2;return;}assert(*n>=2);memset(v,0,2*sizeof *v);v[0].queueCount=v[1].queueCount=1;v[0].queueFlags=id(p)==2?VK_QUEUE_GRAPHICS_BIT:VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT;v[1].queueFlags=VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT;*n=2;
}
static VkResult fixture_surface(VkPhysicalDevice p,uint32_t family,VkSurfaceKHR surface,VkBool32 *present){
 assert(surface==(VkSurfaceKHR)(uintptr_t)2 && family<2);*present=!(scenario==16 || ((scenario==9 || scenario==10)&&id(p)==2));return scenario==10&&id(p)==2?VK_ERROR_SURFACE_LOST_KHR:VK_SUCCESS;
}
static VkResult fixture_extensions(VkPhysicalDevice p,const char *layer,uint32_t *n,VkExtensionProperties *e){
 assert(!layer);if(!e){*n=1;return VK_SUCCESS;}assert(*n>=1);snprintf(e[0].extensionName,sizeof e[0].extensionName,"%s",scenario==11&&id(p)==2?"VK_FIXTURE_no_swapchain":VK_KHR_SWAPCHAIN_EXTENSION_NAME);*n=1;return VK_SUCCESS;
}
static void run(void){
 unsetenv("LARECOMP_GPU");g_vk.inst=(VkInstance)1;g_vk.surface=(VkSurfaceKHR)(uintptr_t)2;
 unsigned expected=2;
 if(scenario==1 || scenario==2 || scenario==3 || scenario==4 || scenario==7 || scenario==12 || scenario==17){snprintf(g_settings.gpu_uuid,sizeof g_settings.gpu_uuid,"%032x",scenario==7?2:1);expected=1;}
 if(scenario==3)snprintf(g_settings.gpu_uuid,sizeof g_settings.gpu_uuid,"%032x",99),expected=2;
 if(scenario==4)setenv("LARECOMP_GPU","1",1),expected=2; // explicit valid legacy index wins
 if(scenario==5 || scenario==6 || scenario==8 || scenario==9 || scenario==10 || scenario==11)expected=1;
 if(scenario==12)setenv("LARECOMP_GPU","junk",1);
 if(scenario==17)setenv("LARECOMP_GPU","999",1);
 pick_device();assert(scenario!=13 && scenario!=14 && scenario!=15 && scenario!=16);
 assert(id(g_vk.phys)==expected && g_vk.qfam==(expected==2?1u:0u));
 _Exit(0);
}
int main(void){
 for(scenario=0;scenario<18;scenario++){
  pid_t child=fork();assert(child>=0);if(!child)run();int status;assert(waitpid(child,&status,0)==child && WIFEXITED(status));
  unsigned wanted=scenario>=13 && scenario<=16?100u:0u;
  if((unsigned)WEXITSTATUS(status)!=wanted){fprintf(stderr,"GPU scenario %u exit %d expected %u\n",scenario,WEXITSTATUS(status),wanted);return 1;}
 }
 puts("Actual renderer GPU selection18 cases: UUID/Auto/reorder/stale/API/features/heap/queue/surface/errors PASS");
}
