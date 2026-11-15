// Authored fake native cadence, actual production party threshold implementation.
#define _GNU_SOURCE
#include <assert.h>
#include <fenv.h>
#include <stdint.h>
#include <stdio.h>
#ifndef PARTY_IMPL
#define PARTY_IMPL "../../runtime/hle/party.c"
#endif
#include PARTY_IMPL
Settings g_settings;
// Unrelated production hooks are linked but must never execute Nintendo code.
bool g_party_switch, g_mouse_invert;
int g_mouse_sens;
static uint64_t fixture_heap;
uint64_t kernel_heap_size(void) { return fixture_heap; }
void rt_fatal_trace(void) { abort(); }
void guest_call(Cpu *cpu, uint64_t function) { (void)cpu; (void)function; abort(); }
uint64_t renderer_present_count(void) { abort(); }
bool xcde_in_cutscene(void) { abort(); }
bool xcde_movie_playing(void) { abort(); }
void plat_terminate(void) { abort(); }
bool input_mouse_on(void) { abort(); }
bool input_mouse_take(uint64_t frame, float *x, float *y) { (void)frame; (void)x; (void)y; abort(); }
void plat_thread_name(const char *name) { (void)name; }
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
    void *heap = VirtualAlloc((void *)(uintptr_t)HEAP_BASE, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    assert(heap == (void *)(uintptr_t)HEAP_BASE); fixture_heap = 0x10000;
    *(uint64_t *)heap = FIELD_VTABLE; assert(obj_ok(HEAP_BASE, K_FIELD));
    uint64_t value = 0; assert(party_read(HEAP_BASE, &value, sizeof value) && value == FIELD_VTABLE);
    assert(!party_read(HEAP_BASE - 8, &value, sizeof value));
    DWORD old; assert(VirtualProtect(heap, 4096, PAGE_NOACCESS, &old));
    assert(!party_read(HEAP_BASE, &value, sizeof value)); assert(!obj_ok(HEAP_BASE, K_FIELD));
    assert(VirtualProtect(heap, 4096, old, &old));
    for (unsigned i = 0; i < 5; ++i) {
        party_scanner_start(); assert(g_scan_started && g_scan_thread && g_scan_wake);
        Sleep(5); party_worker_stop();
        assert(!g_scan_started && !g_scan_thread && !g_scan_wake);
    }
    fixture_heap = 0; assert(VirtualFree(heap, 0, MEM_RELEASE));
    puts("native fractional frame threshold + Windows guarded reads/worker stop/reset: PASS");
}
