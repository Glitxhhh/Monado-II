// [XCDE] Temporal upscaling / anti-aliasing (temporal.c): AMD FSR 3.1 (any GPU) and NVIDIA DLSS (RTX), chosen in
// the launcher (xcde.ini upscaler = fsr3 | dlss, upscale_quality). exec.c finds the game's camera and scene depth and
// jitters the scene draws; at the present the game image is upscaled to the window with motion vectors from the depth
// and the camera's movement.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "vk.h"

typedef struct {
    float view[12], proj[16], viewproj[16];  // rows (clip = dot(row, position)), as the game's constant buffer holds them
    bool ok;
} TmpCamera;

#ifdef __cplusplus
extern "C" {
#endif

// [XCDE] LINUX-017: GPU idle, guest execution stopped; drops history/guest pointers, retains scratch images.
void tmp_reset(void);
bool tmp_fsr4_set_asset_dir(const char *path); // startup only, exact owned locked model files
bool tmp_fsr4_configure(const char *assets,const char *component); // explicit native startup paths
bool tmp_fsr4_configured(void); // requests optional device features only when local assets were verified
bool tmp_fsr4_supported(void);
const char *tmp_fsr4_status(void); // diagnostic availability/fallback reason
bool tmp_fsr3_supported(void); // checked component/API present; not gameplay/hardware qualification
bool tmp_linear_output(void); // successful native FSR uses linear half-float output, encode before HUD
bool tmp_enabled(void);         // the option is on, a temporal upscaler is usable and this run isn't deterministic
void tmp_jitter(float j[2]);
const char *tmp_provider_status(void); // last dispatch/fallback result; never a GPU-pixel qualification
void tmp_note_spatial_frame(void);
typedef struct TmpSceneEvidence {
    uint64_t frame;
    uint32_t guest_depth_w,guest_depth_h,depth_w,depth_h;
    uint32_t rejected_cameras,world_draws,jittered_draws,nodepth_draws,hud_draws;
    uint32_t camera_cbuf,camera_read_mask,presented_passes,passes_after_snapshot,draws_after_snapshot;
    uint32_t snapshot_width,snapshot_height,snapshot_format,snapshot_draws;
    uint64_t snapshot_pass,snapshot_source,snapshot_vs,snapshot_fs,present_source;
    bool camera_valid,fallback_unjittered;
    float normalized_jitter[2];
} TmpSceneEvidence;
void tmp_diagnostics_frame(const TmpSceneEvidence *scene,uint32_t color_w,uint32_t color_h,
    uint32_t output_w,uint32_t output_h,const float declared_jitter[2]);
void tmp_hud_snapshot(VkCommandBuffer cb, TexBackend *b);
bool tmp_hud_started(void);
void tmp_note_target(TexBackend *b);
void tmp_forget_texture(TexBackend *b);  // render thread: clear a borrowed velocity pointer before its backend is freed
void tmp_set_velocity(TexBackend *v);  // [XCDE] T215: the game's own velocity buffer this frame (G-buffer, RGB10A2)
VkImageView tmp_hud_scene_view(uint32_t w, uint32_t h);  // this frame's image before the HUD, or NULL
// The upscaled image (out_w x out_h) of `color` (render size) or VK_NULL_HANDLE (not this frame: no camera / depth -
// the caller presents as without it). Records into cb, outside any rendering. depth: the scene depth (depth aspect).
VkImageView tmp_upscale(VkCommandBuffer cb, TexBackend *color, VkImageView color_view, TexBackend *depth, const TmpCamera *cam,
                        const float jitter[2], bool depth_minus_one, uint32_t out_w, uint32_t out_h);

// Frame generation: Windows SDK swapchain or separate native Linux offscreen/presentation adapter.
bool tmp_fg_active(void);
bool tmp_frame_gen_wanted(void);
bool tmp_fg_install(VkSwapchainKHR *sc, const VkSwapchainCreateInfoKHR *ci, PFN_vkCreateSwapchainKHR *create,
                    PFN_vkDestroySwapchainKHR *destroy, PFN_vkGetSwapchainImagesKHR *images,
                    PFN_vkAcquireNextImageKHR *acquire, PFN_vkQueuePresentKHR *present);
void tmp_fg_idle(VkCommandBuffer cb);
VkImageView tmp_fg_hudless_target(void);
#ifdef _WIN32
bool tmp_fg_supported(void); // checked local component/API, not hardware/gameplay qualification
void tmp_fg_drain(void); // native SDK host presents must retire before resources/device idle
void tmp_fg_before_present(VkCommandBuffer cb); // no stale inputs for unprepared/spatial frames
bool tmp_fg_inputs_ready(void); // successful current preparation plus recent successful generation dispatch
bool tmp_fg_present_count(uint64_t *count); // SDK reported total; no generated/native identity
void tmp_fg_invalidate(void);
#endif
#ifdef __linux__
// [XCDE] LINUX-020: ordinary Linux swapchain, interpolation after final HUD composition.
bool tmp_fg_supported(void);
void tmp_fg_invalidate(void);
void tmp_fg_set_rect(float x, float y, float w, float h);
bool tmp_fg_record_present(VkCommandBuffer cb, VkImage scene, VkImage *base, float *dt_ms);
#endif

#ifdef __cplusplus
}
#endif
