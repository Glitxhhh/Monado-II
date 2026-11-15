// [XCDE] LINUX-094: rendered-game FPS, independent of presentation repeats.
// The render thread calls frame once per real frame; drawing the overlay only
// reads value. No wall-clock, platform, ImGui or game state is needed here.
#ifndef MONADO_FPS_METER_H
#define MONADO_FPS_METER_H
#include <stdbool.h>
#include <stdint.h>

typedef struct FpsMeter {
    uint64_t start_ns, last_ns, intervals;
    double rate;
    bool started, ready;
} FpsMeter;

static inline void fps_meter_reset(FpsMeter *meter) {
    meter->start_ns = meter->last_ns = meter->intervals = 0;
    meter->rate = 0;
    meter->started = meter->ready = false;
}

static inline void fps_meter_frame(FpsMeter *meter, uint64_t now_ns) {
    // A resume starts a new window rather than displaying a stale pre-suspend
    // result. Ordinary hitches below two seconds remain part of measured FPS.
    if (!meter->started || now_ns < meter->last_ns ||
        now_ns - meter->last_ns > UINT64_C(2000000000)) {
        fps_meter_reset(meter);
        meter->started = true;
        meter->start_ns = meter->last_ns = now_ns;
        return;
    }
    if (now_ns == meter->last_ns) return;
    meter->last_ns = now_ns;
    meter->intervals++;
    uint64_t elapsed = now_ns - meter->start_ns;
    if (elapsed >= UINT64_C(500000000)) {
        meter->rate = (double)meter->intervals * 1e9 / (double)elapsed;
        meter->ready = true;
        meter->start_ns = now_ns;
        meter->intervals = 0;
    }
}

static inline bool fps_meter_value(const FpsMeter *meter, double *fps) {
    if (!fps) return false;
    *fps = meter->ready ? meter->rate : 0;
    return meter->ready;
}
// [XCDE] LINUX-097: accepted output events use their own shared time window.
// Native, generated and repeated images are classified by the presenter. These
// are successful queue submissions, not proof of physical monitor scanout.
typedef enum FpsPresentKind {
    FPS_PRESENT_NATIVE = 0,
    FPS_PRESENT_GENERATED = 1,
    FPS_PRESENT_REPEATED = 2
} FpsPresentKind;

typedef struct FpsPresentRates {
    double native, generated, repeated, total;
} FpsPresentRates;

typedef struct FpsPresentMeter {
    uint64_t start_ns, last_ns, counts[3], accepted[3];
    FpsPresentRates rate;
    bool started, ready;
} FpsPresentMeter;

static inline void fps_present_meter_reset(FpsPresentMeter *meter) {
    meter->start_ns = meter->last_ns = 0;
    for (unsigned i = 0; i < 3; ++i) meter->counts[i] = meter->accepted[i] = 0;
    meter->rate.native = meter->rate.generated = meter->rate.repeated = meter->rate.total = 0;
    meter->started = meter->ready = false;
}

// A game frame can arrive without a successful output (for example during
// swapchain recovery). Do not leave old output rates visible after suspension.
static inline void fps_present_meter_tick(FpsPresentMeter *meter, uint64_t now_ns) {
    if (meter->started && (now_ns < meter->last_ns ||
        now_ns - meter->last_ns > UINT64_C(2000000000))) fps_present_meter_reset(meter);
}

static inline bool fps_present_meter_event(FpsPresentMeter *meter, uint64_t now_ns, FpsPresentKind kind) {
    if ((unsigned)kind > (unsigned)FPS_PRESENT_REPEATED) return false;
    fps_present_meter_tick(meter, now_ns);
    if (!meter->started) {
        meter->start_ns = meter->last_ns = now_ns;
        meter->started = true;
        meter->accepted[kind] = 1;
        // The first accepted image anchors a window, like the game's meter.
        return true;
    }
    meter->last_ns = now_ns;
    // Equal-timestamp successful presents still count. A burst is real output
    // work, even though it does not establish evenly spaced displayed frames.
    meter->counts[kind]++;
    meter->accepted[kind]++;
    const uint64_t elapsed = now_ns - meter->start_ns;
    if (elapsed >= UINT64_C(500000000)) {
        const double scale = 1e9 / (double)elapsed;
        meter->rate.native = (double)meter->counts[FPS_PRESENT_NATIVE] * scale;
        meter->rate.generated = (double)meter->counts[FPS_PRESENT_GENERATED] * scale;
        meter->rate.repeated = (double)meter->counts[FPS_PRESENT_REPEATED] * scale;
        meter->rate.total = meter->rate.native + meter->rate.generated + meter->rate.repeated;
        meter->counts[0] = meter->counts[1] = meter->counts[2] = 0;
        meter->start_ns = now_ns;
        meter->ready = true;
    }
    return true;
}

static inline bool fps_present_meter_value(const FpsPresentMeter *meter, FpsPresentRates *rates) {
    if (!rates) return false;
    if (meter->ready) *rates = meter->rate;
    else rates->native = rates->generated = rates->repeated = rates->total = 0;
    return meter->ready;
}
#endif
