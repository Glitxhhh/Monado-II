// [XCDE] LINUX-103: exact mandatory feature chain requested by create_device.
#pragma once
#include <stdbool.h>
#include <vulkan/vulkan.h>
static inline bool monado_gpu_required_features(const VkPhysicalDeviceVulkan11Features *f11,
    const VkPhysicalDeviceVulkan12Features *f12,const VkPhysicalDeviceVulkan13Features *f13) {
    return f11->storageBuffer16BitAccess && f12->timelineSemaphore && f12->shaderInt8 && f12->samplerMirrorClampToEdge &&
        f12->uniformAndStorageBuffer8BitAccess && f12->storageBuffer8BitAccess && f12->scalarBlockLayout &&
        f13->dynamicRendering && f13->synchronization2 && f13->shaderDemoteToHelperInvocation && f13->shaderTerminateInvocation &&
        f13->maintenance4 && f13->pipelineCreationCacheControl;
}
