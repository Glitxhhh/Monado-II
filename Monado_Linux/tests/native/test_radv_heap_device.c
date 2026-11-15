// [XCDE] LINUX-070: controlled driver responses, actual device.c policy includes.
// This validates capability negotiation, not execution on physical AMD hardware.
#include <vulkan/vulkan.h>
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); abort(); } } while (0)
#define LOG(...) ((void)0)

static struct {
    VkPhysicalDevice phys;
    VkPhysicalDeviceProperties props;
    bool radv, heap_placed, heap_dmabuf, feedback_loop;
    VkDeviceSize heap_map_alignment;
} g_vk;
static VkDriverId driver_id;
static unsigned properties_calls, feature_calls, feedback_feature_calls;
static bool can_map, can_reserve, can_feedback;
static VkDeviceSize map_alignment;

static bool has_ext(const VkExtensionProperties *extensions, uint32_t count, const char *name) {
    for (uint32_t i = 0; i < count; ++i)
        if (!strcmp(extensions[i].extensionName, name)) return true;
    return false;
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties2(VkPhysicalDevice device, VkPhysicalDeviceProperties2 *properties) {
    (void)device;
    properties_calls++;
    VkBaseOutStructure *next = properties->pNext;
    if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES) {
        ((VkPhysicalDeviceDriverProperties *)next)->driverID = driver_id;
    } else {
        CHECK(next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_PROPERTIES_EXT);
        ((VkPhysicalDeviceMapMemoryPlacedPropertiesEXT *)next)->minPlacedMemoryMapAlignment = map_alignment;
    }
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures2(VkPhysicalDevice device, VkPhysicalDeviceFeatures2 *features) {
    (void)device;
    VkBaseOutStructure *next = features->pNext;
    if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_FEATURES_EXT) {
        feedback_feature_calls++;
        ((VkPhysicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT *)next)->attachmentFeedbackLoopLayout = can_feedback;
    } else {
        feature_calls++;
        CHECK(next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_FEATURES_EXT);
        VkPhysicalDeviceMapMemoryPlacedFeaturesEXT *placed = (void *)next;
        placed->memoryMapPlaced = can_map;
        placed->memoryUnmapReserve = can_reserve;
    }
}

static unsigned probe(VkExtensionProperties *ep, uint32_t ne) {
    const char *ext[48];
    uint32_t n_ext = 0;
#include "radv_capability.inc"
#include "feedback_capability.inc"
    for (uint32_t i = 0; i < n_ext; ++i) {
        CHECK(ext[i] && has_ext(ep, ne, ext[i]));
        for (uint32_t j = 0; j < i; ++j) CHECK(strcmp(ext[i], ext[j]));
    }
    int prior_feature_chain;
    VkPhysicalDeviceFeatures2 f = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &prior_feature_chain};
#include "radv_features.inc"
#include "feedback_chain.inc"
    void *tail = f.pNext;
    if (g_vk.feedback_loop) {
        CHECK(tail == &feedback && feedback.attachmentFeedbackLoopLayout);
        tail = feedback.pNext;
    }
    if (g_vk.heap_placed) {
        VkPhysicalDeviceMapMemoryPlacedFeaturesEXT *placed = tail;
        CHECK(placed == &heap_features);
        CHECK(placed->memoryMapPlaced && placed->memoryUnmapReserve && !placed->memoryMapRangePlaced);
        CHECK(placed->pNext == &prior_feature_chain);
    } else {
        CHECK(tail == &prior_feature_chain);
    }
    return n_ext;
}

