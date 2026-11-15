// [XCDE] LINUX-103: anisotropy is an optional color-detail override, never a
// change to integer/depth/point/nonmip/reduction sampling semantics.
#pragma once
#include <stdbool.h>
#include <math.h>
static inline float rt_sampler_anisotropy(unsigned choice, unsigned min_filter,
    unsigned mag_filter, unsigned compare, unsigned reduction, bool color,
    bool integer, bool shadow, unsigned levels, float device_limit) {
    if(choice<1 || choice>4 || !color || integer || shadow || levels<2 ||
       mag_filter!=1 || (min_filter!=3 && min_filter!=5) || compare || reduction ||
       !isfinite(device_limit) || device_limit<2.f)return 0.f;
    return fminf((float)(1u<<choice),device_limit);
}
