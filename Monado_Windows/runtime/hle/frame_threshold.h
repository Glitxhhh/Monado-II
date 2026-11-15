// Monado II: integer-only native-frame thresholds. No generated/presented frames.
#ifndef MONADO_FRAME_THRESHOLD_H
#define MONADO_FRAME_THRESHOLD_H
#include <stdint.h>
#include <limits.h>

// The original compare is strictly > n. Retain its duration at fractional rates
// without rounding the whole game's cadence to a multiple of 30 Hz. A measured
// integer-nanosecond period has <1 ns precision: round the divisor upward so an
// exact 120 Hz cadence does not gain a frame from truncation alone.
static inline uint32_t frame_threshold_gt(uint32_t n, uint64_t period_ns) {
    if (!period_ns || period_ns >= 33333333u) return n;
    uint64_t numerator = ((uint64_t)n + 1) * 1000000000ull;
    uint64_t divisor = 30 * (period_ns + 1);
    uint64_t frames = numerator / divisor + (numerator % divisor != 0);
    if (frames > UINT32_MAX) return UINT32_MAX - 1;
    return frames ? (uint32_t)(frames - 1) : n;
}

typedef struct FrameThresholdClock {
    uint64_t last_frame, last_ns, average_ns;
    int last_fps;
    int initialized;
} FrameThresholdClock;

static inline uint64_t frame_threshold_sample(FrameThresholdClock *clock,
        uint64_t frame, uint64_t now, int fps) {
    if (clock->initialized && frame == clock->last_frame && fps == clock->last_fps)
        return clock->average_ns;
    int discontinuity = !clock->initialized || fps != clock->last_fps ||
        frame <= clock->last_frame || now <= clock->last_ns ||
        now - clock->last_ns >= 250000000ull;
    uint64_t dt = discontinuity ? 0 : (now - clock->last_ns) / (frame - clock->last_frame);
    if (discontinuity) clock->average_ns = 1000000000ull / 60;
    else clock->average_ns = (clock->average_ns * 19 + dt) / 20;
    clock->initialized = 1;
    clock->last_frame = frame;
    clock->last_ns = now;
    clock->last_fps = fps;
    return clock->average_ns;
}
#endif
