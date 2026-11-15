// [XCDE] LINUX-099: authored MIT Vulkan presentation observer.
// This explicit layer belongs below NVIDIA's implicit present layer. It never
// changes images, waits, semaphores, queues or present results. Accepted output
// requests are measured, not physical scanout or inferred generated frames.
#define _POSIX_C_SOURCE 200809L
#include "present_observer_linux.h"
#include <vulkan/vk_layer.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define OBS_EXPORT __attribute__((visibility("default")))
#define OBS_WINDOW_NS UINT64_C(500000000)
#define OBS_EVENTS 4096u

typedef struct ObserverInstance {
    void *key; VkInstance handle; PFN_vkGetInstanceProcAddr get;
    PFN_vkDestroyInstance destroy; struct ObserverInstance *next;
} ObserverInstance;
typedef struct ObserverSwapchain { VkSwapchainKHR handle; struct ObserverSwapchain *next; } ObserverSwapchain;
typedef struct ObserverDevice {
    void *key; VkDevice handle; PFN_vkGetDeviceProcAddr get;
    PFN_vkQueuePresentKHR present; PFN_vkCreateSwapchainKHR create_swapchain;
    PFN_vkDestroySwapchainKHR destroy_swapchain; PFN_vkDestroyDevice destroy;
    ObserverSwapchain *swapchains; struct ObserverDevice *next;
} ObserverDevice;
static pthread_mutex_t observer_lock = PTHREAD_MUTEX_INITIALIZER;
static ObserverInstance *instances;
static ObserverDevice *devices;
static PFN_vkGetInstanceProcAddr global_next;
static uint64_t accepted, failed, first_ns, last_ns, evicted_ns, events[OBS_EVENTS];
static uint32_t write_at, event_count, active_swapchains;
static bool bookkeeping_fault;

