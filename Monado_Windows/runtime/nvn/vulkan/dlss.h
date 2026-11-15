// [XCDE] LINUX-017: renderer/provider boundary, shared with Windows Streamline.
#pragma once
#include "temporal.h"
#ifdef __cplusplus
extern "C" {
#endif
bool dlss_init_early(void);
int dlss_instance_exts(const char **out, int max);
void dlss_device_requirements(VkInstance inst, VkPhysicalDevice phys, const char **ext, uint32_t *n, uint32_t max,
    VkPhysicalDeviceVulkan12Features *f12, VkPhysicalDeviceVulkan13Features *f13, uint32_t *extra_gfx, uint32_t *extra_compute);
void dlss_device_created(VkInstance inst, VkPhysicalDevice phys, VkDevice dev, uint32_t qfam);
PFN_vkVoidFunction dlss_proxy(VkDevice dev, const char *name);
bool dlss_available(void);
bool dlss_evaluate(VkCommandBuffer cb, const TexBackend *color, const TexBackend *mv, const TexBackend *depth,
    const TexBackend *out, const TexBackend *hud, const float jitter[2], bool reset, const TmpCamera *cam,
    const double reproj[16], uint64_t recording_seq, uint64_t completed_seq);
// GPU-idle guest reset / final shutdown; never called from a pending frame.
void dlss_reset(void);
void dlss_shutdown(void);
#ifdef __cplusplus
}
#endif
