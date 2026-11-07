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

bool tmp_enabled(void);         // the option is on, a temporal upscaler is usable and this run isn't deterministic
void tmp_jitter(float j[2]);
void tmp_hud_snapshot(VkCommandBuffer cb, TexBackend *b);
bool tmp_hud_started(void);
void tmp_set_velocity(TexBackend *v);  // [XCDE] T215: the game's own velocity buffer this frame (G-buffer, RGB10A2)
VkImageView tmp_hud_scene_view(uint32_t w, uint32_t h);  // this frame's image before the HUD (render size) or NULL  // this frame's scene composite is done: the HUD follows (never jittered)  // [XCDE] T213: the presented image before the HUD passes    // the next frame's sub-pixel jitter (render pixels, y down)
// The upscaled image (out_w x out_h) of `color` (render size) or VK_NULL_HANDLE (not this frame: no camera / depth -
// the caller presents as without it). Records into cb, outside any rendering. depth: the scene depth (depth aspect).
VkImageView tmp_upscale(VkCommandBuffer cb, TexBackend *color, VkImageView color_view, TexBackend *depth, const TmpCamera *cam,
                        const float jitter[2], bool depth_minus_one, uint32_t out_w, uint32_t out_h);