int main(int argc, char **argv) {
    CHECK(argc == 2);
    const char *scenario = argv[1];
    VkExtensionProperties extensions[5] = {{0}};
    const char *names[] = {VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
                          VK_KHR_MAP_MEMORY_2_EXTENSION_NAME, VK_EXT_MAP_MEMORY_PLACED_EXTENSION_NAME,
                          VK_EXT_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_EXTENSION_NAME};
    for (unsigned i = 0; i < 5; ++i)
        snprintf(extensions[i].extensionName, sizeof extensions[i].extensionName, "%s", names[i]);
    g_vk.props.vendorID = 0x1002;
    driver_id = VK_DRIVER_ID_MESA_RADV;
    can_map = can_reserve = can_feedback = true;
    map_alignment = 65536;
    if (!strcmp(scenario, "nvidia")) {
        g_vk.props.vendorID = 0x10de;
        CHECK(probe(extensions, 4) == 0);
        CHECK(!g_vk.radv && !g_vk.heap_placed && !g_vk.heap_dmabuf && !g_vk.heap_map_alignment);
        CHECK(properties_calls == 0 && feature_calls == 0);
    } else if (!strcmp(scenario, "amd-proprietary")) {
        driver_id = VK_DRIVER_ID_AMD_PROPRIETARY;
        CHECK(probe(extensions, 4) == 0 && !g_vk.radv);
        CHECK(properties_calls == 1 && feature_calls == 0);
    } else if (!strcmp(scenario, "radv")) {
        CHECK(probe(extensions, 4) == 4);
        CHECK(g_vk.radv && g_vk.heap_placed && g_vk.heap_dmabuf && g_vk.heap_map_alignment == map_alignment);
    } else if (!strcmp(scenario, "dma-only")) {
        CHECK(probe(extensions, 2) == 2);
        CHECK(g_vk.radv && !g_vk.heap_placed && g_vk.heap_dmabuf && !g_vk.heap_map_alignment);
        CHECK(properties_calls == 1 && feature_calls == 0);
    } else if (!strcmp(scenario, "placed-only")) {
        CHECK(probe(extensions + 2, 2) == 2);
        CHECK(g_vk.radv && g_vk.heap_placed && !g_vk.heap_dmabuf);
    } else if (!strcmp(scenario, "radv-feedback")) {
        CHECK(probe(extensions, 5) == 5);
        CHECK(g_vk.radv && g_vk.heap_placed && g_vk.heap_dmabuf && g_vk.feedback_loop);
        CHECK(g_vk.heap_map_alignment == map_alignment && properties_calls == 2 && feature_calls == 1 && feedback_feature_calls == 1);
    } else if (!strcmp(scenario, "radv-feedback-unavailable")) {
        can_feedback = false;
        CHECK(probe(extensions, 5) == 4);
        CHECK(g_vk.radv && g_vk.heap_placed && g_vk.heap_dmabuf && !g_vk.feedback_loop);
        CHECK(g_vk.heap_map_alignment == map_alignment && properties_calls == 2 && feature_calls == 1 && feedback_feature_calls == 1);
    } else if (!strcmp(scenario, "nvidia-feedback")) {
        g_vk.props.vendorID = 0x10de;
        CHECK(probe(extensions, 5) == 1);
        CHECK(!g_vk.radv && !g_vk.heap_placed && !g_vk.heap_dmabuf && !g_vk.heap_map_alignment && g_vk.feedback_loop);
        CHECK(properties_calls == 0 && feature_calls == 0 && feedback_feature_calls == 1);
    } else if (!strcmp(scenario, "dma-feedback")) {
        can_map = false;
        CHECK(probe(extensions, 5) == 3);
        CHECK(g_vk.radv && !g_vk.heap_placed && g_vk.heap_dmabuf && !g_vk.heap_map_alignment && g_vk.feedback_loop);
        CHECK(properties_calls == 2 && feature_calls == 1 && feedback_feature_calls == 1);
    } else if (!strcmp(scenario, "no-extensions")) {
        CHECK(probe(extensions, 0) == 0);
        CHECK(g_vk.radv && !g_vk.heap_placed && !g_vk.heap_dmabuf);
        CHECK(properties_calls == 1 && feature_calls == 0);
    } else {
        if (!strcmp(scenario, "no-map")) can_map = false;
        else if (!strcmp(scenario, "no-reserve")) can_reserve = false;
        else if (!strcmp(scenario, "zero-alignment")) map_alignment = 0;
        else if (!strcmp(scenario, "bad-alignment")) map_alignment = 3;
        else CHECK(!"unknown scenario");
        CHECK(probe(extensions, 4) == 2);
        CHECK(g_vk.radv && !g_vk.heap_placed && g_vk.heap_dmabuf && !g_vk.heap_map_alignment);
    }
    printf("RADV device policy: PASS %s\n", scenario);
    return 0;
}
