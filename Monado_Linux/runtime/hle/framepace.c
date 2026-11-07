// Frame pacing: the game's speed doesn't depend on the frame rate.
// The game advances its world each frame by its frame period (16.67 ms) times the swap interval its frame timer
// (f_700000c910) reads from the system object - normally 1. When a frame takes longer than a vsync (GPU-bound at
// 32:9, a hitch), the next frame starts a vsync later but still advanced only 16.67 ms: slow motion (30 fps = half
// speed). tools/patches.py FRAME_STEPS_SITE routes that read here: we return how many vsyncs really passed since
// the last frame, so the world advances by the time that passed. The game's own waiting is unaffected (it reads
// the interval nowhere else). Shared by the macOS and Windows builds.
// FINDING (2026-10-09): forcing 2 steps did NOT change Link's walking speed - gameplay is a true fixed step per
// frame; this timer feeds something else. Kept for research, off by default.
//   LARECOMP_FRAMEPACE=1     hand the timer the vsyncs elapsed (default: the game's own value)
//   LARECOMP_FRAME_STEPS_MAX maximum steps per frame (default 4: after a long stall the game doesn't jump ahead)
//   LARECOMP_FORCE_STEPS=<n> (test) always n
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../platform.h"
#include "../settings.h"

// [XCDE] LINUX-013: passive hooks; compile-time disabled in ordinary builds.
#include "foley_trace.inc"

// [XCDE] LINUX-013: a phase-preserving clip change seeks the new selected
// control to old_time * new_duration / old_duration. The loop detector otherwise
// compares that new clock with the previous clip's time and can invent a wrap.
// Rebase only after this seek, on the same animation update thread. Keep the
// existing generation, marker history, forced outgoing events and playback state.
void xcde_animation_phase_rebase(uint64_t animation, uint32_t layer) {
    if (!animation || layer >= 2) return;
    uint64_t state = animation + 0x20 + 0x50 * layer;
    uint8_t selected;
    uint64_t entries, count, control;
    memcpy(&selected, (const void *)(uintptr_t)(state + 0x18), sizeof selected);
    memcpy(&entries, (const void *)(uintptr_t)(state + 0x20), sizeof entries);
    memcpy(&count, (const void *)(uintptr_t)(state + 0x28), sizeof count);
    if (!entries || selected >= count) return;
    memcpy(&control, (const void *)(uintptr_t)(entries + 0x30 * selected + 0x18), sizeof control);
    if (!control) return;
    uint32_t cursor;
    memcpy(&cursor, (const void *)(uintptr_t)(control + 0x80), sizeof cursor);
    memcpy((void *)(uintptr_t)(state + 0x10), &cursor, sizeof cursor);
}

extern volatile uint64_t g_vsync_count;  // runtime/hle/vi.c
uint64_t g_frame_steps_late;             // (stats) frames that advanced more than one step

uint32_t rt_frame_steps(uint32_t interval) {
    static int on = -1, max_steps, forced;
    if (on < 0) {
        const char *e = ENV("LARECOMP_FRAMEPACE");
        on = e ? atoi(e) != 0 : 0;  // off by default: the world (Link's movement) doesn't use this step (tested)
        e = ENV("LARECOMP_FRAME_STEPS_MAX");
        max_steps = e ? atoi(e) : 4;
        if (max_steps < 1) max_steps = 1;
        e = ENV("LARECOMP_FORCE_STEPS");
        forced = e ? atoi(e) : 0;
    }
    static int ncall;
    if (ENV("LARECOMP_LOG_FRAMEPACE") && ncall++ % 120 == 0) LOG("[framepace] call %d interval %u vsync %llu\n", ncall, interval, (unsigned long long)g_vsync_count);
    if (forced > 0) return (uint32_t)forced;
    static uint64_t last;
    uint64_t now = __atomic_load_n(&g_vsync_count, __ATOMIC_RELAXED);
    uint64_t steps = last ? now - last : 1;
    last = now;
    if (!on) return interval;
    if (steps < interval) steps = interval;  // (the game's own 30 fps mode: it waits that many vsyncs anyway)
    if (steps < 1) steps = 1;
    if (steps > (uint64_t)max_steps) steps = (uint64_t)max_steps;
    if (steps > 1) g_frame_steps_late++;
    return (uint32_t)steps;
}

// [XCDE] T157: the game's frame limiter (games/xcde/patches.py) spins on the system tick until its frame time is up.
// xcde_limiter_done marks each frame boundary (the limiter returning); xcde_limiter_spin, at each turn of the wait loop,
// sleeps on a high-resolution timer while the predicted end (last boundary + shortest of the last 8 intervals, when
// they agree within 3 ms) is more than 1.5 ms away, so only the last stretch spins and the frame ends when it did.
// If the game ever leaves the wait right after one of our sleeps, its target was earlier than predicted (another frame
// rate): what was learned is dropped and the next frames spin as before. Deterministic runs: untouched (virtual time).
// XCDE_LIMITER_SLEEP=0: off. Only the limiter's thread calls these.
#include "../diag/diag.h"
#include "../platform.h"

static uint64_t lim_last, lim_iv[8];
static int lim_n, lim_turns_since_sleep = -1;
uint64_t g_limiter_slept_ns;  // (stats)

