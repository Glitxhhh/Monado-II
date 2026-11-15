// Authored fake native cadence, actual production party threshold implementation.
#define _GNU_SOURCE
#include <assert.h>
#include <fenv.h>
#include <stdint.h>
#include <stdio.h>
#ifndef PARTY_IMPL
#define PARTY_IMPL "../../Monado_Linux/runtime/hle/party.c"
#endif
#include PARTY_IMPL
Settings g_settings;
static uint64_t clock_ns = 1000000000, frame;
uint64_t plat_now_ns(void) { return clock_ns; }
static void cadence(unsigned hz) {
    g_settings.fps = hz;
    for (unsigned i = 0; i < 1200; ++i) {
        clock_ns += 1000000000ull / hz;
        frames30_update(++frame);
    }
}
int main(int argc, char **argv) {
    assert(argc == 2);
    unsigned hz = (unsigned)strtoul(argv[1], NULL, 10);
    cadence(hz);
    assert(fesetround(FE_DOWNWARD) == 0);
    feclearexcept(FE_ALL_EXCEPT); feraiseexcept(FE_DIVBYZERO);
    unsigned expected = (10u * hz + 29) / 30 - 1;
    assert(xcde_frames_gt(9) == expected);
    assert(fegetround() == FE_DOWNWARD && fetestexcept(FE_ALL_EXCEPT) == FE_DIVBYZERO);
    // A second pad read/output event in one native frame cannot change cadence.
    uint32_t before = xcde_frames_gt(9);
    clock_ns += 1000;
    for (unsigned i = 0; i < 100; ++i) frames30_update(frame);
    assert(xcde_frames_gt(9) == before);
    // Skipped pad polls still measure each intervening native frame.
    if (hz != 30 && hz != 60) {
        for (unsigned i = 0; i < 1200; ++i) {
            clock_ns += 2 * (1000000000ull / hz);
            frame += 2; frames30_update(frame);
        }
        assert(xcde_frames_gt(9) == expected);
    }
    assert(xcde_frames_gt(UINT32_MAX) >= UINT32_MAX - 1);
    // Pause/reboot/backward clocks discard stale high-rate history immediately.
    if (hz != 30 && hz != 60) {
        clock_ns += 1000000000; frames30_update(++frame);
        assert(xcde_frames_gt(9) == 19);
        clock_ns = 1000; frames30_update(++frame);
        assert(xcde_frames_gt(9) == 19);
        cadence(90); assert(xcde_frames_gt(9) == 29);
        cadence(30); assert(xcde_frames_gt(9) == 9);
        cadence(hz); assert(xcde_frames_gt(9) == expected);
    }
    puts("native fractional frame threshold: PASS");
}
