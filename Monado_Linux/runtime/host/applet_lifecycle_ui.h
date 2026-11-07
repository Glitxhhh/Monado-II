// [XCDE] LINUX-007: native window close participates in the guest exit lock.
#pragma once
#include <SDL3/SDL_events.h>
#include "../platform.h"
#include "../services/am.h"

// Poll for every event and on idle: continuous input must not postpone the
// host-monotonic exit deadline. Desktop focus/minimize events intentionally do
// not change Horizon applet ownership or suspend guest execution.
static inline bool applet_ui_should_exit(const SDL_Event *event) {
    uint64_t now = plat_now_ns();
    if (event && event->type == SDL_EVENT_QUIT) am_request_host_exit(now);
    return am_host_exit_ready(now);
}
