// [XCDE] LINUX-103: authored bounded vibration envelope. No SDL or guest state.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <math.h>

typedef struct {
    uint16_t amplitude[2], sent[2];
    uint64_t offered[2], last_send;
} RtRumble;

static inline uint16_t rt_rumble_amplitude(float value) {
    if (!isfinite(value) || value <= 0.f) return 0;
    if (value >= 1.f) return UINT16_MAX;
    return (uint16_t)(value * 65535.f + .5f);
}

// Support the existing Monado handles and the public Horizon device-index ABI.
// Only our single served player (or handheld) may address the selected pad.
static inline bool rt_rumble_offer(RtRumble *r, uint32_t handle,
                                   const float value[4], uint64_t now) {
    unsigned player = (handle >> 8) & 255u;
    if (player != 0 && player != 0x20) return false;
    unsigned device = (handle >> 24) == 3 ? handle & 255u : (handle >> 16) & 255u;
    if (device > 1) return false;
    uint16_t low = rt_rumble_amplitude(value[0]);
    uint16_t high = rt_rumble_amplitude(value[2]);
    r->amplitude[device] = device ? high : low;
    r->offered[device] = now;
    // Some effects use a single actuator handle with both frequency bands.
    // A zero keep-alive for the other actuator must not re-arm a stale band.
    if (device && low) { r->amplitude[0] = low; r->offered[0] = now; }
    if (!device && high) { r->amplitude[1] = high; r->offered[1] = now; }
    return true;
}

// Owner thread polls even without new packets: expiry/disable actively sends zero.
// A short SDL envelope additionally bounds effects if the event loop is stopped.
static inline bool rt_rumble_poll(RtRumble *r, uint64_t now, bool enabled,
                                  uint16_t out[2]) {
    for (unsigned i = 0; i < 2; ++i) {
        if (!enabled || now < r->offered[i] || now - r->offered[i] >= 100000000ull)
            r->amplitude[i] = 0;
        out[i] = r->amplitude[i];
    }
    bool changed = out[0] != r->sent[0] || out[1] != r->sent[1];
    bool refresh = (out[0] || out[1]) && (now < r->last_send || now-r->last_send >= 75000000ull);
    if (!changed && !refresh) return false;
    r->sent[0] = out[0]; r->sent[1] = out[1]; r->last_send = now;
    return true;
}
