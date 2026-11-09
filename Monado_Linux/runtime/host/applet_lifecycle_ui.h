// [XCDE] LINUX-007: native window close participates in the guest exit lock.
#pragma once
#include <SDL3/SDL_events.h>
#include "../platform.h"
#include "../rt.h"
#include "../services/am.h"

// Poll for every event and on idle: continuous input must not postpone the
// host-monotonic exit deadline. Desktop focus/minimize events intentionally do
// not change Horizon applet ownership or suspend guest execution.
static inline bool applet_ui_should_exit(const SDL_Event *event) {
    uint64_t now = plat_now_ns();
    if (event && event->type == SDL_EVENT_QUIT) am_request_host_exit(now);
    return am_host_exit_ready(now);
}

// Only the UI owner calls this, after AM permits closing. Guest safe points
// must stop and join every producer before a queue snapshot can be drained.
// Finish directly: exit() can run later-loaded graphics-library destructors
// before the older atexit handler while the render worker is still using them.
// The renderer callbacks keep this ordering independent of the backend.
static inline void applet_ui_finish_exit(void (*drain_renderer)(void), void (*finish_process)(void)) {
    kernel_stop_threads(NULL);
    drain_renderer();
    finish_process();
}
