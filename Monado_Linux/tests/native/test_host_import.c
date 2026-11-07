// [XCDE] LINUX-005: controlled Vulkan responses exercise the real import helper.
// This is failure-path/selection coverage, separate from the real GPU test.
#include "../../runtime/nvn/vulkan/host_import.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); abort(); } } while (0)
static uint32_t pointer_bits, buffer_bits, selected_type;
static VkMemoryPropertyFlags type_flags[3];
static VkDeviceSize requirement_size, allocation_size, alignment;
static VkExternalMemoryFeatureFlags external_features;
static VkResult create_result, pointer_result, allocate_result, bind_result;
static int creates, destroys, allocates, frees, binds, queries, dedicated_required, dedicated_seen, missing_entry;
static void reset(void) {
    pointer_bits = 3; buffer_bits = 6; selected_type = UINT32_MAX;
    type_flags[0] = type_flags[1] = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    type_flags[2] = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    requirement_size = 70000; alignment = 65536; allocation_size = 0;
    external_features = VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT;
    create_result = pointer_result = allocate_result = bind_result = VK_SUCCESS;
    creates = destroys = allocates = frees = binds = queries = dedicated_required = dedicated_seen = missing_entry = 0;
}
VKAPI_ATTR VkResult VKAPI_CALL fake_pointer(VkDevice d, VkExternalMemoryHandleTypeFlagBits h, const void *p, VkMemoryHostPointerPropertiesEXT *out) {
    (void)d; CHECK(h == VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT); CHECK((uintptr_t)p % alignment == 0);
    queries++; out->memoryTypeBits = pointer_bits; return pointer_result;
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice d, const char *name) {
    (void)d; CHECK(!strcmp(name, "vkGetMemoryHostPointerPropertiesEXT"));
    return missing_entry ? NULL : (PFN_vkVoidFunction)fake_pointer;
}
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties2(VkPhysicalDevice d, VkPhysicalDeviceProperties2 *p) {
    (void)d; ((VkPhysicalDeviceExternalMemoryHostPropertiesEXT *)p->pNext)->minImportedHostPointerAlignment = alignment;
}
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceExternalBufferProperties(VkPhysicalDevice d, const VkPhysicalDeviceExternalBufferInfo *q, VkExternalBufferProperties *p) {
    (void)d; CHECK(q->usage == VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    p->externalMemoryProperties.externalMemoryFeatures = external_features;
    p->externalMemoryProperties.compatibleHandleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
}
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice d, VkPhysicalDeviceMemoryProperties *p) {
    (void)d; memset(p, 0, sizeof *p); p->memoryTypeCount = 3;
    for (int i = 0; i < 3; i++) p->memoryTypes[i].propertyFlags = type_flags[i];
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateBuffer(VkDevice d, const VkBufferCreateInfo *c, const VkAllocationCallbacks *a, VkBuffer *b) {
    (void)d; (void)a; CHECK(c->size == 70000 && c->usage == VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    CHECK(((const VkExternalMemoryBufferCreateInfo *)c->pNext)->handleTypes == VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT);
    creates++; if (create_result == VK_SUCCESS) *b = (VkBuffer)(uintptr_t)1; return create_result;
}
VKAPI_ATTR void VKAPI_CALL vkGetBufferMemoryRequirements2(VkDevice d, const VkBufferMemoryRequirementsInfo2 *q, VkMemoryRequirements2 *r) {
    (void)d; CHECK(q->buffer == (VkBuffer)(uintptr_t)1);
    r->memoryRequirements = (VkMemoryRequirements){requirement_size, 256, buffer_bits};
    ((VkMemoryDedicatedRequirements *)r->pNext)->requiresDedicatedAllocation = dedicated_required;
}
VKAPI_ATTR VkResult VKAPI_CALL vkAllocateMemory(VkDevice d, const VkMemoryAllocateInfo *a, const VkAllocationCallbacks *c, VkDeviceMemory *m) {
    (void)d; (void)c;
    CHECK(a->memoryTypeIndex < 3 && (pointer_bits & buffer_bits & (1u << a->memoryTypeIndex)));
    CHECK(a->allocationSize % alignment == 0 && a->allocationSize >= requirement_size);
    CHECK(creates == 1); allocates++; selected_type = a->memoryTypeIndex; allocation_size = a->allocationSize;
    const VkImportMemoryHostPointerInfoEXT *i = a->pNext;
    CHECK(i->handleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT);
    if (i->pNext) {
        const VkMemoryDedicatedAllocateInfo *ded = i->pNext;
        CHECK(ded->sType == VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO && ded->buffer == (VkBuffer)(uintptr_t)1);
        dedicated_seen++;
    }
    if (allocate_result == VK_SUCCESS) *m = (VkDeviceMemory)(uintptr_t)2;
    return allocate_result;
}
VKAPI_ATTR VkResult VKAPI_CALL vkBindBufferMemory(VkDevice d, VkBuffer b, VkDeviceMemory m, VkDeviceSize o) {
    (void)d; CHECK(b == (VkBuffer)(uintptr_t)1 && m == (VkDeviceMemory)(uintptr_t)2 && o == 0); binds++; return bind_result;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyBuffer(VkDevice d, VkBuffer b, const VkAllocationCallbacks *a) {
    (void)d; (void)a; CHECK(b == (VkBuffer)(uintptr_t)1); CHECK(frees == 0); destroys++;
}
VKAPI_ATTR void VKAPI_CALL vkFreeMemory(VkDevice d, VkDeviceMemory m, const VkAllocationCallbacks *a) {
    (void)d; (void)a; CHECK(m == (VkDeviceMemory)(uintptr_t)2 && destroys == 1); frees++;
}
static VkResult import(VkHostImport *out, uintptr_t pointer, VkDeviceSize available, char error[256]) {
    return vk_host_import_buffer((VkPhysicalDevice)(uintptr_t)3, (VkDevice)(uintptr_t)4, (void *)pointer,
        available, 70000, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, out, error, 256);
}
int main(void) {
    VkHostImport out = {0}; char error[256]; int cases = 0;
    reset(); CHECK(import(&out, 0x100000, 131072, error) == VK_SUCCESS);
    CHECK(selected_type == 1 && allocation_size == 131072 && out.size == 70000 && out.allocation_size == 131072);
    vk_host_import_destroy((VkDevice)(uintptr_t)4, &out); CHECK(destroys == 1 && frees == 1 && !out.buffer && !out.memory);
    vk_host_import_destroy((VkDevice)(uintptr_t)4, &out); CHECK(destroys == 1 && frees == 1); cases++;
    for (int kind = 0; kind < 2; kind++) {
        reset(); if (kind) external_features |= VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT; else dedicated_required = 1;
        CHECK(import(&out, 0x100000, 131072, error) == VK_SUCCESS && dedicated_seen == 1);
        vk_host_import_destroy((VkDevice)(uintptr_t)4, &out); cases++;
    }
    for (int kind = 0; kind < 12; kind++) {
        reset(); uintptr_t pointer = 0x100000; VkDeviceSize available = 131072;
        switch (kind) {
        case 0: pointer++; break;
        case 1: available = 70000; break;
        case 2: pointer_bits = 1; break; // no intersection, although pointer type0 is coherent
        case 3: type_flags[1] = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT; break;
        case 4: type_flags[1] = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT; break;
        case 5: bind_result = VK_ERROR_OUT_OF_DEVICE_MEMORY; break;
        case 6: allocate_result = VK_ERROR_OUT_OF_HOST_MEMORY; break;
        case 7: create_result = VK_ERROR_OUT_OF_DEVICE_MEMORY; break;
        case 8: pointer_result = VK_ERROR_INVALID_EXTERNAL_HANDLE; break;
        case 9: external_features = 0; break;
        case 10: requirement_size = UINT64_MAX; break;
        case 11: missing_entry = 1; break;
        }
        CHECK(import(&out, pointer, available, error) != VK_SUCCESS && error[0]);
        CHECK(!out.buffer && !out.memory);
        if (creates && create_result == VK_SUCCESS) CHECK(destroys == 1);
        if (allocates && allocate_result == VK_SUCCESS) CHECK(frees == 1);
        if (kind < 5 || kind >= 7) CHECK(allocates == 0);
        if (!kind || kind == 9 || kind == 11) CHECK(creates == 0);
        cases++;
    }
    printf("host import selection/failure cleanup: %d cases OK\n", cases);
    return 0;
}