static void *dispatch_key(const void *handle) { return handle ? *(void *const *)handle : NULL; }
static ObserverInstance *instance_locked(const void *handle) {
    void *key = dispatch_key(handle);
    for (ObserverInstance *c = instances; c; c = c->next) if (c->key == key) return c;
    return NULL;
}
static ObserverDevice *device_locked(const void *handle) {
    void *key = dispatch_key(handle);
    for (ObserverDevice *c = devices; c; c = c->next) if (c->key == key) return c;
    return NULL;
}
static uint64_t observer_now(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) || t.tv_sec < 0) return 0;
    return (uint64_t)t.tv_sec * UINT64_C(1000000000) + (uint64_t)t.tv_nsec;
}
static void reset_window_locked(void) {
    first_ns = last_ns = evicted_ns = 0;
    write_at = event_count = 0;
}
static void accept_locked(uint64_t now) {
    if (accepted != UINT64_MAX) ++accepted;
    if (!now) { reset_window_locked(); return; }
    if (last_ns && (now < last_ns || now - last_ns > OBS_WINDOW_NS)) reset_window_locked();
    if (!first_ns) first_ns = now;
    if (event_count == OBS_EVENTS) evicted_ns = events[write_at];
    else ++event_count;
    events[write_at] = now;
    write_at = (write_at + 1) % OBS_EVENTS;
    last_ns = now;
}
static void present_result_locked(const VkPresentInfoKHR *info, VkResult overall, uint64_t now) {
    for (uint32_t i = 0; i < info->swapchainCount; ++i) {
        VkResult r = info->pResults ? info->pResults[i] : overall;
        if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) accept_locked(now);
        else if (failed != UINT64_MAX) ++failed;
    }
}
OBS_EXPORT int monado_present_observer_snapshot(uint64_t now, MonadoPresentObserverSnapshot *out, uint32_t bytes) {
    if (!out || bytes < sizeof *out) return 0;
    MonadoPresentObserverSnapshot s = {.abi = MONADO_PRESENT_OBSERVER_ABI,
        .bytes = sizeof s, .window_ns = OBS_WINDOW_NS};
    pthread_mutex_lock(&observer_lock);
    s.accepted = accepted; s.failed = failed; s.last_accepted_ns = last_ns;
    if (active_swapchains) s.flags |= MONADO_PRESENT_OBSERVER_ATTACHED;
    if (last_ns) s.flags |= MONADO_PRESENT_OBSERVER_SEEN;
    if (bookkeeping_fault) s.flags |= MONADO_PRESENT_OBSERVER_FAULT;
    if (now && first_ns && last_ns && now >= last_ns && now >= first_ns && now - first_ns >= OBS_WINDOW_NS && now - last_ns <= OBS_WINDOW_NS) {
        const uint64_t cutoff = now - OBS_WINDOW_NS;
        if (evicted_ns > cutoff) s.flags |= MONADO_PRESENT_OBSERVER_SATURATED;
        else if (active_swapchains && !bookkeeping_fault) {
            uint32_t n = 0;
            for (uint32_t j = 0; j < event_count; ++j) {
                uint32_t index = (write_at + OBS_EVENTS - 1 - j) % OBS_EVENTS;
                if (events[index] <= cutoff) break;
                if (events[index] <= now) ++n;
            }
            s.output_fps = (double)n * (1000000000.0 / (double)OBS_WINDOW_NS);
            s.flags |= MONADO_PRESENT_OBSERVER_READY;
        }
    }
    pthread_mutex_unlock(&observer_lock);
    *out = s;
    return 1;
}
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL observer_get_instance(VkInstance, const char *);
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL observer_get_device(VkDevice, const char *);
static VKAPI_ATTR VkResult VKAPI_CALL observer_create_instance(const VkInstanceCreateInfo *info, const VkAllocationCallbacks *allocator, VkInstance *result) {
    VkLayerInstanceCreateInfo *link = (VkLayerInstanceCreateInfo *)info->pNext;
    while (link && (link->sType != VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO || link->function != VK_LAYER_LINK_INFO)) link = (void *)link->pNext;
    if (!link || !link->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetInstanceProcAddr get = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkCreateInstance create = (PFN_vkCreateInstance)get(VK_NULL_HANDLE, "vkCreateInstance");
    if (!create) return VK_ERROR_INITIALIZATION_FAILED;
    ObserverInstance *c = calloc(1, sizeof *c);
    if (!c) return VK_ERROR_OUT_OF_HOST_MEMORY;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;
    VkResult r = create(info, allocator, result);
    if (r != VK_SUCCESS) { free(c); return r; }
    c->handle = *result; c->key = dispatch_key(*result); c->get = get;
    c->destroy = (PFN_vkDestroyInstance)get(*result, "vkDestroyInstance");
    pthread_mutex_lock(&observer_lock);
    global_next = get; c->next = instances; instances = c;
    pthread_mutex_unlock(&observer_lock);
    return r;
}
static VKAPI_ATTR void VKAPI_CALL observer_destroy_instance(VkInstance handle, const VkAllocationCallbacks *allocator) {
    pthread_mutex_lock(&observer_lock);
    ObserverInstance **p = &instances, *c = NULL;
    while (*p) { if ((*p)->handle == handle) { c = *p; *p = c->next; break; } p = &(*p)->next; }
    pthread_mutex_unlock(&observer_lock);
    if (c) { c->destroy(handle, allocator); free(c); }
}
static VKAPI_ATTR VkResult VKAPI_CALL observer_create_device(VkPhysicalDevice physical, const VkDeviceCreateInfo *info, const VkAllocationCallbacks *allocator, VkDevice *result) {
    VkLayerDeviceCreateInfo *link = (VkLayerDeviceCreateInfo *)info->pNext;
    while (link && (link->sType != VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO || link->function != VK_LAYER_LINK_INFO)) link = (void *)link->pNext;
    if (!link || !link->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;
    pthread_mutex_lock(&observer_lock);
    ObserverInstance *parent = instance_locked(physical);
    VkInstance instance = parent ? parent->handle : VK_NULL_HANDLE;
    pthread_mutex_unlock(&observer_lock);
    if (!instance) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetInstanceProcAddr get_instance = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr get = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    PFN_vkCreateDevice create = (PFN_vkCreateDevice)get_instance(instance, "vkCreateDevice");
    if (!create || !get) return VK_ERROR_INITIALIZATION_FAILED;
    ObserverDevice *c = calloc(1, sizeof *c);
    if (!c) return VK_ERROR_OUT_OF_HOST_MEMORY;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;
    VkResult r = create(physical, info, allocator, result);
    if (r != VK_SUCCESS) { free(c); return r; }
    c->handle = *result; c->key = dispatch_key(*result); c->get = get;
    c->present = (PFN_vkQueuePresentKHR)get(*result, "vkQueuePresentKHR");
    c->create_swapchain = (PFN_vkCreateSwapchainKHR)get(*result, "vkCreateSwapchainKHR");
    c->destroy_swapchain = (PFN_vkDestroySwapchainKHR)get(*result, "vkDestroySwapchainKHR");
    c->destroy = (PFN_vkDestroyDevice)get(*result, "vkDestroyDevice");
    pthread_mutex_lock(&observer_lock); c->next = devices; devices = c; pthread_mutex_unlock(&observer_lock);
    return r;
}
static VKAPI_ATTR VkResult VKAPI_CALL observer_present(VkQueue queue, const VkPresentInfoKHR *info) {
    pthread_mutex_lock(&observer_lock);
    ObserverDevice *c = device_locked(queue);
    PFN_vkQueuePresentKHR next = c ? c->present : NULL;
    pthread_mutex_unlock(&observer_lock);
    if (!next) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult result = next(queue, info);
    uint64_t now = observer_now();
    pthread_mutex_lock(&observer_lock); present_result_locked(info, result, now); pthread_mutex_unlock(&observer_lock);
    return result;
}
static VKAPI_ATTR VkResult VKAPI_CALL observer_create_swapchain(VkDevice device, const VkSwapchainCreateInfoKHR *info, const VkAllocationCallbacks *allocator, VkSwapchainKHR *result) {
    pthread_mutex_lock(&observer_lock);
    ObserverDevice *c = device_locked(device);
    PFN_vkCreateSwapchainKHR next = c ? c->create_swapchain : NULL;
    pthread_mutex_unlock(&observer_lock);
    if (!next) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = next(device, info, allocator, result);
    if (r == VK_SUCCESS) {
        ObserverSwapchain *s = malloc(sizeof *s);
        pthread_mutex_lock(&observer_lock);
        c = device_locked(device);
        if (!s || !c) { bookkeeping_fault = true; free(s); }
        else { s->handle = *result; s->next = c->swapchains; c->swapchains = s; ++active_swapchains; }
        pthread_mutex_unlock(&observer_lock);
    }
    return r;
}
static VKAPI_ATTR void VKAPI_CALL observer_destroy_swapchain(VkDevice device, VkSwapchainKHR handle, const VkAllocationCallbacks *allocator) {
    pthread_mutex_lock(&observer_lock);
    ObserverDevice *c = device_locked(device);
    PFN_vkDestroySwapchainKHR next = c ? c->destroy_swapchain : NULL;
    if (c) {
        ObserverSwapchain **p = &c->swapchains;
        while (*p) {
            if ((*p)->handle == handle) { ObserverSwapchain *s = *p; *p = s->next; free(s); if (active_swapchains) --active_swapchains; break; }
            p = &(*p)->next;
        }
    }
    if (!active_swapchains) reset_window_locked();
    pthread_mutex_unlock(&observer_lock);
    if (next) next(device, handle, allocator);
}
static VKAPI_ATTR void VKAPI_CALL observer_destroy_device(VkDevice handle, const VkAllocationCallbacks *allocator) {
    pthread_mutex_lock(&observer_lock);
    ObserverDevice **p = &devices, *c = NULL;
    while (*p) { if ((*p)->handle == handle) { c = *p; *p = c->next; break; } p = &(*p)->next; }
    if (c) while (c->swapchains) { ObserverSwapchain *s = c->swapchains; c->swapchains = s->next; free(s); if (active_swapchains) --active_swapchains; }
    if (!active_swapchains) reset_window_locked();
    pthread_mutex_unlock(&observer_lock);
    if (c) { c->destroy(handle, allocator); free(c); }
}
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL observer_get_device(VkDevice device, const char *name) {
    if (!name) return NULL;
    if (!strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)observer_get_device;
    pthread_mutex_lock(&observer_lock); ObserverDevice *c = device_locked(device); PFN_vkGetDeviceProcAddr next = c ? c->get : NULL; pthread_mutex_unlock(&observer_lock);
    PFN_vkVoidFunction result = next ? next(device, name) : NULL;
    if (!result) return NULL; // Respect disabled device extensions.
    if (!strcmp(name, "vkQueuePresentKHR")) return (PFN_vkVoidFunction)observer_present;
    if (!strcmp(name, "vkCreateSwapchainKHR")) return (PFN_vkVoidFunction)observer_create_swapchain;
    if (!strcmp(name, "vkDestroySwapchainKHR")) return (PFN_vkVoidFunction)observer_destroy_swapchain;
    if (!strcmp(name, "vkDestroyDevice")) return (PFN_vkVoidFunction)observer_destroy_device;
    return result;
}
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL observer_get_instance(VkInstance instance, const char *name) {
    if (!name) return NULL;
    if (!strcmp(name, "vkGetInstanceProcAddr")) return (PFN_vkVoidFunction)observer_get_instance;
    if (!strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)observer_get_device;
    if (!strcmp(name, "vkCreateInstance")) return (PFN_vkVoidFunction)observer_create_instance;
    if (!strcmp(name, "vkCreateDevice")) return (PFN_vkVoidFunction)observer_create_device;
    if (!strcmp(name, "vkDestroyInstance")) return (PFN_vkVoidFunction)observer_destroy_instance;
    if (!strcmp(name, "vkQueuePresentKHR")) return (PFN_vkVoidFunction)observer_present;
    if (!strcmp(name, "vkCreateSwapchainKHR")) return (PFN_vkVoidFunction)observer_create_swapchain;
    if (!strcmp(name, "vkDestroySwapchainKHR")) return (PFN_vkVoidFunction)observer_destroy_swapchain;
    pthread_mutex_lock(&observer_lock); ObserverInstance *c = instance_locked(instance); PFN_vkGetInstanceProcAddr next = c ? c->get : global_next; pthread_mutex_unlock(&observer_lock);
    return next ? next(instance, name) : NULL;
}
OBS_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *version) {
    if (!version || version->loaderLayerInterfaceVersion < 1) return VK_ERROR_INITIALIZATION_FAILED;
    if (version->loaderLayerInterfaceVersion > 2) version->loaderLayerInterfaceVersion = 2;
    version->pfnGetInstanceProcAddr = observer_get_instance;
    version->pfnGetDeviceProcAddr = observer_get_device;
    version->pfnGetPhysicalDeviceProcAddr = NULL;
    return VK_SUCCESS;
}
OBS_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL monado_present_GetInstanceProcAddr(VkInstance instance, const char *name) { return observer_get_instance(instance, name); }
OBS_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL monado_present_GetDeviceProcAddr(VkDevice device, const char *name) { return observer_get_device(device, name); }
