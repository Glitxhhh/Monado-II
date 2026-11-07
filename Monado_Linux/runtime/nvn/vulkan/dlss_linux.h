// [XCDE] LINUX-017: native NGX Vulkan SR/DLAA provider; no renderer globals.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    VkImage image;
    VkImageView view;
    VkFormat format;
    uint32_t width, height;
} DlssImage;
typedef struct {
    DlssImage color, motion, depth, output;
    float jitter[2];
    bool reset;
    int quality;  // 0 DLAA, 1 Quality, 2 Balanced, 3 Performance, 4 Ultra Performance
    // Monotonic submission IDs: recorded commands may not have been submitted yet.
    uint64_t recording_seq, completed_seq;
} DlssFrame;
bool ngx_linux_init(const char *feature_dir, const char *data_dir);
int ngx_linux_instance_exts(const char **out, int max);
bool ngx_linux_device_exts(VkInstance inst, VkPhysicalDevice phys, const char **out, uint32_t *n, uint32_t max);
void ngx_linux_device_created(VkInstance inst, VkPhysicalDevice phys, VkDevice dev, uint32_t qfam);
bool ngx_linux_available(void);
bool ngx_linux_optimal(uint32_t ow, uint32_t oh, int quality, uint32_t *rw, uint32_t *rh);
bool ngx_linux_evaluate(VkCommandBuffer cb, const DlssFrame *frame);
// Call only after all recorded work has been submitted/completed or discarded.
void ngx_linux_reset(void);
void ngx_linux_shutdown(void);
#ifdef __cplusplus
}
#endif
