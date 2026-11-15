// [XCDE] LINUX-103: read-only loader/physical-device probe, without a device/window.
#pragma once
#include <SDL3/SDL.h>
#include <vulkan/vulkan.h>
#include "../../runtime/gpu_selection.h"
#include "../../runtime/gpu_requirements_vk.h"
#include "../../runtime/rt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
namespace launcher_gpu {
struct Adapter {
    char name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE],uuid[33],driver_name[VK_MAX_DRIVER_NAME_SIZE],reason[256];
    uint32_t vendor,device,api,driver,type,max_storage_range;
    uint64_t local_heap;
    bool eligible,graphics_compute,swapchain,anisotropy,float16,int8,feedback,memory_budget;
};
struct Result { Adapter adapters[32];uint32_t count;char error[512];bool truncated; };
static inline bool extension(const VkExtensionProperties *properties,uint32_t count,const char *name) {
    for(uint32_t i=0;i<count;i++)if(!strcmp(properties[i].extensionName,name))return true;
    return false;
}
static inline void probe(Result *out) {
    if(!out)return;memset(out,0,sizeof *out);
    SDL_SharedObject *library=SDL_LoadObject("libvulkan.so.1");
    if(!library){snprintf(out->error,sizeof out->error,"Vulkan loader unavailable: %.420s",SDL_GetError());return;}
    PFN_vkGetInstanceProcAddr get=(PFN_vkGetInstanceProcAddr)SDL_LoadFunction(library,"vkGetInstanceProcAddr");
    PFN_vkCreateInstance create=get?(PFN_vkCreateInstance)get(VK_NULL_HANDLE,"vkCreateInstance"):nullptr;
    PFN_vkEnumerateInstanceVersion version=get?(PFN_vkEnumerateInstanceVersion)get(VK_NULL_HANDLE,"vkEnumerateInstanceVersion"):nullptr;
    uint32_t api=VK_API_VERSION_1_0;if(version)version(&api);
    if(!create || api<VK_API_VERSION_1_3){snprintf(out->error,sizeof out->error,"A Vulkan 1.3 loader/driver is required (found %u.%u).",VK_API_VERSION_MAJOR(api),VK_API_VERSION_MINOR(api));SDL_UnloadObject(library);return;}
    VkApplicationInfo app{};app.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO;app.pApplicationName="Monado II capability probe";app.apiVersion=VK_API_VERSION_1_3;
    VkInstanceCreateInfo ci{};ci.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;ci.pApplicationInfo=&app;
    VkInstance instance=VK_NULL_HANDLE;VkResult result=create(&ci,nullptr,&instance);
    if(result!=VK_SUCCESS){snprintf(out->error,sizeof out->error,"Vulkan capability probe could not create an instance (%d).",result);SDL_UnloadObject(library);return;}
    PFN_vkDestroyInstance destroy=(PFN_vkDestroyInstance)get(instance,"vkDestroyInstance");
    PFN_vkEnumeratePhysicalDevices enumerate=(PFN_vkEnumeratePhysicalDevices)get(instance,"vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceProperties2 props=(PFN_vkGetPhysicalDeviceProperties2)get(instance,"vkGetPhysicalDeviceProperties2");
    PFN_vkGetPhysicalDeviceFeatures2 features=(PFN_vkGetPhysicalDeviceFeatures2)get(instance,"vkGetPhysicalDeviceFeatures2");
    PFN_vkGetPhysicalDeviceQueueFamilyProperties queues=(PFN_vkGetPhysicalDeviceQueueFamilyProperties)get(instance,"vkGetPhysicalDeviceQueueFamilyProperties");
    PFN_vkGetPhysicalDeviceMemoryProperties memory=(PFN_vkGetPhysicalDeviceMemoryProperties)get(instance,"vkGetPhysicalDeviceMemoryProperties");
    PFN_vkEnumerateDeviceExtensionProperties extensions=(PFN_vkEnumerateDeviceExtensionProperties)get(instance,"vkEnumerateDeviceExtensionProperties");
    if(!destroy || !enumerate || !props || !features || !queues || !memory || !extensions) {
        snprintf(out->error,sizeof out->error,"Vulkan driver did not expose the required query functions.");
        if(destroy)destroy(instance,nullptr);SDL_UnloadObject(library);return;
    }
    uint32_t count=0;result=enumerate(instance,&count,nullptr);
    if(result!=VK_SUCCESS || !count){snprintf(out->error,sizeof out->error,"No Vulkan physical devices found (%d).",result);destroy(instance,nullptr);SDL_UnloadObject(library);return;}
    if(count>32){out->truncated=true;count=32;}
    VkPhysicalDevice devices[32];result=enumerate(instance,&count,devices);
    if(result!=VK_SUCCESS && result!=VK_INCOMPLETE){snprintf(out->error,sizeof out->error,"Could not enumerate Vulkan devices (%d).",result);destroy(instance,nullptr);SDL_UnloadObject(library);return;}
    out->count=count>32?32:count;
    for(uint32_t i=0;i<out->count;i++) {
        Adapter &a=out->adapters[i];
        VkPhysicalDeviceDriverProperties driver{};driver.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
        VkPhysicalDeviceIDProperties id{};id.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;id.pNext=&driver;
        VkPhysicalDeviceProperties2 p{};p.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;p.pNext=&id;props(devices[i],&p);
        snprintf(a.name,sizeof a.name,"%s",p.properties.deviceName);monado_gpu_uuid_format(id.deviceUUID,a.uuid);
        snprintf(a.driver_name,sizeof a.driver_name,"%s",driver.driverName);
        a.vendor=p.properties.vendorID;a.device=p.properties.deviceID;a.api=p.properties.apiVersion;a.driver=p.properties.driverVersion;a.type=p.properties.deviceType;a.max_storage_range=p.properties.limits.maxStorageBufferRange;
        VkPhysicalDeviceVulkan13Features f13{};f13.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
        VkPhysicalDeviceVulkan12Features f12{};f12.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;f12.pNext=&f13;
        VkPhysicalDeviceVulkan11Features f11{};f11.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;f11.pNext=&f12;
        VkPhysicalDeviceFeatures2 f{};f.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;f.pNext=&f11;features(devices[i],&f);
        a.anisotropy=f.features.samplerAnisotropy;a.float16=f12.shaderFloat16;a.int8=f12.shaderInt8;
        uint32_t nq=0;queues(devices[i],&nq,nullptr);
        if(nq && nq<=128){VkQueueFamilyProperties q[128];queues(devices[i],&nq,q);for(uint32_t j=0;j<nq && j<128;j++)if(q[j].queueCount && (q[j].queueFlags&(VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT))==(VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT))a.graphics_compute=true;}
        VkPhysicalDeviceMemoryProperties m{};memory(devices[i],&m);for(uint32_t j=0;j<m.memoryHeapCount && j<VK_MAX_MEMORY_HEAPS;j++)if(m.memoryHeaps[j].flags&VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)a.local_heap+=m.memoryHeaps[j].size;
        uint32_t ne=0;result=extensions(devices[i],nullptr,&ne,nullptr);
        if(result==VK_SUCCESS && ne<=4096) {
            VkExtensionProperties *e=(VkExtensionProperties*)calloc(ne?ne:1,sizeof *e);
            if(e){if(extensions(devices[i],nullptr,&ne,e)==VK_SUCCESS){a.swapchain=extension(e,ne,VK_KHR_SWAPCHAIN_EXTENSION_NAME);a.feedback=extension(e,ne,"VK_EXT_attachment_feedback_loop_layout");a.memory_budget=extension(e,ne,VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);}free(e);}
        }
        a.eligible=a.api>=VK_API_VERSION_1_3 && a.graphics_compute && a.swapchain && monado_gpu_required_features(&f11,&f12,&f13) && a.max_storage_range>=HEAP_MAX;
        if(a.api<VK_API_VERSION_1_3)snprintf(a.reason,sizeof a.reason,"Requires Vulkan 1.3.");
        else if(!a.graphics_compute)snprintf(a.reason,sizeof a.reason,"No combined graphics/compute queue.");
        else if(!a.swapchain)snprintf(a.reason,sizeof a.reason,"Swapchain extension unavailable.");
        else if(!monado_gpu_required_features(&f11,&f12,&f13))snprintf(a.reason,sizeof a.reason,"Required renderer features are unavailable.");
        else if(a.max_storage_range<HEAP_MAX)snprintf(a.reason,sizeof a.reason,"Storage-buffer range %u MiB is smaller than the complete guest heap (%llu MiB).",a.max_storage_range>>20,(unsigned long long)(HEAP_MAX>>20));
        else snprintf(a.reason,sizeof a.reason,"Base requirements available; presentation and optional providers are checked when starting the game.");
    }
    destroy(instance,nullptr);SDL_UnloadObject(library);
}
} // namespace launcher_gpu
