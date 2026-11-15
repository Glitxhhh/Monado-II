// Actual production instance creation, authored SDL/SDK/loader boundaries.
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
typedef uint32_t Uint32;
static unsigned mode,calls;
static struct {VkInstance inst;} g_vk;
static VkDebugUtilsMessengerEXT messenger;
#define LOG(...) ((void)0)
#define FATAL(...) abort()
static VKAPI_ATTR VkBool32 VKAPI_CALL debug_cb(VkDebugUtilsMessageSeverityFlagBitsEXT s,VkDebugUtilsMessageTypeFlagsEXT t,const VkDebugUtilsMessengerCallbackDataEXT *d,void *u){(void)s;(void)t;(void)d;(void)u;return VK_FALSE;}
static const char *const *SDL_Vulkan_GetInstanceExtensions(Uint32 *count){
    static const char *const names[]={"VK_KHR_surface","VK_KHR_win32_surface"};*count=2;return names;
}
static char *authored_env(const char *name){assert(!strcmp(name,"LARECOMP_VK_VALIDATION"));return mode?(char *)"1":NULL;}
int dlss_instance_exts(const char **out,int capacity){
    assert(capacity==128);
    static const char *const names[]={"VK_AUTHORED_0","VK_AUTHORED_1","VK_AUTHORED_2","VK_AUTHORED_3","VK_AUTHORED_4","VK_AUTHORED_5","VK_AUTHORED_6","VK_AUTHORED_7","VK_AUTHORED_8","VK_AUTHORED_9","VK_AUTHORED_10","VK_AUTHORED_11"};
    if(!mode){memcpy(out,names,sizeof names);return 12;}
    if(mode==3){out[0]="VK_AUTHORED_0";out[1]="VK_KHR_surface";return 2;}
    out[0]=VK_EXT_DEBUG_UTILS_EXTENSION_NAME;out[1]="VK_AUTHORED_0";out[2]="VK_KHR_surface";return 3;
}
static VkResult authored_create(const VkInstanceCreateInfo *ci,const VkAllocationCallbacks *allocator,VkInstance *instance){
    assert(!allocator&&ci->pApplicationInfo->apiVersion==VK_API_VERSION_1_3);
    bool debug=false,provider=false;
    for(uint32_t i=0;i<ci->enabledExtensionCount;i++){
        debug|=!strcmp(ci->ppEnabledExtensionNames[i],VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        provider|=!strcmp(ci->ppEnabledExtensionNames[i],"VK_AUTHORED_0");
        for(uint32_t j=0;j<i;j++)assert(strcmp(ci->ppEnabledExtensionNames[i],ci->ppEnabledExtensionNames[j]));
    }
    assert(provider);calls++;
    if(!mode)assert(calls==1&&ci->enabledExtensionCount==14&&!debug&&!ci->enabledLayerCount);
    else if((mode==1||mode==3)&&calls==2)assert(ci->enabledExtensionCount==(mode==1?4:3)&&debug==(mode==1)&&!ci->enabledLayerCount);
    else {
        assert(calls==1&&ci->enabledExtensionCount==4&&debug&&ci->enabledLayerCount==1);
        if(mode==1||mode==3)return VK_ERROR_LAYER_NOT_PRESENT;
    }
    *instance=(VkInstance)(uintptr_t)17;return VK_SUCCESS;
}
static PFN_vkVoidFunction authored_proc(VkInstance instance,const char *name){assert(instance==g_vk.inst&&!strcmp(name,"vkCreateDebugUtilsMessengerEXT"));return NULL;}
#define getenv authored_env
#define vkCreateInstance authored_create
#define vkGetInstanceProcAddr authored_proc
// PRODUCTION_CREATE_INSTANCE
int main(void){
    for(mode=0;mode<4;mode++){calls=0;g_vk.inst=VK_NULL_HANDLE;create_instance();assert(g_vk.inst==(VkInstance)(uintptr_t)17&&calls==((mode==1||mode==3)?2:1));}
    puts("PASS production instance complete provider extensions, deduplication and exact validation retry");
}