// [XCDE] frame rate option (launcher "FPS": xcde.ini fps = 30 | 60 | 120 | 144 | ... | unlimited). The game's limiter
// (f_70005bcfe0) waits until the frame took interval x display period (1/60 s) x the constant at 0x70009d3dcc (0.997),
// where interval is the int behind the pointer at 0x7000b92ea0 (2 = its 30 fps; 0 reads as 2). 60 fps = interval 1;
// above, interval 1 and the constant scaled by 60 / target (unlimited: x 0.02). The world, the in-game clock and
// cutscenes advance by the time that passed (deterministic runs at 60 and 120 with the route time-scaled: same clock and
// positions at equal times). Tests: XCDE_FPS_INTERVAL=<n>, XCDE_FPS_PERIOD_SCALE=<f>, XCDE_FPS_AFTER_S / _FRAMES.
// (A display / driver vsync or an overlay's frame limit, e.g. RivaTuner's, still caps the rate.)
// [XCDE] T224: is a cutscene playing? Global state compared across field / menu / battle and two cutscenes (Dickson's,
// Vangarre's; build/snap_*.bin): the cinematic camera's block at 0x7000b70628 holds 10 instead of 60 (with
// its neighbours: 120 -> 10, floats 20 -> 0.5, 10 -> 5.5), and 0x70012d47b4 is 0 instead of 0x72 - both, only while
// a cutscene runs. Changes are logged.
bool xcde_in_cutscene(void) {
    // (the camera block alone dropped out for shots with a field-like camera inside a cutscene: 0x70012b9410 - 0x7203 in
    // field / menu / battle, 0x7003 for the whole Dickson scene - and 0x70012d47b4 hold through them)
    uint32_t mode = *(volatile uint32_t *)(uintptr_t)0x70012b9410ull;
    bool now = *(volatile uint32_t *)(uintptr_t)0x70012d47b4ull == 0 && mode != 0 && !(mode & 0x200);
    // (one-frame flickers of 0x70012b9410 inside a scene: a change counts after 4 frames in a row; called once a frame)
    static bool in;
    static int streak;
    streak = now != in ? streak + 1 : 0;
    if (streak >= 4) { in = now; streak = 0; }
    static int last = -1;
    if (in != last) {
        extern uint64_t renderer_present_count(void);
        #define XW(a) (*(volatile uint32_t *)(uintptr_t)(a##ull))
        if (last >= 0)
            LOG("[cutscene] %s (frame %llu) | b70628 %x b70650 %x b706a0 %x b706f0 %x 12b9410 %x 12d47b4 %x 12d47bc %x\n", in ? "start" : "end",
                (unsigned long long)renderer_present_count(), XW(0x7000b70628), XW(0x7000b70650), XW(0x7000b706a0), XW(0x7000b706f0),
                XW(0x70012b9410), XW(0x70012d47b4), XW(0x70012d47bc));
        #undef XW
        last = in;
    }
    return in;
}
// The option (launcher "Cutscenes") holds a cutscene at 30 fps: the game's poses there change at 30, and with the
// camera at 60 over them the motion looks uneven (the user: cutscenes feel like 30, Dickson's ponytail shakes).
// cutscenes = 30fg (launcher "Frame generation in cutscenes only"): the same, plus frame generation only there -
// gameplay keeps its FPS without it (the user plays locked at 60; generated frames there would look laggy).
static bool g_cut30;
bool xcde_cutscene_30(void) { return g_cut30; }
// the frame rate the window is presented at (device.c repeat presents): 30 while a cutscene is held at 30
int xcde_present_fps(void) { return g_cut30 ? 30 : g_settings.fps; }

// [XCDE] LINUX-011: MPGUI initialized its animation step to 1/30 second and
// never updated it, so 165 GUI updates advanced 5.5 seconds of animation. The
// checked hook at the GUI's own update callback runs before its child traversal,
// on the same thread that already applies the GUI's pending speed changes.
// Use the game's frame clock (the source used by f_70005ea460), not a host timer,
// requested FPS, present count, or the paused/scaled world-layer UpdateInfo.
// Match the framework's existing two-reference-period hitch cap. Keep the
// global GUI speed, per-node speed and pause checks in the original consumer.
void xcde_gui_frame_delta(uint64_t gui) {
    if (!gui) return;
    uint64_t slot, clock = 0;
    memcpy(&slot, (const void *)(uintptr_t)0x7000b93458ull, sizeof slot);
    if (slot) memcpy(&clock, (const void *)(uintptr_t)slot, sizeof clock);
    float seconds = 0, reference_fps;
    if (clock) memcpy(&seconds, (const void *)(uintptr_t)(clock + 0x2034), sizeof seconds);
    memcpy(&reference_fps, (const void *)(uintptr_t)(gui + 0x6c), sizeof reference_fps);
    if (!isfinite(seconds) || seconds < 0 || !isfinite(reference_fps) || reference_fps <= 0) {
        LOG("[timing] invalid GUI frame clock: seconds=%g reference_fps=%g\n",
            (double)seconds, (double)reference_fps);
        abort();
    }
    float maximum = 2.0f / reference_fps;
    if (seconds > maximum) seconds = maximum;
    memcpy((void *)(uintptr_t)(gui + 0x70), &seconds, sizeof seconds);
    static bool logged;
    if (!logged && clock) {
        logged = true;
        LOG("[timing] GUI animation uses engine elapsed time (reference %g Hz)\n", (double)reference_fps);
    }
}

// [XCDE] LINUX-012: navigation's held-duration updater reads UpdateInfo's
// reference period (+4), which stays 1/30 even at high FPS. Its repeat consumer
// already compares durations in seconds; supply the existing engine elapsed
// seconds at just those reads. Raw HID, press/release masks and borrowed
// UpdateInfo stay intact. Use the same two-reference-period cap as the framework
// and GUI, independently of world-layer pause/speed. Bits in/out preserve the
// original scalar LDR's complete 128-bit destination clearing in generated code.
uint32_t xcde_repeat_delta_bits(uint32_t reference_bits) {
    float reference, seconds = 0;
    memcpy(&reference, &reference_bits, sizeof reference);
    uint64_t slot, clock = 0;
    memcpy(&slot, (const void *)(uintptr_t)0x7000b93458ull, sizeof slot);
    if (slot) memcpy(&clock, (const void *)(uintptr_t)slot, sizeof clock);
    if (clock) memcpy(&seconds, (const void *)(uintptr_t)(clock + 0x2034), sizeof seconds);
    if (!isfinite(reference) || reference <= 0 || !isfinite(seconds) || seconds < 0) {
        LOG("[timing] invalid repeat frame clock: seconds=%g reference=%g\n",
            (double)seconds, (double)reference);
        abort();
    }
    float maximum = 2.0f * reference;
    if (seconds > maximum) seconds = maximum;
    uint32_t result;
    memcpy(&result, &seconds, sizeof result);
    static unsigned logged;
    if (clock && !__atomic_load_n(&logged, __ATOMIC_RELAXED) &&
        !__atomic_exchange_n(&logged, 1, __ATOMIC_RELAXED)) {
        LOG("[timing] input repeat uses engine elapsed time\n");
    }
    return result;
}

