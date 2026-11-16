// [XCDE] Native FFX swapchain owns output pacing. VSync keeps every integrated image in FIFO at all saved caps.
#pragma once
#include <stdbool.h>
static inline bool windows_present_fifo(bool explicit_mode,bool vsync,int fps,bool integrated,
    bool period_override,bool hidden,bool hidden_test,bool deterministic) {
    return !explicit_mode&&vsync&&(integrated||(fps>0&&fps<=60))&&!period_override&&
        (!hidden||hidden_test)&&!deterministic;
}
