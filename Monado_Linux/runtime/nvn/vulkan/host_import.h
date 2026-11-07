// [XCDE] LINUX-005: checked external host-memory buffer import.
#pragma once
#include <stddef.h>
#include <vulkan/vulkan.h>

typedef struct VkHostImport {
    VkBuffer buffer;
    VkDeviceMemory memory;
    VkDeviceSize size, allocation_size;
    uint32_t memory_type;
} VkHostImport;

// Requires Vulkan 1.1+ and enabled VK_EXT_external_memory_host. The caller owns
// host[0..accessible_size), must not free/remap it while imported, and must not
// import overlapping host pages twice. CPU/GPU access still requires ordering.
// Only HOST_VISIBLE|HOST_COHERENT memory is accepted: runtime CPU accesses use the
// original guest address and have no noncoherent flush/invalidate protocol.
// out must be zero-initialized; on error it remains empty and error describes the
// failed operation. No fallback storage or uninitialized buffer is returned.
VkResult vk_host_import_buffer(VkPhysicalDevice physical, VkDevice device, void *host,
                               VkDeviceSize accessible_size, VkDeviceSize buffer_size,
                               VkBufferUsageFlags usage, VkHostImport *out,
                               char *error, size_t error_size);

// Caller must finish all submitted AND recorded uses before releasing the import.
// Does not free or remap the caller-owned host allocation; idempotent when empty.
void vk_host_import_destroy(VkDevice device, VkHostImport *imported);
