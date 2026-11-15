// [XCDE] LINUX-103: pure bounds/target policy for the existing DRS controller.
#pragma once
#include <math.h>
#include "settings.h"
static inline void xcde_dynamic_bounds(const Settings *s,double output_h,double *minimum,double *maximum) {
    double high=s->dynamic_max_height>0 ? fmin(output_h,s->dynamic_max_height) : output_h;
    high=fmax(1.,high);
    double low=s->dynamic_min_height>0 ? fmin(high,s->dynamic_min_height) : .5*high;
    *minimum=fmax(1.,low); *maximum=high;
}
static inline double xcde_dynamic_target_ms(const Settings *s) {
    // Match the native render budget, never the generated/output frame rate.
    int fps=s->fps>0 ? s->fps : 60;
    return 1000.0/(fps<30 ? 30 : fps);
}
