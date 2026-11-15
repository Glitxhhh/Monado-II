// [XCDE] WINDOWS-005: synthetic real-device host import/data/lifetime test.
// No game inputs, window, shaders, saves or renderer-success stubs.
#include "../../runtime/nvn/vulkan/host_import.h"
#include "../../runtime/nvn/vulkan/host_import_memory_windows.h"
#define HEAP_MAX 0xD0000000ull
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
#define VK_OK(x) do { VkResult result_ = (x); if (result_ != VK_SUCCESS) { fprintf(stderr, "FAIL line %d: %s -> %d\n", __LINE__, #x, result_); exit(1); } } while (0)
static VkPhysicalDevice physical;
static VkDevice device;
static VkQueue queue;
static VkCommandBuffer command;
static VkFence fence;
static VkBuffer staging;
static VkDeviceMemory staging_memory;
static void *staging_host;
static int staging_coherent;
static const VkDeviceSize sample_size = 262144;

static void begin(void) {
    VK_OK(vkResetCommandBuffer(command, 0));
    VkCommandBufferBeginInfo start = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    VK_OK(vkBeginCommandBuffer(command, &start));
}
static void host_barrier(void) {
    VkMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, NULL, 0, NULL);
}
static void finish(void) {
    VK_OK(vkEndCommandBuffer(command));
    VK_OK(vkResetFences(device, 1, &fence));
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &command};
    VK_OK(vkQueueSubmit(queue, 1, &submit, fence));
    VK_OK(vkWaitForFences(device, 1, &fence, VK_TRUE, 10000000000ull));
}
static void readback(const VkHostImport *imported, VkDeviceSize offset, void *host) {
    begin();
    VkBufferCopy copy = {offset, 0, sample_size};
    vkCmdCopyBuffer(command, imported->buffer, staging, 1, &copy);
    host_barrier(); finish();
    if (!staging_coherent) {
        VkMappedMemoryRange range = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
            .memory = staging_memory, .offset = 0, .size = VK_WHOLE_SIZE};
        VK_OK(vkInvalidateMappedMemoryRanges(device, 1, &range));
    }
    CHECK(memcmp(staging_host, host, (size_t)sample_size) == 0);
}
static void pattern(unsigned char *host, unsigned seed) {
    for (VkDeviceSize i = 0; i < sample_size; i++) host[i] = (unsigned char)((i * 73 + (i >> 9) + seed * 31) ^ (i >> 3));
}
static void exercise(VkHostImport *imported, unsigned char *host, unsigned seed) {
    VkDeviceSize offsets[] = {0, (imported->size / 2) & ~3ull, imported->size - sample_size};
    for (unsigned sample = 0; sample < 3; ++sample) {
        VkDeviceSize offset = offsets[sample];
        if (offset + sample_size > imported->size) offset = imported->size - sample_size;
        unsigned char *at = host + offset;
        pattern(at, seed + sample); readback(imported, offset, at); // original host alias -> GPU -> mapped staging
        uint32_t fill = 0x12340000u | (seed << 8) | sample;
        begin(); vkCmdFillBuffer(command, imported->buffer, offset, sample_size, fill); host_barrier(); finish();
        for (VkDeviceSize word = 0; word < sample_size / 4; ++word) CHECK(((uint32_t *)at)[word] == fill);
        pattern(at, seed + sample + 19); readback(imported, offset, at); // CPU writes after GPU write are visible again
    }
}
static void make_staging(void) {
    VkBufferCreateInfo create = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = sample_size,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VK_OK(vkCreateBuffer(device, &create, NULL, &staging));
    VkMemoryRequirements requirements; vkGetBufferMemoryRequirements(device, staging, &requirements);
    VkPhysicalDeviceMemoryProperties memory; vkGetPhysicalDeviceMemoryProperties(physical, &memory);
    uint32_t chosen = UINT32_MAX;
    for (uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
        if ((requirements.memoryTypeBits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
            chosen = i;
            if (memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) break;
        }
    }
    CHECK(chosen != UINT32_MAX);
    staging_coherent = !!(memory.memoryTypes[chosen].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkMemoryAllocateInfo allocate = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size, .memoryTypeIndex = chosen};
    VK_OK(vkAllocateMemory(device, &allocate, NULL, &staging_memory));
    VK_OK(vkBindBufferMemory(device, staging, staging_memory, 0));
    VK_OK(vkMapMemory(device, staging_memory, 0, VK_WHOLE_SIZE, 0, &staging_host));
}

int main(int argc, char **argv) {
    int full_heap = 0;
    if (argc == 2 && !strcmp(argv[1], "--full-heap")) full_heap = 1;
    else CHECK(argc == 1);
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "Monado synthetic host import test",
        .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo create = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    VkInstance instance; VK_OK(vkCreateInstance(&create, NULL, &instance));
    uint32_t count = 0; VK_OK(vkEnumeratePhysicalDevices(instance, &count, NULL)); CHECK(count > 0 && count < 64);
    VkPhysicalDevice *devices = calloc(count, sizeof *devices); CHECK(devices != NULL);
    VK_OK(vkEnumeratePhysicalDevices(instance, &count, devices));
    for (uint32_t i = 0; i < count; ++i) {
        VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(devices[i], &p);
        printf("device %u: %s vendor=%#x id=%#x\n", i, p.deviceName, p.vendorID, p.deviceID);
        if (p.vendorID == 0x10de && p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) physical = devices[i];
    }
    free(devices); if (!physical) { puts("UNAVAILABLE: no NVIDIA discrete GPU selected for this authored probe"); return 77; }
    VkPhysicalDeviceExternalMemoryHostPropertiesEXT host_properties = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
    VkPhysicalDeviceDriverProperties driver = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES, .pNext = &host_properties};
    VkPhysicalDeviceProperties2 properties = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &driver};
    vkGetPhysicalDeviceProperties2(physical, &properties);
    printf("selected=%s driver=%s %s api=%u.%u.%u host_alignment=%llu maxStorageBufferRange=%u\n",
        properties.properties.deviceName, driver.driverName, driver.driverInfo,
        VK_API_VERSION_MAJOR(properties.properties.apiVersion), VK_API_VERSION_MINOR(properties.properties.apiVersion),
        VK_API_VERSION_PATCH(properties.properties.apiVersion), (unsigned long long)host_properties.minImportedHostPointerAlignment,
        properties.properties.limits.maxStorageBufferRange);
    uint32_t layers = 0; VK_OK(vkEnumerateInstanceLayerProperties(&layers, NULL));
    VkLayerProperties *layer = calloc(layers, sizeof *layer); CHECK(layer || !layers);
    VK_OK(vkEnumerateInstanceLayerProperties(&layers, layer));
    int validation = 0; for (uint32_t i = 0; i < layers; i++) if (!strcmp(layer[i].layerName, "VK_LAYER_KHRONOS_validation")) validation = 1;
    free(layer); printf("Khronos validation available=%d; test instance validation disabled (no validation-pass claim)\n", validation);
    uint32_t extensions = 0; VK_OK(vkEnumerateDeviceExtensionProperties(physical, NULL, &extensions, NULL));
    VkExtensionProperties *extension = calloc(extensions, sizeof *extension); CHECK(extension != NULL);
    VK_OK(vkEnumerateDeviceExtensionProperties(physical, NULL, &extensions, extension));
    int supported = 0; for (uint32_t i = 0; i < extensions; i++) if (!strcmp(extension[i].extensionName, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME)) supported = 1;
    free(extension); if (!supported) { puts("UNAVAILABLE: VK_EXT_external_memory_host is unsupported"); vkDestroyInstance(instance,NULL); return 77; }
    uint32_t families = 0; vkGetPhysicalDeviceQueueFamilyProperties(physical, &families, NULL);
    VkQueueFamilyProperties *family = calloc(families, sizeof *family); CHECK(family != NULL);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &families, family);
    uint32_t chosen = UINT32_MAX;
    for (uint32_t i = 0; i < families; ++i) if ((family[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) { chosen = i; break; }
    free(family); CHECK(chosen != UINT32_MAX);
    float priority = 1;
    VkDeviceQueueCreateInfo queues = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = chosen,
        .queueCount = 1, .pQueuePriorities = &priority};
    const char *enabled[] = {VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME};
    VkDeviceCreateInfo device_create = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queues, .enabledExtensionCount = 1, .ppEnabledExtensionNames = enabled};
    VK_OK(vkCreateDevice(physical, &device_create, NULL, &device)); vkGetDeviceQueue(device, chosen, 0, &queue);
    VkCommandPoolCreateInfo pool_create = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = chosen, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};
    VkCommandPool pool; VK_OK(vkCreateCommandPool(device, &pool_create, NULL, &pool));
    VkCommandBufferAllocateInfo commands = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VK_OK(vkAllocateCommandBuffers(device, &commands, &command));
    VkFenceCreateInfo fence_create = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VK_OK(vkCreateFence(device, &fence_create, NULL, &fence)); make_staging();

    VkDeviceSize alignment = host_properties.minImportedHostPointerAlignment;
    CHECK(alignment && !(alignment & (alignment - 1)));
    VkDeviceSize capacity = full_heap ? HEAP_MAX : 4ull << 20;
    CHECK(capacity % alignment == 0 && capacity <= properties.properties.limits.maxStorageBufferRange);
    CHECK(capacity <= SIZE_MAX - alignment);
    void *mapping = VirtualAlloc(NULL, (size_t)(capacity+alignment), MEM_RESERVE, PAGE_READWRITE);
    CHECK(mapping != NULL);
    unsigned char *host = (unsigned char *)(((uintptr_t)mapping + alignment - 1) & ~(uintptr_t)(alignment - 1));
    printf("host allocation=%p accessible=%llu\n", (void *)host, (unsigned long long)capacity);
    VkBufferUsageFlags usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
        VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    VkHostImport imported = {0}; char error[256];
    CHECK(vk_host_import_buffer(physical, device, host + 1, capacity - 1, sample_size, usage, &imported, error, sizeof error) != VK_SUCCESS);
    CHECK(!imported.buffer && !imported.memory); printf("misaligned pointer rejected before import: %s\n", error);
    for (unsigned iteration = 0; iteration < 3; ++iteration) {
        VkDeviceSize size = full_heap ? capacity : iteration ? 2ull << 20 : 1ull << 20;
        CHECK(windows_host_import_commit(host, (size_t)size, error, sizeof error));
        pattern(host, iteration); // imported existing contents must survive
        VkResult result = vk_host_import_buffer(physical, device, host, size, size, usage, &imported, error, sizeof error);
        if (result != VK_SUCCESS) fprintf(stderr, "import: %s\n", error);
        CHECK(result == VK_SUCCESS);
        printf("iteration=%u buffer=%llu allocation=%llu type=%u\n", iteration,
            (unsigned long long)imported.size, (unsigned long long)imported.allocation_size, imported.memory_type);
        readback(&imported, 0, host); exercise(&imported, host, iteration + 1);
        VK_OK(vkDeviceWaitIdle(device)); vk_host_import_destroy(device, &imported);
        CHECK(!imported.buffer && !imported.memory);
        if (iteration == 1) { // discard physical pages only after unpinning, retaining reservation
            CHECK(VirtualFree(host, (size_t)capacity, MEM_DECOMMIT));
            CHECK(windows_host_import_commit(host, (size_t)size, error, sizeof error));
            CHECK(host[0] == 0 && host[size-1] == 0);
        }
    }
    CHECK(VirtualFree(mapping, 0, MEM_RELEASE));
    vkUnmapMemory(device, staging_memory); vkDestroyBuffer(device, staging, NULL); vkFreeMemory(device, staging_memory, NULL);
    vkDestroyFence(device, fence, NULL); vkDestroyCommandPool(device, pool, NULL); vkDestroyDevice(device, NULL); vkDestroyInstance(instance, NULL);
    printf("real native Vulkan host import: CPU->GPU, GPU->CPU, coherent aliases, growth/release/remap cycles OK (%s)\n", full_heap ? "3.25GiB extent; sampled beginning/middle/end" : "1/2MiB extents");
    return 0;
}