// [XCDE] LINUX-014: speed up only the repeat consumers' local duration views.
// The shared held timers keep real elapsed seconds for all other hold queries;
// raw input, tap/release edges, GUI/world/animation clocks remain unchanged.
uint32_t xcde_navigation_duration_bits(uint32_t duration_bits) {
    if (g_settings.menu_navigation_speed != MENU_NAV_FAST) return duration_bits;
    float duration;
    memcpy(&duration, &duration_bits, sizeof duration);
    // Inactive/sentinel values retain their original bits and comparison policy.
    if (!isfinite(duration) || duration <= 0) return duration_bits;
    duration *= 2.0f;
    memcpy(&duration_bits, &duration, sizeof duration_bits);
    return duration_bits;
}

static void fps_interval_force(void) {
    static int want = -2;
    static float scale = -1;
    static uint64_t after, after_frames, frames;
    if (want == -2) {
        int fps = g_settings.fps;
        const char *e = ENV("XCDE_FPS_INTERVAL");
        want = e ? atoi(e) : fps == 30 ? -1 : 1;
        e = ENV("XCDE_FPS_PERIOD_SCALE");
        scale = e ? (float)atof(e) : fps == 0 ? 0.02f : fps > 60 ? 60.0f / (float)fps : 0;
        e = ENV("XCDE_FPS_AFTER_S"); after = plat_now_ns() + (e ? (uint64_t)(atof(e) * 1e9) : 0);
        e = ENV("XCDE_FPS_AFTER_FRAMES"); after_frames = e ? strtoull(e, NULL, 10) : 0;
    }
    frames++;
    g_cut30 = g_settings.cutscene_mode && !ENV("XCDE_FPS_INTERVAL") && xcde_in_cutscene();
    if (want < 0 || plat_now_ns() < after || frames < after_frames) return;
    uint32_t *p = (uint32_t *)(uintptr_t)*(volatile uint64_t *)(uintptr_t)0x7000b92ea0ull;
    if (!p) return;
    // [XCDE] LINUX-011: a fractional high-FPS period cannot represent the native
    // cutscene period by rounding an integer interval (165 gave nominal 27.5 Hz).
    // Keep the original constant and select the exact pair for each mode. This
    // also handles entering a cutscene before the first high-FPS constant write.
    volatile float *k = (volatile float *)(uintptr_t)0x70009d3dccull;
    static float original_period;
    static bool have_period;
    if (!have_period) { original_period = *k; have_period = true; }
    float period = original_period * (!g_cut30 && scale > 0 ? scale : 1.0f);
    if (*k != period) {
        uint64_t page = (uint64_t)(uintptr_t)k & ~0xFFFull;
        float previous = *k;
        if (plat_protect(page, 0x1000, PLAT_PROT_RW) != 0) {
            LOG("[fps] cannot make limiter constant writable\n");
            abort();
        }
        *k = period;
        if (plat_protect(page, 0x1000, PLAT_PROT_READ) != 0) {
            LOG("[fps] cannot restore limiter constant protection\n");
            abort();
        }
        LOG("[fps] limiter period constant %g -> %g\n", (double)previous, (double)period);
    }
    if (g_cut30) {
        *p = 2;  // native 30 Hz: two unscaled display periods
        return;
    }
    static int logged;
    if (!logged && *p != (uint32_t)want) { logged = 1; LOG("[fps] frame interval %u -> %d\n", *p, want); }
    {   // XCDE_FPS_LOG=1: how often the game set its interval back since the last frame (per 120 frames)
        static int lg = -1, reset;
        static uint64_t n;
        if (lg < 0) lg = ENV("XCDE_FPS_LOG") != NULL;
        if (lg) { reset += *p != (uint32_t)want; if (++n % 120 == 0) { LOG("[fps] frame %llu: interval set back %d of 120 frames\n", (unsigned long long)n, reset); reset = 0; } }
    }
    *p = (uint32_t)want;
}

