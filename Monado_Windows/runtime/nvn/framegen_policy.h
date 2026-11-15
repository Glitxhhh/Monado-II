// [XCDE] LINUX-097: explicit two-output refresh policy; no game-clock changes.
#ifndef MONADO_FRAMEGEN_POLICY_H
#define MONADO_FRAMEGEN_POLICY_H
#include <stdbool.h>
#include <math.h>
#include <stdint.h>

static inline float fg_policy_base_fps(int configured, float display_hz, bool match) {
    float selected = configured > 0 ? (float)configured : 0.f;
    if (!match || !isfinite(display_hz) || display_hz < 59.f || display_hz > 1000.f)
        return selected;
    float ceiling = display_hz * .5f;
    return selected > 0.f && selected < ceiling ? selected : ceiling;
}
// [XCDE] LINUX-099: Vulkan permits N-min+1 simultaneously acquired images.
// Two old outputs plus the next producer pair therefore need min+3 images.
static inline uint32_t fg_policy_image_count(uint32_t minimum, uint32_t maximum, bool enabled) {
    uint64_t count = (uint64_t)minimum + (enabled ? 3 : 1);
    if (enabled && count < 4) count = 4;
    if (maximum && count > maximum) count = maximum;
    if (count > 8) count = 8;
    return (uint32_t)count;
}
static inline uint32_t fg_policy_acquire_budget(uint32_t actual, uint32_t minimum) {
    return actual >= minimum ? actual - minimum + 1 : 0;
}
#endif
