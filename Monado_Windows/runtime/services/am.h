// [XCDE] LINUX-008: native close coordination for the always-active applet.
#pragma once
#include <stdbool.h>
#include <stdint.h>

// UI or guest hook thread: request close once, preserving guest execution while
// LockExit is held. Pass the same host monotonic nanosecond clock to both functions.
// These functions take g_klock internally; never call with that lock held.
// They do not exit, stop threads, commit saves or invoke UI callbacks.
void am_request_host_exit(uint64_t now_ns);
bool am_host_exit_ready(uint64_t now_ns);
