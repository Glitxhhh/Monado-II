// [XCDE] LINUX-005: one checked import path for renderer and synthetic GPU tests.
#include "host_import.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static VkResult fail(char *error, size_t size, VkResult result, const char *operation) {
    if (error && size) snprintf(error, size, "%s (VkResult %d)", operation, result);
    return result;
}

void vk_host_import_destroy(VkDevice device, VkHostImport *imported) {
    if (!imported) return;
    if (imported->buffer) vkDestroyBuffer(device, imported->buffer, NULL);
    if (imported->memory) vkFreeMemory(device, imported->memory, NULL);
    memset(imported, 0, sizeof *imported);
}

VkResult vk_host_import_buffer(VkPhysicalDevice physical, VkDevice device, void *host,
                               VkDeviceSize accessible_size, VkDeviceSize buffer_size,
                               VkBufferUsageFlags usage, VkHostImport *out,
                               char *error, size_t error_size) {
    if (error && error_size) error[0] = 0;
    if (!out || out->buffer || out->memory || !host || !buffer_size ||
        !usage || accessible_size < buffer_size ||
        accessible_size > UINTPTR_MAX - (uintptr_t)host)
        return fail(error, error_size, VK_ERROR_INITIALIZATION_FAILED, "invalid or occupied host import arguments");
    PFN_vkGetMemoryHostPointerPropertiesEXT pointer_properties =
        (PFN_vkGetMemoryHostPointerPropertiesEXT)vkGetDeviceProcAddr(device, "vkGetMemoryHostPointerPropertiesEXT");
    if (!pointer_properties)
        return fail(error, error_size, VK_ERROR_EXTENSION_NOT_PRESENT, "VK_EXT_external_memory_host entry point unavailable");

    VkPhysicalDeviceExternalMemoryHostPropertiesEXT host_properties = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
    VkPhysicalDeviceProperties2 properties = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &host_properties};
    vkGetPhysicalDeviceProperties2(physical, &properties);
    VkDeviceSize alignment = host_properties.minImportedHostPointerAlignment;
    if (!alignment || (alignment & (alignment - 1)) || ((uintptr_t)host & (alignment - 1)))
        return fail(error, error_size, VK_ERROR_INVALID_EXTERNAL_HANDLE, "host pointer import alignment");

    const VkExternalMemoryHandleTypeFlagBits handle_type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    VkPhysicalDeviceExternalBufferInfo query = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO,
        .usage = usage, .handleType = handle_type};
    VkExternalBufferProperties external = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES};
    vkGetPhysicalDeviceExternalBufferProperties(physical, &query, &external);
    if (!(external.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) ||
        !(external.externalMemoryProperties.compatibleHandleTypes & handle_type))
        return fail(error, error_size, VK_ERROR_INVALID_EXTERNAL_HANDLE, "buffer usage does not support host import");

    VkMemoryHostPointerPropertiesEXT pointer = {.sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    VkResult result = pointer_properties(device, handle_type, host, &pointer);
    if (result != VK_SUCCESS) return fail(error, error_size, result, "vkGetMemoryHostPointerPropertiesEXT");

    VkHostImport candidate = {0};
    VkExternalMemoryBufferCreateInfo external_create = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
        .handleTypes = handle_type};
    VkBufferCreateInfo create = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &external_create,
        .size = buffer_size, .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    result = vkCreateBuffer(device, &create, NULL, &candidate.buffer);
    if (result != VK_SUCCESS) return fail(error, error_size, result, "vkCreateBuffer for host import");

    VkMemoryDedicatedRequirements dedicated = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
    VkMemoryRequirements2 requirements = {.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, .pNext = &dedicated};
    VkBufferMemoryRequirementsInfo2 requirements_query = {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2,
        .buffer = candidate.buffer};
    vkGetBufferMemoryRequirements2(device, &requirements_query, &requirements);
    VkDeviceSize required = requirements.memoryRequirements.size;
    if (!required || required > UINT64_MAX - (alignment - 1)) {
        result = fail(error, error_size, VK_ERROR_OUT_OF_HOST_MEMORY, "host import requirement size overflow");
        goto cleanup;
    }
    VkDeviceSize allocation = (required + alignment - 1) & ~(alignment - 1);
    if (allocation > accessible_size) {
        result = fail(error, error_size, VK_ERROR_OUT_OF_HOST_MEMORY, "host allocation is smaller than aligned buffer requirements");
        goto cleanup;
    }

    VkPhysicalDeviceMemoryProperties memory;
    vkGetPhysicalDeviceMemoryProperties(physical, &memory);
    uint32_t types = pointer.memoryTypeBits & requirements.memoryRequirements.memoryTypeBits;
    uint32_t chosen = UINT32_MAX;
    const VkMemoryPropertyFlags needed = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t index = 0; index < memory.memoryTypeCount; ++index) {
        VkMemoryPropertyFlags flags = memory.memoryTypes[index].propertyFlags;
        if ((types & (1u << index)) && (flags & needed) == needed && !(flags & VK_MEMORY_PROPERTY_PROTECTED_BIT)) {
            chosen = index;
            break;
        }
    }
    if (chosen == UINT32_MAX) {
        result = fail(error, error_size, VK_ERROR_FEATURE_NOT_PRESENT, "no compatible host-visible coherent memory type");
        goto cleanup;
    }
    VkMemoryDedicatedAllocateInfo dedicated_allocate = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .buffer = candidate.buffer};
    VkImportMemoryHostPointerInfoEXT import = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
        .pNext = dedicated.requiresDedicatedAllocation ||
            (external.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT)
            ? &dedicated_allocate : NULL,
        .handleType = handle_type, .pHostPointer = host};
    VkMemoryAllocateInfo allocate = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &import,
        .allocationSize = allocation, .memoryTypeIndex = chosen};
    result = vkAllocateMemory(device, &allocate, NULL, &candidate.memory);
    if (result != VK_SUCCESS) {
        fail(error, error_size, result, "vkAllocateMemory for host import");
        goto cleanup;
    }
    result = vkBindBufferMemory(device, candidate.buffer, candidate.memory, 0);
    if (result != VK_SUCCESS) {
        fail(error, error_size, result, "vkBindBufferMemory for host import");
        goto cleanup;
    }
    candidate.size = buffer_size;
    candidate.allocation_size = allocation;
    candidate.memory_type = chosen;
    *out = candidate;
    return VK_SUCCESS;
cleanup:
    vk_host_import_destroy(device, &candidate);
    return result;
}
