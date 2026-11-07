// [XCDE] LINUX-017: authored ABI bridge to the separately licensed NVIDIA SDK.
// Only the loader module links the vendor archive. The host resolves this table
// after enforcing its offline policy, before any vendor module constructors run.
#pragma once
#include <stdint.h>
#include <vulkan/vulkan.h>
#include <nvsdk_ngx_vk.h>

#define MONADO_NGX_FUNCTIONS(X) \
    X(NVSDK_NGX_VULKAN_Init_with_ProjectID) \
    X(NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements) \
    X(NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements) \
    X(NVSDK_NGX_VULKAN_GetFeatureRequirements) \
    X(NVSDK_NGX_VULKAN_GetCapabilityParameters) \
    X(NVSDK_NGX_VULKAN_AllocateParameters) \
    X(NVSDK_NGX_VULKAN_DestroyParameters) \
    X(NVSDK_NGX_VULKAN_CreateFeature) \
    X(NVSDK_NGX_VULKAN_CreateFeature1) \
    X(NVSDK_NGX_VULKAN_EvaluateFeature) \
    X(NVSDK_NGX_VULKAN_EvaluateFeature_C) \
    X(NVSDK_NGX_VULKAN_ReleaseFeature) \
    X(NVSDK_NGX_VULKAN_Shutdown1) \
    X(NVSDK_NGX_Parameter_SetUI) \
    X(NVSDK_NGX_Parameter_SetI) \
    X(NVSDK_NGX_Parameter_SetF) \
    X(NVSDK_NGX_Parameter_SetVoidPointer) \
    X(NVSDK_NGX_Parameter_GetUI) \
    X(NVSDK_NGX_Parameter_GetI) \
    X(NVSDK_NGX_Parameter_GetF) \
    X(NVSDK_NGX_Parameter_GetVoidPointer)

struct MonadoNgxApi {
    uint32_t abi_version;
    uint32_t size;
#define MONADO_NGX_FIELD(function) decltype(&::function) function;
    MONADO_NGX_FUNCTIONS(MONADO_NGX_FIELD)
#undef MONADO_NGX_FIELD
};

// Validate abi_version, size and every pointer before using the returned table. Keep
// the module loaded until all NGX handles/parameters and Vulkan work are retired.
extern "C" const MonadoNgxApi *monado_ngx_api(void);