// [XCDE] T224 research: XCDE_MEMWATCH=<frame>:<lo hex>-<hi hex>[,<lo>-<hi>...] - at 8 consecutive frame boundaries from
// that presented frame (this thread: the game's limiter, so each snapshot is a whole game frame), the committed words
// of those guest ranges; then build/memwatch.txt lists the 32-bit words that alternate between two values every frame
// or change only every other frame (a 30 Hz gate or clock inside a 60 fps frame).
#ifdef _WIN32
#include <windows.h>
#endif
static void memwatch(void) {
#ifdef _WIN32
    static int st = -1;
    static uint64_t at, rlo[8], rhi[8];
    static int nr;
    enum { N = 8 };
    static struct { uint64_t a, len; } reg[4096];
    static int nreg;
    static uint8_t *snap[N];
    static uint64_t total;
    static int taken;
    if (st < 0) {
        st = 0;
        const char *e = getenv("XCDE_MEMWATCH");
        if (!e) return;
        char *q;
        at = strtoull(e, &q, 10);
        while (*q == ':' || *q == ',') {
            q++;
            if (nr < 8) { rlo[nr] = strtoull(q, &q, 16); if (*q == '-') q++; rhi[nr] = strtoull(q, &q, 16); nr++; }
        }
        st = nr ? 1 : 0;
    }
    if (st != 1) return;
    extern uint64_t renderer_present_count(void);
    if (renderer_present_count() < at) return;
    if (!nreg) {  // (the committed, read-write regions, once)
        for (int r = 0; r < nr; r++)
            for (uint64_t a = rlo[r]; a < rhi[r] && nreg < 4096;) {
                MEMORY_BASIC_INFORMATION mi;
                if (!VirtualQuery((void *)(uintptr_t)a, &mi, sizeof mi) || !mi.RegionSize) break;
                uint64_t b = (uint64_t)(uintptr_t)mi.BaseAddress, e2 = b + mi.RegionSize;
                if (b < a) b = a;
                if (e2 > rhi[r]) e2 = rhi[r];
                if (mi.State == MEM_COMMIT && (mi.Protect & 0xFF) == PAGE_READWRITE && !(mi.Protect & PAGE_GUARD) && e2 > b) {
                    reg[nreg].a = b; reg[nreg].len = e2 - b; total += e2 - b; nreg++;
                }
                a = e2 > a ? e2 : a + 0x1000;
            }
        LOG("[memwatch] %d regions, %.1f MB from frame %llu\n", nreg, total / 1048576.0, (unsigned long long)renderer_present_count());
        for (int i = 0; i < N; i++) snap[i] = malloc(total);
        if (!snap[N - 1]) { st = 0; return; }
    }
    uint64_t o = 0;
    for (int i = 0; i < nreg; i++) { memcpy(snap[taken] + o, (const void *)(uintptr_t)reg[i].a, reg[i].len); o += reg[i].len; }
    if (++taken < N) return;
    st = 2;
    FILE *f = fopen("build/memwatch.txt", "w");
    int found = 0;
    o = 0;
    for (int i = 0; i < nreg && f; i++) {
        for (uint64_t k = 0; k + 4 <= reg[i].len; k += 4) {
            uint32_t v[N];
            for (int j = 0; j < N; j++) memcpy(&v[j], snap[j] + o + k, 4);
            bool alt = v[0] != v[1], step = true, every = true;
            for (int j = 2; j < N; j++) alt &= v[j] == v[j - 2];
            // changes only every other frame: v1==v0, v2!=v1, v3==v2 ... (either phase)
            int ch[N - 1], nch = 0;
            for (int j = 1; j < N; j++) { ch[j - 1] = v[j] != v[j - 1]; nch += ch[j - 1]; }
            for (int j = 2; j < N; j++) step &= ch[j - 1] != ch[j - 2];
            for (int j = 1; j < N; j++) every &= ch[j - 1];
            // [XCDE] T224: a float that grows by the same amount every frame (a clock: seconds or frames)
            bool clock = every;
            float fv0, fv1;
            memcpy(&fv0, &v[0], 4); memcpy(&fv1, &v[1], 4);
            float d0 = fv1 - fv0;
            if (clock && d0 > 0.004f && d0 < 2.0f && fv0 > -1e6f && fv0 < 1e6f) {
                for (int j = 2; j < N && clock; j++) {
                    float a0, a1;
                    memcpy(&a0, &v[j - 1], 4); memcpy(&a1, &v[j], 4);
                    clock = fabsf((a1 - a0) - d0) <= d0 * 0.02f;
                }
            } else clock = false;
            static int clk_only = -1, clk_only_steps = -1;
            if (clk_only < 0) clk_only = getenv("XCDE_MEMWATCH_CLK") != NULL;  // [XCDE] T224: only the clocks
            if (clk_only) { alt = false; if (clk_only_steps < 0) clk_only_steps = getenv("XCDE_MEMWATCH_STEPS") != NULL; if (!clk_only_steps) step = false; }
            if (clk_only && step) {  // steps mode: only counters (every change the same increase, integer or float)
                int64_t di = 0; float df = 0; bool iok = true, fok = true;
                for (int j = 1; j < N; j++) {
                    if (v[j] == v[j - 1]) continue;
                    int64_t d = (int64_t)v[j] - (int64_t)v[j - 1];
                    float a0, a1; memcpy(&a0, &v[j - 1], 4); memcpy(&a1, &v[j], 4);
                    float e = a1 - a0;
                    if (!di) di = d; else iok &= d == di;
                    if (df == 0) df = e; else fok &= fabsf(e - df) <= fabsf(df) * 0.01f;
                }
                iok &= di > 0 && di <= 4;
                fok &= df > 0.01f && df < 4.0f;
                step = iok || fok;
            }
            if ((alt || (step && nch >= 3) || clock) && found < 20000) {
                found++;
                fprintf(f, "%llx %s", (unsigned long long)(reg[i].a + k), alt ? "alt " : clock ? "clk " : "half");
                for (int j = 0; j < N; j++) fprintf(f, " %08x", v[j]);
                fputc('\n', f);
            }
        }
        o += reg[i].len;
    }
    if (f) fclose(f);
    LOG("[memwatch] %d words written to build/memwatch.txt\n", found);
    if (getenv("XCDE_MEMWATCH_AUTO")) {  // watch the first word that changes every other frame and holds a world-sized float
        o = 0;
        for (int i = 0; i < nreg; i++) {
            for (uint64_t k = 0; k + 4 <= reg[i].len; k += 4) {
                float fv[N]; uint32_t v[N];
                for (int j = 0; j < N; j++) { memcpy(&v[j], snap[j] + o + k, 4); memcpy(&fv[j], &v[j], 4); }
                static int cnt = -1;  // [XCDE] T224: XCDE_MEMWATCH_AUTO=cnt: an integer counter (+1 every other frame, < 3000)
                if (cnt < 0) cnt = !strcmp(getenv("XCDE_MEMWATCH_AUTO"), "cnt");
                if (cnt) {
                    if (reg[i].a + k < 0x7200000000ull || v[0] < 2 || v[0] >= 3000 || v[N - 1] - v[0] < 3 || v[N - 1] - v[0] > 4) continue;
                    bool ok = true;
                    for (int j = 1; j < N; j++) ok &= v[j] == v[j - 1] || v[j] == v[j - 1] + 1;
                    for (int j = 2; j < N; j++) ok &= (v[j] != v[j - 1]) != (v[j - 1] != v[j - 2]);
                    if (!ok) continue;
                    extern void (*diag_watch_hook)(uint64_t, uint64_t);
                    uint64_t a = reg[i].a + k;
                    LOG("[memwatch] auto-watching counter %llx (%u)\n", (unsigned long long)a, v[0]);
                    if (diag_watch_hook) diag_watch_hook(a, a + 4);
                    i = nreg; break;
                }
                if (reg[i].a + k < 0x7200000000ull || !(fv[0] > 20 || fv[0] < -20) || !(fv[0] < 1e5f && fv[0] > -1e5f)) continue;
                bool step = true; int nch = 0;
                for (int j = 1; j < N; j++) nch += v[j] != v[j - 1];
                for (int j = 2; j < N; j++) step &= (v[j] != v[j - 1]) != (v[j - 1] != v[j - 2]);
                if (!step || nch < 3) continue;
                extern void (*diag_watch_hook)(uint64_t, uint64_t);
                uint64_t a = reg[i].a + k;
                LOG("[memwatch] auto-watching %llx (%g)\n", (unsigned long long)a, (double)fv[0]);
                if (diag_watch_hook) diag_watch_hook(a, a + 4);
                i = nreg; break;
            }
            if (i < nreg) o += reg[i].len;
        }
    }
#endif
}

