// Diagnostics shared by the runtime (M3): deterministic mode, per-frame hash, input record/replay, strict mode.
//   XCDE_DETERMINISTIC=1   guest-visible clocks derive from the presented-frame count, fixed random seed, vsync gated by frames
//   XCDE_FRAME_HASH=<file> one line per presented frame: "<frame> <fnv1a64 of the window image>" (default with the above:
//                          build/frame_hashes.txt)
//   XCDE_INPUT_RECORD=<f>  record the pad state the game reads, one line per change: "<frame> <buttons hex> <lx> <ly> <rx> <ry>"
//   XCDE_INPUT_REPLAY=<f>  play such a file back (a step function of the presented-frame number); overrides host input
//   XCDE_STRICT=1          a not-implemented SVC / IPC command / HLE function stops the game with a report (Art. VI)
#pragma once
#include <stdbool.h>
#include <stdint.h>

bool diag_deterministic(void);
// XCDE_SHOTS="<frame>:<path>,<frame>:<path>": window captures of the given presented frames (debug; never commit them)
const char *diag_shot_path(uint64_t frame);
bool diag_strict(void);

// Presented-frame counter, advanced by the renderer once per frame (device.c).
void diag_frame_tick(uint64_t frame);
uint64_t diag_frame(void);

// Guest-visible monotonic time in nanoseconds in deterministic mode (frame * 33.3 ms + 1 us per query inside a frame).
uint64_t diag_now_ns(void);
uint64_t diag_random64(void);  // seeded in deterministic mode, OS entropy otherwise

// Per-frame hash. The renderer asks whether it must read the window image back for this frame.
bool diag_hash_wanted(void);
void diag_frame_hash(uint64_t frame, const uint8_t *rgba, uint32_t w, uint32_t h);

// Pad state hook (runtime/hle/input.c fill_state): records and/or replaces the state. frame = presented frames so far.
void diag_input(uint64_t frame, uint64_t *buttons, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry);

// Strict mode: `what` names the missing piece ("svc 0x41", "audren cmd 5", ...). Logs always; in strict mode also stops
// the game with the guest + host stack (rt_fatal_trace). Returns normally outside strict mode.
void diag_unimplemented(const char *kind, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
// Coverage for docs/hle_audit.md: XCDE_HLE_COVERAGE=<file> writes "<kind> <name> <count>" at exit.
void diag_hle_hit(const char *kind, const char *name);
// Usage census for research.md (T034): XCDE_USAGE=<file> writes "<kind>\t<name>\t<count>" for every SVC, IPC command,
// HLE function, NVN proc address and memory request the game makes. diag_usage_on() is a cheap flag for hot paths.
bool diag_usage_on(void);
void diag_use(const char *kind, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void diag_use_svc(unsigned imm);

// Stall watchdog (T027): XCDE_STALL=<s> (default 5 in windowed builds) reports every thread after s seconds without a frame.
void diag_watchdog_start(void);

// Crash report (T028): the exception filter calls diag_crash_report(EXCEPTION_POINTERS *); main() hands "--minidump ..." to the helper.
int diag_minidump_helper(int argc, char **argv);
