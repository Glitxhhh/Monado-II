// [XCDE] T-FSR4-LITE: versioned C interface to the optional native Vulkan component.
// SPDX-License-Identifier: MIT
#pragma once
#include <stdint.h>
#include <vulkan/vulkan.h>
#ifdef __cplusplus
extern "C" {
#endif
#define MONADO_FSR4_ABI 2u
typedef struct { VkImage image; VkImageView view; uint32_t width, height; } MonadoFsr4Image;
typedef struct {
    VkCommandBuffer cmd;
    MonadoFsr4Image color, depth, motion, output;
    float jitter[2];
    uint32_t reset, ultra;
    float sharpness; // [XCDE] T-RENDER-FIXES: zero disables RCAS
} MonadoFsr4Frame;
typedef struct {
    uint32_t abi;
    void *(*create)(VkPhysicalDevice, VkDevice, const char *assets);
    void (*destroy)(void *);
    int (*build)(void *, uint32_t width, uint32_t height, uint32_t ultra);
    int (*record)(void *, const MonadoFsr4Frame *);
    const char *(*error)(void *);
} MonadoFsr4Api;
typedef const MonadoFsr4Api *(*MonadoFsr4GetApi)(uint32_t);
#ifdef __cplusplus
}
#endif