// [XCDE] diagnostics: XCDE_STATESNAP="<frame>:<name>,..." writes the game's global data (0x7000a00000-0x7001400000,
// committed pages only, zeros elsewhere) to build/snap_<name>.bin at those frames - to find state flags by
// comparing moments (e.g. field / menu / cutscene).
static void statesnap(void) {
#ifdef _WIN32
    static int n = -1;
    static uint64_t at[16];
    static char nm[16][32];
    if (n < 0) {
        n = 0;
        const char *e = getenv("XCDE_STATESNAP");
        while (e && *e && n < 16) {
            char *q;
            at[n] = strtoull(e, &q, 10);
            if (*q != ':') break;
            q++;
            int k = 0;
            while (*q && *q != ',' && k < 31) nm[n][k++] = *q++;
            nm[n][k] = 0;
            n++;
            e = *q == ',' ? q + 1 : q;
        }
    }
    if (!n) return;
    extern uint64_t renderer_present_count(void);
    uint64_t f = renderer_present_count();
    for (int i = 0; i < n; i++) {
        if (at[i] != f) continue;
        at[i] = 0;
        const uint64_t lo = 0x7000a00000ull, hi = 0x7001400000ull;
        uint8_t *buf = calloc(1, hi - lo);
        for (uint64_t a = lo; a < hi;) {
            MEMORY_BASIC_INFORMATION mi;
            if (!VirtualQuery((void *)(uintptr_t)a, &mi, sizeof mi) || !mi.RegionSize) break;
            uint64_t b = (uint64_t)(uintptr_t)mi.BaseAddress, e2 = b + mi.RegionSize;
            if (b < a) b = a;
            if (e2 > hi) e2 = hi;
            if (mi.State == MEM_COMMIT && (mi.Protect & 0xFF) == PAGE_READWRITE) memcpy(buf + (b - lo), (const void *)(uintptr_t)b, e2 - b);
            a = e2 > a ? e2 : a + 0x1000;
        }
        char path[128];
        snprintf(path, sizeof path, "build/snap_%s.bin", nm[i]);
        FILE *fo = fopen(path, "wb");
        if (fo) { fwrite(buf, 1, hi - lo, fo); fclose(fo); }
        free(buf);
        LOG("[statesnap] frame %llu -> %s\n", (unsigned long long)f, path);
    }
#endif
}

