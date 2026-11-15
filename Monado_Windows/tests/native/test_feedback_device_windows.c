// [XCDE] WINPARITY-105: actual production capability query, authored device only.
#include <assert.h>
#include <stdio.h>
#include "nvn/vulkan/feedback_device.h"
static unsigned calls;
static VkBool32 advertised;
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures2(VkPhysicalDevice physical, VkPhysicalDeviceFeatures2 *query) {
    assert(physical == (VkPhysicalDevice)(uintptr_t)17);
    assert(query->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2);
    VkPhysicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT *feature = query->pNext;
    assert(feature && !feature->pNext && !feature->attachmentFeedbackLoopLayout);
    assert(feature->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_FEATURES_EXT);
    feature->attachmentFeedbackLoopLayout = advertised;
    calls++;
}
int main(void) {
    VkPhysicalDevice physical = (VkPhysicalDevice)(uintptr_t)17;
    VkPhysicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT feature;
    advertised = VK_TRUE;
    assert(!monado_feedback_device_features(physical, false, &feature));
    assert(!calls && !feature.attachmentFeedbackLoopLayout && !feature.pNext);
    advertised = VK_FALSE;
    assert(!monado_feedback_device_features(physical, true, &feature) && calls == 1);
    advertised = VK_TRUE;
    assert(monado_feedback_device_features(physical, true, &feature) && calls == 2);
    assert(feature.attachmentFeedbackLoopLayout && !feature.pNext);
    assert(!monado_feedback_device_features(physical, false, &feature));
    assert(calls == 2 && !feature.attachmentFeedbackLoopLayout);
    puts("PASS native feedback capability: missing extension, unavailable feature, enabled feature, reset");
}
