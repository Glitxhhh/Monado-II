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