// [XCDE] diagnostics: XCDE_DELTAFIND="<f0>,<f1>:<d1>,<f2>:<d2>,..." with XCDE_DELTAFIND_RANGE=<lo>-<hi> (hex): the
// bytes of guest memory that hold a small value (< 16) at frame f0 and change by d1 at f1, d2 at f2... (relative to
// the previous step) - e.g. the arts palette cursor (+1 / +1 / -1 with d-pad right / right / left). One copy of the
// range is kept; survivors are logged at the end (at most 64).
static void deltafind(void) {
#ifdef _WIN32
    static int st = -1, nstep;
    static uint64_t at[8];
    static int dl[8];
    static uint64_t lo, hi;
    static uint8_t *prev;
    static uint32_t *cand;
    static size_t ncand;
    if (st < 0) {
        st = 0;
        const char *e = getenv("XCDE_DELTAFIND"), *r = getenv("XCDE_DELTAFIND_RANGE");
        if (!e || !r || sscanf(r, "%llx-%llx", (unsigned long long *)&lo, (unsigned long long *)&hi) != 2 || hi <= lo) return;
        while (*e && nstep < 8) {
            char *q;
            at[nstep] = strtoull(e, &q, 10);
            dl[nstep] = 0;
            if (*q == ':') dl[nstep] = (int)strtol(q + 1, &q, 10);
            nstep++;
            e = *q == ',' ? q + 1 : q;
            if (q == e && !*q) break;
        }
        st = nstep >= 2 ? 1 : 0;
    }
    if (st != 1) return;
    extern uint64_t renderer_present_count(void);
    uint64_t f = renderer_present_count();
    static int step;
    if (f != at[step]) return;
    // read the committed pages of the range into a fresh buffer (zeros elsewhere)
    uint8_t *cur = calloc(1, hi - lo);
    if (!cur) { st = 0; return; }
    for (uint64_t a = lo; a < hi;) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery((void *)(uintptr_t)a, &mi, sizeof mi) || !mi.RegionSize) break;
        uint64_t b = (uint64_t)(uintptr_t)mi.BaseAddress, e2 = b + mi.RegionSize;
        if (b < a) b = a;
        if (e2 > hi) e2 = hi;
        if (mi.State == MEM_COMMIT && (mi.Protect & 0xFF) == PAGE_READWRITE && !(mi.Protect & PAGE_GUARD)) memcpy(cur + (b - lo), (const void *)(uintptr_t)b, e2 - b);
        a = e2 > a ? e2 : a + 0x1000;
    }
    // XCDE_DELTAFIND_PAT="ABCBC": instead of deltas, the value at each step must equal the one at the earlier step
    // with the same letter and differ from the other letters' (for cursors that wrap)
    static const char *pat;
    static uint8_t (*vals)[8];
    if (step == 0) pat = getenv("XCDE_DELTAFIND_PAT");
    bool okv;
    if (step == 0) {
    } else if (step == 1) {
        size_t cap = (size_t)1 << 28;
        cand = malloc(cap * sizeof *cand);
        if (pat) vals = malloc(cap * sizeof *vals);
        for (uint64_t i = 0; i < hi - lo && ncand < cap; i++) {
            if (prev[i] >= 16 || cur[i] >= 16) continue;
            okv = pat ? ((pat[1] == pat[0]) == (cur[i] == prev[i])) : (int)cur[i] - (int)prev[i] == dl[1];
            if (!okv) continue;
            if (pat) { vals[ncand][0] = prev[i]; vals[ncand][1] = cur[i]; }
            cand[ncand++] = (uint32_t)i;
        }
    } else {
        size_t k = 0;
        for (size_t j = 0; j < ncand; j++) {
            uint32_t i = cand[j];
            if (pat) {
                okv = cur[i] < 16;
                for (int t = 0; t < step && okv; t++) okv = (pat[t] == pat[step]) == (vals[j][t] == cur[i]);
                if (!okv) continue;
                memcpy(vals[k], vals[j], 8);
                vals[k][step] = cur[i];
            } else if ((int)cur[i] - (int)prev[i] != dl[step]) continue;
            cand[k++] = i;
        }
        ncand = k;
    }
    LOG("[deltafind] frame %llu step %d (delta %d): %zu candidates\n", (unsigned long long)f, step, dl[step], ncand);
    free(prev);
    prev = cur;
    if (++step == nstep) {
        for (size_t j = 0; j < ncand && j < 64; j++)
            LOG("[deltafind]   %llx = %u (%u %u %u %u %u %u %u)\n", (unsigned long long)(lo + cand[j]), prev[cand[j]],
                vals ? vals[j][0] : 0, vals ? vals[j][1] : 0, vals ? vals[j][2] : 0, vals ? vals[j][3] : 0,
                vals ? vals[j][4] : 0, vals ? vals[j][5] : 0, vals ? vals[j][6] : 0);
        // the pointers to (up to 0x800 before) each survivor, in the range and the game's globals
        for (size_t j = 0; j < ncand && j < 4; j++) {
            uint64_t t = lo + cand[j];
            for (uint64_t p = (t & ~7ull), n = 0; p > t - 0x1000 && n < 8; p -= 8) {  // (the vtables before it)
                uint64_t v = *(const uint64_t *)(uintptr_t)p;
                if (v >= 0x7000b00000ull && v < 0x7000c00000ull) { LOG("[deltafind]     %llx: vtable %llx at -%llx\n", (unsigned long long)t, (unsigned long long)v, (unsigned long long)(t - p)); n++; }
            }
            uint64_t rg[2][2] = {{lo, hi}, {0x7000a00000ull, 0x7001400000ull}};
            for (int r = 0; r < 2; r++) {
                int nh = 0;
                for (uint64_t a = rg[r][0]; a < rg[r][1] && nh < 16;) {
                    MEMORY_BASIC_INFORMATION mi;
                    if (!VirtualQuery((void *)(uintptr_t)a, &mi, sizeof mi) || !mi.RegionSize) break;
                    uint64_t b = (uint64_t)(uintptr_t)mi.BaseAddress, e2 = b + mi.RegionSize;
                    if (b < a) b = a;
                    if (e2 > rg[r][1]) e2 = rg[r][1];
                    if (mi.State == MEM_COMMIT && (mi.Protect & 0xFF) == PAGE_READWRITE && !(mi.Protect & PAGE_GUARD))
                        for (uint64_t p = (b + 7) & ~7ull; p + 8 <= e2 && nh < 16; p += 8) {
                            uint64_t v = *(const uint64_t *)(uintptr_t)p;
                            if (v <= t && t - v < 0x800) { LOG("[deltafind]     %llx <- [%llx] + %llx\n", (unsigned long long)t, (unsigned long long)p, (unsigned long long)(t - v)); nh++; }
                        }
                    a = e2 > a ? e2 : a + 0x1000;
                }
            }
        }
        st = 2;
        free(prev); prev = NULL; free(cand); cand = NULL; free(vals); vals = NULL;
    }
#endif
}

