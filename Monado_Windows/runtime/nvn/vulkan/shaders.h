// Shader compilation and reflection for the Vulkan backend (shaders.c).
#pragma once
#include <stddef.h>

#include "vk.h"

typedef struct { uint32_t set, binding; VkDescriptorType type; } VkBindingInfo;

void vk_shaders_init(void);
uint32_t *vk_glsl_to_spirv(const char *src, int stage, const char *name, size_t *nwords);  // malloc'd, NULL on error
bool vk_spirv_cached(const char *src, int stage);  // [XCDE] T187
extern uint64_t vk_spirv_hits, vk_spirv_misses;  // [XCDE] T187: SPIR-V disk cache hits / compiles this run
extern _Thread_local unsigned vk_spirv_tl_misses;  // [XCDE] (the calling thread's compiles: build-wait log)
VkShaderModule vk_module(const uint32_t *spv, size_t nwords);
// Descriptor bindings declared by a SPIR-V module.
int vk_reflect(const uint32_t *spv, size_t nwords, VkBindingInfo *out, int max);
VkDescriptorSetLayout vk_set_layout(const VkBindingInfo *b, int n, uint32_t set, VkShaderStageFlags stages);
