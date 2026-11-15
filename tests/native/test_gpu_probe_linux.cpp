// [XCDE] LINUX-103: run the production headless query with Vulkan/loader mocks.
#include <SDL3/SDL.h>
#include <vulkan/vulkan.h>
#include <cassert>
#include <cstring>
static SDL_SharedObject *fixture_load(const char *);
static SDL_FunctionPointer fixture_function(SDL_SharedObject *,const char *);
static void fixture_unload(SDL_SharedObject *);
#define SDL_LoadObject fixture_load
#define SDL_LoadFunction fixture_function
#define SDL_UnloadObject fixture_unload
#include "../../Monado_Linux/tools/launcher/linux_gpu_probe.h"
#undef SDL_LoadObject
#undef SDL_LoadFunction
#undef SDL_UnloadObject
static unsigned loaded,unloaded,created,destroyed,mode;
static SDL_SharedObject *fixture_load(const char *name){assert(!strcmp(name,"libvulkan.so.1"));loaded++;return mode==4?nullptr:(SDL_SharedObject*)1;}
static void fixture_unload(SDL_SharedObject *p){assert(p==(SDL_SharedObject*)1);unloaded++;}
static VkResult VKAPI_CALL version(uint32_t *v){*v=mode==3?VK_API_VERSION_1_2:VK_API_VERSION_1_3;return VK_SUCCESS;}
static VkResult VKAPI_CALL create(const VkInstanceCreateInfo *ci,const VkAllocationCallbacks *,VkInstance *i){assert(ci->pApplicationInfo->apiVersion==VK_API_VERSION_1_3 && !ci->enabledExtensionCount && !ci->enabledLayerCount);if(mode==2)return VK_ERROR_INCOMPATIBLE_DRIVER;created++;*i=(VkInstance)1;return VK_SUCCESS;}
static void VKAPI_CALL destroy(VkInstance i,const VkAllocationCallbacks *){assert(i==(VkInstance)1);destroyed++;}
static VkResult VKAPI_CALL enumerate(VkInstance i,uint32_t *count,VkPhysicalDevice *devices){assert(i==(VkInstance)1);if(!devices){*count=2;return VK_SUCCESS;}assert(*count>=2);devices[0]=(VkPhysicalDevice)1;devices[1]=(VkPhysicalDevice)2;*count=2;return VK_SUCCESS;}
static void VKAPI_CALL properties(VkPhysicalDevice device,VkPhysicalDeviceProperties2 *p){
 unsigned index=(unsigned)(uintptr_t)device;assert(p->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2);
 strcpy(p->properties.deviceName,index==1?"Fixture integrated":"Fixture discrete");p->properties.vendorID=index==1?0x8086:0x10de;p->properties.apiVersion=VK_API_VERSION_1_3;p->properties.deviceType=index==1?VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;p->properties.limits.maxStorageBufferRange=mode==1 && index==2?0x80000000u:0xffffffffu;
 auto id=(VkPhysicalDeviceIDProperties*)p->pNext;assert(id && id->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES);id->deviceUUID[15]=(uint8_t)index;
 auto driver=(VkPhysicalDeviceDriverProperties*)id->pNext;assert(driver && driver->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES);strcpy(driver->driverName,"Fixture driver");
}
static void VKAPI_CALL features(VkPhysicalDevice,VkPhysicalDeviceFeatures2 *f){
 f->features.samplerAnisotropy=true;auto f11=(VkPhysicalDeviceVulkan11Features*)f->pNext;f11->storageBuffer16BitAccess=true;
 auto f12=(VkPhysicalDeviceVulkan12Features*)f11->pNext;f12->timelineSemaphore=f12->shaderInt8=f12->samplerMirrorClampToEdge=f12->uniformAndStorageBuffer8BitAccess=f12->storageBuffer8BitAccess=f12->scalarBlockLayout=f12->shaderFloat16=true;
 auto f13=(VkPhysicalDeviceVulkan13Features*)f12->pNext;f13->dynamicRendering=f13->synchronization2=f13->shaderDemoteToHelperInvocation=f13->shaderTerminateInvocation=f13->maintenance4=f13->pipelineCreationCacheControl=true;
}
static void VKAPI_CALL queues(VkPhysicalDevice,uint32_t *n,VkQueueFamilyProperties *p){if(!p){*n=1;return;}assert(*n>=1);p[0].queueCount=1;p[0].queueFlags=VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT;*n=1;}
static void VKAPI_CALL memory(VkPhysicalDevice,VkPhysicalDeviceMemoryProperties *p){p->memoryHeapCount=1;p->memoryHeaps[0].flags=VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;p->memoryHeaps[0].size=8ull<<30;}
static VkResult VKAPI_CALL extensions(VkPhysicalDevice,const char *,uint32_t *n,VkExtensionProperties *e){if(!e){*n=2;return VK_SUCCESS;}assert(*n>=2);strcpy(e[0].extensionName,VK_KHR_SWAPCHAIN_EXTENSION_NAME);strcpy(e[1].extensionName,VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);*n=2;return VK_SUCCESS;}
static PFN_vkVoidFunction VKAPI_CALL get(VkInstance,const char *name){
 if(mode==5 && !strcmp(name,"vkGetPhysicalDeviceFeatures2"))return nullptr;
#define PROC(n,f) if(!strcmp(name,n))return (PFN_vkVoidFunction)f
 PROC("vkCreateInstance",create);PROC("vkEnumerateInstanceVersion",version);PROC("vkDestroyInstance",destroy);PROC("vkEnumeratePhysicalDevices",enumerate);PROC("vkGetPhysicalDeviceProperties2",properties);PROC("vkGetPhysicalDeviceFeatures2",features);PROC("vkGetPhysicalDeviceQueueFamilyProperties",queues);PROC("vkGetPhysicalDeviceMemoryProperties",memory);PROC("vkEnumerateDeviceExtensionProperties",extensions);
#undef PROC
 return nullptr;
}
static SDL_FunctionPointer fixture_function(SDL_SharedObject *,const char *name){assert(!strcmp(name,"vkGetInstanceProcAddr"));return (SDL_FunctionPointer)get;}
int main(){
 launcher_gpu::Result r;assert(!SDL_WasInit(0));launcher_gpu::probe(&r);assert(!*r.error && r.count==2 && r.adapters[0].eligible && r.adapters[1].eligible);
 assert(!strcmp(r.adapters[0].uuid,"00000000000000000000000000000001") && r.adapters[1].local_heap==(8ull<<30));assert(created==1 && destroyed==1 && loaded==1 && unloaded==1);
 mode=1;launcher_gpu::probe(&r);assert(!r.adapters[1].eligible && strstr(r.adapters[1].reason,"complete guest heap") && r.adapters[0].eligible);assert(created==2 && destroyed==2);
 mode=2;launcher_gpu::probe(&r);assert(*r.error && r.count==0 && created==2 && destroyed==2);
 mode=3;launcher_gpu::probe(&r);assert(strstr(r.error,"1.3") && created==2 && destroyed==2);
 mode=5;launcher_gpu::probe(&r);assert(*r.error && created==3 && destroyed==3);
 mode=4;launcher_gpu::probe(&r);assert(*r.error && loaded==6 && unloaded==5);assert(!SDL_WasInit(0));
 puts("GPU actual headless probe capabilities/UUIDs/driver limits/error cleanup/no device/window PASS");
}