// [XCDE] diagnostics: XCDE_FLOATFIND="<f0>,<f1>,..." + XCDE_FLOATFIND_PAT (one letter per step after f0: 'u' unchanged,
// 'a' changed one way, 'b' the other way) + XCDE_DELTAFIND_RANGE: the 4-byte-aligned finite floats (|v| < 1000) that
// follow it - e.g. the camera's angles (right stick right / left). Survivors (at most 64) logged with their values.
static void floatfind(void) {
#ifdef _WIN32
    static int st = -1, nstep;
    static uint64_t at[12], lo, hi;
    static const char *pat;
    static float *prev;
    static uint32_t *cand;
    static float (*vals)[12];
    static size_t ncand;
    if (st < 0) {
        st = 0;
        const char *e = getenv("XCDE_FLOATFIND"), *r = getenv("XCDE_DELTAFIND_RANGE");
        pat = getenv("XCDE_FLOATFIND_PAT");
        if (!e || !r || !pat || sscanf(r, "%llx-%llx", (unsigned long long *)&lo, (unsigned long long *)&hi) != 2 || hi <= lo) return;
        while (*e && nstep < 12) {
            char *q;
            at[nstep++] = strtoull(e, &q, 10);
            if (*q != ',') break;
            e = q + 1;
        }
        st = nstep >= 2 && (int)strlen(pat) >= nstep - 1 ? 1 : 0;
    }
    if (st != 1) return;
    extern uint64_t renderer_present_count(void);
    static int step;
    if (renderer_present_count() != at[step]) return;
    size_t nw = (hi - lo) / 4;
    float *cur = calloc(nw, 4);
    if (!cur) { st = 0; return; }
    for (uint64_t a = lo; a < hi;) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery((void *)(uintptr_t)a, &mi, sizeof mi) || !mi.RegionSize) break;
        uint64_t b = (uint64_t)(uintptr_t)mi.BaseAddress, e2 = b + mi.RegionSize;
        if (b < a) b = a;
        if (e2 > hi) e2 = hi;
        if (mi.State == MEM_COMMIT && (mi.Protect & 0xFF) == PAGE_READWRITE && !(mi.Protect & PAGE_GUARD)) memcpy((uint8_t *)cur + (b - lo), (const void *)(uintptr_t)b, e2 - b);
        a = e2 > a ? e2 : a + 0x1000;
    }
    // the sign of step s's change: 0 unchanged, 1 up, -1 down
    #define FSIGN(d) (fabsf(d) < 1e-5f ? 0 : (d) > 0 ? 1 : -1)
    if (step == 1) {
        size_t cap = (size_t)1 << 26;
        cand = malloc(cap * 4);
        vals = malloc(cap * sizeof *vals);
        for (size_t i = 0; i < nw && ncand < cap; i++) {
            float p = prev[i], c = cur[i];
            if (!isfinite(p) || !isfinite(c) || fabsf(p) > 1000 || fabsf(c) > 1000 || (p != 0 && fabsf(p) < 1e-20f)) continue;
            int s = FSIGN(c - p);
            if ((pat[0] == 'u') != (s == 0)) continue;
            vals[ncand][0] = p, vals[ncand][1] = c;
            cand[ncand++] = (uint32_t)i;
        }
    } else if (step > 1) {
        // the direction of the first 'a' step (+1 / -1) once seen
        size_t k = 0;
        for (size_t j = 0; j < ncand; j++) {
            uint32_t i = cand[j];
            float c = cur[i];
            if (!isfinite(c) || fabsf(c) > 1000) continue;
            int s = FSIGN(c - vals[j][step - 1]);
            char want = pat[step - 1];
            int adir = 0;
            for (int t = 1; t < step && !adir; t++) if (pat[t - 1] == 'a') adir = FSIGN(vals[j][t] - vals[j][t - 1]);
            for (int t = 1; t < step && !adir; t++) if (pat[t - 1] == 'b') adir = -FSIGN(vals[j][t] - vals[j][t - 1]);
            bool ok = want == 'u' ? s == 0 : s != 0 && (!adir || (want == 'a' ? s == adir : s == -adir));
            if (!ok) continue;
            memcpy(vals[k], vals[j], sizeof *vals);
            vals[k][step] = c;
            cand[k++] = i;
        }
        ncand = k;
    }
    LOG("[floatfind] frame %llu step %d: %zu candidates\n", (unsigned long long)at[step], step, ncand);
    free(prev);
    prev = cur;
    if (++step == nstep) {
        // XCDE_FLOATFIND_RATIO=<r>: the 'b' change / the 'a' change within 30 % of r (an angle turned back part way)
        const char *rs = getenv("XCDE_FLOATFIND_RATIO");
        int ia = (int)(strchr(pat, 'a') ? strchr(pat, 'a') - pat : -1) + 1, ib = (int)(strchr(pat, 'b') ? strchr(pat, 'b') - pat : -1) + 1;
        if (rs && ia > 0 && ib > 0) {
            float r = (float)atof(rs);
            size_t k = 0;
            for (size_t j = 0; j < ncand; j++) {
                float da = vals[j][ia] - vals[j][ia - 1], db = vals[j][ib] - vals[j][ib - 1];
                if (da != 0 && fabsf(db / da - r) <= fabsf(r) * 0.3f) { memcpy(vals[k], vals[j], sizeof *vals); cand[k++] = cand[j]; }
            }
            LOG("[floatfind] ratio %.2f: %zu of %zu\n", r, k, ncand);
            ncand = k;
        }
        for (size_t j = 0; j < ncand && j < (getenv("XCDE_FLOATFIND_ALL") ? 50000u : 64u); j++) {
            char line[256];
            int o = snprintf(line, sizeof line, "%llx:", (unsigned long long)(lo + 4ull * cand[j]));
            for (int t = 0; t < nstep && o < 240; t++) o += snprintf(line + o, sizeof line - o, " %.4f", vals[j][t]);
            LOG("[floatfind]   %s\n", line);
        }
        st = 2;
        free(prev); prev = NULL; free(cand); cand = NULL; free(vals); vals = NULL;
    }
    #undef FSIGN
#endif
}

// [XCDE] diagnostics: XCDE_FPOKE="<frame>:<hex addr>:<add>,..." adds <add> to the float there at that frame (and logs
// the vtables just before it and the floats around it) - to try a found value (the camera's angle).
static void fpoke(void) {
    static int n = -1;
    static uint64_t at[8], ad[8];
    static float add[8];
    if (n < 0) {
        n = 0;
        for (const char *p = getenv("XCDE_FPOKE"); p && *p && n < 8;) {
            char *q;
            at[n] = strtoull(p, &q, 10);
            if (*q != ':') break;
            ad[n] = strtoull(q + 1, &q, 16);
            if (*q != ':') break;
            add[n++] = strtof(q + 1, &q);
            p = *q == ',' ? q + 1 : q;
        }
    }
    extern uint64_t renderer_present_count(void);
    uint64_t f = renderer_present_count();
    for (int i = 0; i < n; i++) {
        if (at[i] != f) continue;
        float *v = (float *)(uintptr_t)ad[i];
        LOG("[fpoke] frame %llu %llx: %.4f -> %.4f\n", (unsigned long long)f, (unsigned long long)ad[i], *v, *v + add[i]);
        for (uint64_t p = ad[i] & ~7ull, k = 0; p > ad[i] - 0x800 && k < 6; p -= 8) {
            uint64_t q = *(uint64_t *)(uintptr_t)p;
            if (q >= 0x7000b00000ull && q < 0x7000c00000ull) { LOG("[fpoke]   vtable %llx at -%llx\n", (unsigned long long)q, (unsigned long long)(ad[i] - p)); k++; }
        }
        char line[512];
        int o = 0;
        for (int k = -8; k < 24 && o < 480; k++) o += snprintf(line + o, sizeof line - o, "%s%.3f", k ? " " : " [", v[k]), k == 0 ? (o += snprintf(line + o, sizeof line - o, "]")) : 0;
        LOG("[fpoke]   %s\n", line);
        *v += add[i];
    }
}

