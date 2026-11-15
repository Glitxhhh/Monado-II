// [XCDE] WINPARITY-105: actual production initialization block, authored handles.
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <vulkan/vulkan.h>
#define FATAL(...) abort()
static void *window = (void *)(uintptr_t)42;
static struct {VkInstance inst;VkPhysicalDevice phys;VkDevice dev;VkSurfaceKHR surface;uint32_t qfam;} g_vk;
static unsigned step, mode, destroyed;
static const char *SDL_GetError(void){return "authored failure";}
static void create_instance(void){assert(step==0);g_vk.inst=(VkInstance)(uintptr_t)9;step=1;}
static bool SDL_Vulkan_CreateSurface(void *w,VkInstance instance,void *allocator,VkSurfaceKHR *surface){
    assert(step==1&&w==window&&instance==g_vk.inst&&!allocator);*surface=(VkSurfaceKHR)(uintptr_t)41;step=2;return true;
}
static void pick_device(void){assert(step==2);g_vk.phys=(VkPhysicalDevice)(uintptr_t)17;g_vk.qfam=3;step=3;}
static void dlss_validate_adapter(VkPhysicalDevice physical,uint32_t api,uint32_t family){assert(step==3&&physical==g_vk.phys&&api==VK_API_VERSION_1_3&&family==3);step=4;}
static void create_device(void){assert(step==4);g_vk.dev=(VkDevice)(uintptr_t)19;step=5;}
static bool dlss_create_surface(void *w,VkInstance instance,VkPhysicalDevice physical,uint32_t family,VkSurfaceKHR *surface){
    assert(step==5&&w==window&&instance==g_vk.inst&&physical==g_vk.phys&&family==3&&g_vk.dev);
    assert(!*surface&&g_vk.surface==(VkSurfaceKHR)(uintptr_t)41);step=6;
    if(!mode)return false;
    *surface=(VkSurfaceKHR)(uintptr_t)(mode==1?77:41);return true;
}
VKAPI_ATTR void VKAPI_CALL vkDestroySurfaceKHR(VkInstance instance,VkSurfaceKHR surface,const VkAllocationCallbacks *allocator){
    assert(step==6&&mode==1&&instance==g_vk.inst&&surface==(VkSurfaceKHR)(uintptr_t)41&&!allocator);
    assert(g_vk.surface==(VkSurfaceKHR)(uintptr_t)77);destroyed++;
}
static void sc_proxies(void){assert(step==6);assert(g_vk.surface==(VkSurfaceKHR)(uintptr_t)(mode==1?77:41));step=7;}
static void create_swapchain(void){assert(step==7);step=8;}
static void production_boot(void){
// PRODUCTION_DEVICE_INITIALIZATION
}
int main(void){
    for(mode=0;mode<3;mode++){step=destroyed=0;g_vk=(typeof(g_vk)){0};production_boot();assert(step==8&&destroyed==(mode==1));}
    puts("PASS production selected-adapter validation before device, hooked surface before proxies/swapchain, ordinary-surface fallback/ownership");
}
