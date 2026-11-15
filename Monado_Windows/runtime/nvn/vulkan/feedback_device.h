// [XCDE] WINPARITY-105 / LINUX-103: query the optional native feedback contract.
// Extension advertisement alone never permits attachment sampling. Call before
// vkCreateDevice, and retain this exact enabled feature in its pNext chain.
#ifndef MONADO_FEEDBACK_DEVICE_H
#define MONADO_FEEDBACK_DEVICE_H
#include <stdbool.h>
#include <vulkan/vulkan.h>

static inline bool monado_feedback_device_features(VkPhysicalDevice physical, bool extension,
    VkPhysicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT *feature) {
    *feature = (VkPhysicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT){
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_FEATURES_EXT};
    if (!extension) return false;
    VkPhysicalDeviceFeatures2 query = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = feature};
    vkGetPhysicalDeviceFeatures2(physical, &query);
    return feature->attachmentFeedbackLoopLayout != VK_FALSE;
}
#endif
