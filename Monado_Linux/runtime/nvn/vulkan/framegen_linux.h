// [XCDE] LINUX-020: optional Linux FidelityFX interpolation, independent of presentation and guest clocks.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FgLinux FgLinux;
typedef struct {
    VkImage image;
    VkFormat format;
    uint32_t width, height;
} FgLinuxImage;

// All images are caller owned, in GENERAL layout. Record only outside rendering.
// Keep their memory live until recording_seq completes on the caller's actual GPU timeline.
// The wrapper records commands only: it neither submits/presents nor advances game/input/audio time.
typedef struct {
    VkCommandBuffer commands;
    FgLinuxImage scene, depth, motion, output, hudless;  // hudless may be empty
    float jitter[2], motion_scale[2];
    float dt_ms, camera_near, camera_far, camera_fov, meters;
    int32_t rect_x, rect_y;
    uint32_t rect_width, rect_height;  // zero/zero means the whole scene
    uint64_t real_frame_id, recording_seq, completed_seq;
    bool reset, depth_infinite, depth_inverted;
} FgLinuxFrame;

typedef enum {
    FG_LINUX_ERROR = -1,
    FG_LINUX_DISABLED = 0,
    FG_LINUX_WARMUP = 1,      // successful reset frame; present only its real image
    FG_LINUX_GENERATED = 2   // recorded one interpolation output; presentation is a separate operation
} FgLinuxResult;

typedef struct {
    uint64_t real_frames, generated_recordings, resets, failures;
    uint32_t live_contexts;
    bool enabled, history_valid;
} FgLinuxStats;

// Loads only an explicit canonical regular companion file. Verifies the bridge ABI,
// pinned SDK source identity and actual FG provider version; starts disabled.
FgLinux *fg_linux_open(const char *library_path, VkPhysicalDevice physical, VkDevice device,
                      PFN_vkGetDeviceProcAddr device_proc, char *error, size_t error_size);
bool fg_linux_available(const FgLinux *fg);
void fg_linux_set_enabled(FgLinux *fg, bool enabled);
void fg_linux_reset(FgLinux *fg);
FgLinuxResult fg_linux_dispatch(FgLinux *fg, const FgLinuxFrame *frame);
// Only actual completed timeline values may be passed. These calls never wait for the GPU.
void fg_linux_collect(FgLinux *fg, uint64_t completed_seq);
bool fg_linux_close(FgLinux *fg, uint64_t completed_seq);  // false: pending work/context retained, call again after completion
void fg_linux_stats(const FgLinux *fg, FgLinuxStats *out);
const char *fg_linux_error(const FgLinux *fg);
uint64_t fg_linux_provider_version(const FgLinux *fg);

#ifdef __cplusplus
}
#endif
