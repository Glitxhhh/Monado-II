// Authored Windows composition policy. Nominal monitor Hz does not establish
// the actual FIFO presentation cadence under DWM (including mixed-Hz displays).
#pragma once
#include <stdbool.h>
#include <math.h>
static inline int monado_windows_repeat_count(bool legacy_requested,bool fifo,bool generation,
                                               float nominal_hz,float real_fps) {
    if(!legacy_requested || !fifo || generation || !isfinite(nominal_hz) || !isfinite(real_fps) ||
       nominal_hz<=0.f || real_fps<=0.f)return 1;
    float ratio=nominal_hz/real_fps;
    if(!isfinite(ratio)||ratio<1.5f||ratio>8.5f)return 1;
    int count=(int)lroundf(ratio);
    return count>=2&&count<=8&&fabsf(ratio-count)<=.01f*count?count:1;
}