// [XCDE] diagnostics: XCDE_FTRACE="<hex addr>:<from>:<to>": the float there at every presented frame in that range
static void ftrace(void) {
    static int on = -1;
    static uint64_t ad, from, to;
    if (on < 0) on = getenv("XCDE_FTRACE") && sscanf(getenv("XCDE_FTRACE"), "%llx:%llu:%llu", (unsigned long long *)&ad, (unsigned long long *)&from, (unsigned long long *)&to) == 3;
    if (!on) return;
    extern uint64_t renderer_present_count(void);
    uint64_t f = renderer_present_count();
    if (f >= from && f <= to) LOG("[ftrace] %llu %.5f\n", (unsigned long long)f, *(float *)(uintptr_t)ad);
}

// [XCDE] diagnostics: XCDE_FVALFIND="<frame>:<v1>/<v2>/...:<rel tol>" + XCDE_DELTAFIND_RANGE (or the game's image and
// globals when unset): the 4-byte-aligned floats within the tolerance of any value at that frame (at most 400 logged)
static void fvalfind(void) {
#ifdef _WIN32
    static int st = -1;
    static uint64_t at;
    static float want[8], tol;
    static int nw;
    if (st < 0) {
        st = 0;
        const char *e = getenv("XCDE_FVALFIND");
        if (!e) return;
        char *q;
        at = strtoull(e, &q, 10);
        while (*q == ':' || *q == '/') { if (*q == ':' && nw) break; want[nw++] = strtof(q + 1, &q); if (nw == 8) break; }
        tol = *q == ':' ? strtof(q + 1, &q) : 0.002f;
        st = nw ? 1 : 0;
    }
    extern uint64_t renderer_present_count(void);
    if (st != 1 || renderer_present_count() != at) return;
    st = 2;
    uint64_t lo = 0x7000000000ull, hi = 0x7001400000ull;
    const char *r = getenv("XCDE_DELTAFIND_RANGE");
    if (r) sscanf(r, "%llx-%llx", (unsigned long long *)&lo, (unsigned long long *)&hi);
    int found = 0;
    for (uint64_t a = lo; a < hi && found < 400;) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery((void *)(uintptr_t)a, &mi, sizeof mi) || !mi.RegionSize) break;
        uint64_t b = (uint64_t)(uintptr_t)mi.BaseAddress, e2 = b + mi.RegionSize;
        if (b < a) b = a;
        if (e2 > hi) e2 = hi;
        DWORD pr = mi.Protect & 0xFF;
        if (mi.State == MEM_COMMIT && !(mi.Protect & PAGE_GUARD) && pr != PAGE_NOACCESS)
            for (uint64_t p = (b + 3) & ~3ull; p + 4 <= e2 && found < 400; p += 4) {
                float v = *(const float *)(uintptr_t)p;
                for (int i = 0; i < nw; i++)
                    if (fabsf(v - want[i]) <= fabsf(want[i]) * tol) { LOG("[fvalfind] %llx = %.6f\n", (unsigned long long)p, v); found++; break; }
            }
        a = e2 > a ? e2 : a + 0x1000;
    }
    LOG("[fvalfind] %d found\n", found);
#endif
}

void xcde_limiter_done(void) {
    fvalfind();
    floatfind();
    fpoke();
    ftrace();
    memwatch();
    statesnap();
    deltafind();
    fps_interval_force();
    static int on = -1;
    if (on < 0) { const char *e = ENV("XCDE_LIMITER_SLEEP"); on = (!e || atoi(e) != 0) && !diag_deterministic(); }
    if (!on) return;
    uint64_t now = plat_now_ns();
    if (lim_turns_since_sleep == 0) lim_n = 0;  // left right after our sleep: we may have overslept - relearn
    lim_turns_since_sleep = -1;
    if (lim_last) {
        uint64_t iv = now - lim_last;
        if (iv >= 4000000 && iv <= 100000000) lim_iv[lim_n++ % 8] = iv;
        else lim_n = 0;  // (a loading stall: start over)
    }
    lim_last = now;
}

void xcde_limiter_spin(void) {
    static int on = -1;
    if (on < 0) { const char *e = ENV("XCDE_LIMITER_SLEEP"); on = (!e || atoi(e) != 0) && !diag_deterministic(); }
    if (!on || lim_n < 8 || !lim_last) return;
    if (lim_turns_since_sleep >= 0) lim_turns_since_sleep++;
    uint64_t lo = ~0ull, hi = 0;
    for (int i = 0; i < 8; i++) { if (lim_iv[i] < lo) lo = lim_iv[i]; if (lim_iv[i] > hi) hi = lim_iv[i]; }
    if (hi - lo > 3000000) return;  // pacing not steady: leave it to the game
    uint64_t wake = lim_last + lo - 1500000, now = plat_now_ns();
    if (now >= wake) return;
    if (wake - now > 2000000) wake = now + 2000000;  // short naps: the loop re-checks in between
    static int logged;
    if (!logged++) LOG("[limiter] frame interval %.2f ms: sleeping until 1.5 ms before each frame end\n", lo / 1e6);
    plat_sleep_until_ns(wake);
    g_limiter_slept_ns += plat_now_ns() - now;
    lim_turns_since_sleep = 0;
}

