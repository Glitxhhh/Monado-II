// [XCDE] LINUX-103: adapted from Monado-Reborn contributors (MIT),
// commit 3cc9eeb46dc879fe36929d13758754c429b8377b; see NOTICE.md.
// [XCDE] near/far are spelled near_r/far_r: <windows.h> defines near and far as empty macros.
#pragma once
#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#define XCDE_FOLIAGE_DEFAULT_SCALE 1.f
#define XCDE_FOLIAGE_MIN_SCALE .5f
#define XCDE_FOLIAGE_MAX_SCALE 3.f
#define XCDE_FOLIAGE_AUTHORED_MAX_RADIUS 500.f
#define XCDE_FOLIAGE_MAX_RADIUS 1500.f
#define XCDE_FOLIAGE_BITMAP_BYTES 0x40000u
#define XCDE_FOLIAGE_POOL_BYTES (0x380000u * 9u)
#define XCDE_FOLIAGE_METADATA_BYTES 0x20000u

/* Scale authored local ranges, preserving the broad 7000/8000 fallbacks.
 * Allocate for the largest live preset at map startup: coverage grows with
 * radius squared. A 256 KiB bitmap fits five 5-unit layers at the full 1500-unit
 * endpoint, including the guest's extra 32-byte clear block. */
static inline float xcde_foliage_far_scaled(float near_r, float far_r, float scale) {
    if (!isfinite(near_r) || !isfinite(far_r) || near_r < 0.f ||
        far_r <= near_r || far_r > XCDE_FOLIAGE_AUTHORED_MAX_RADIUS || !isfinite(scale) ||
        scale < XCDE_FOLIAGE_MIN_SCALE || scale > XCDE_FOLIAGE_MAX_SCALE)
        return far_r;
    if (scale == 1.f) return far_r;
    float extended = near_r + (far_r - near_r) * scale;
    return extended > XCDE_FOLIAGE_MAX_RADIUS ? XCDE_FOLIAGE_MAX_RADIUS : extended;
}

float xcde_foliage_distance(float near_r, float far_r);
float xcde_foliage_shading_distance(float near_r, float far_r);
/* Native MPF allocation layout. One block owns bitmap, variable patch pool,
 * fixed metadata, layer scratch and distribution tables, in that order. */
uint32_t xcde_foliage_pool_layout(int part);

uint64_t xcde_foliage_pool_alloc(uint64_t owner);
bool xcde_foliage_pool_free(uint64_t address);
float xcde_foliage_active_scale(void);
bool xcde_foliage_expanded(void);
