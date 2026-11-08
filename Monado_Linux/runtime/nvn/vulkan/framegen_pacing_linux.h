// [XCDE] LINUX-020: one monotonic half-frame wait, after the generated image is GPU ready/presented.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <math.h>
#include <errno.h>
#include <time.h>

static uint64_t fg_pacing_now(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) return 0;
    return (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
}
// Pacing has a target budget; the SDK still receives the actual measured delta.
// Do not turn invalid measurements into valid intervals or guess an uncapped FPS.
static float fg_pacing_interval_ms(float measured_ms, int target_fps) {
    if (!isfinite(measured_ms) || measured_ms < 1.f || measured_ms > 100.f || target_fps <= 0) return measured_ms;
    float target_ms = 1000.f / (float)target_fps;
    return measured_ms < target_ms ? measured_ms : target_ms;
}
static bool fg_pacing_deadline(uint64_t presented, float frame_ms, uint64_t *deadline) {
    if (!presented || !deadline || !isfinite(frame_ms) || frame_ms < 1.f || frame_ms > 100.f) return false;
    uint64_t half = (uint64_t)((double)frame_ms * 500000.);
    if (UINT64_MAX - presented < half) return false;
    *deadline = presented + half;
    return true;
}
static bool fg_pacing_wait(uint64_t deadline) {
    if (!deadline) return false;
    struct timespec time = {(time_t)(deadline / 1000000000ull), (long)(deadline % 1000000000ull)};
    int result;
    do { result = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &time, NULL); } while (result == EINTR);
    return result == 0;
}
// The render worker's deliberate idle accounting needs observed time, never
// the requested half-period: preparation may have consumed that budget already.
static bool fg_pacing_wait_elapsed(uint64_t deadline, uint64_t *elapsed_ns) {
    if (!elapsed_ns) return fg_pacing_wait(deadline);
    *elapsed_ns = 0;
    if (!deadline) return false;
    uint64_t before = fg_pacing_now();
    bool result = fg_pacing_wait(deadline);
    uint64_t after = fg_pacing_now();
    if (before && after >= before) *elapsed_ns = after - before;
    return result;
}
