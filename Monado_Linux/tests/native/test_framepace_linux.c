// Actual authored frame-pacing implementation, synthetic guest pages only.
// No game input, generated guest code, graphics, saves, or wall-clock sleeps.
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <sys/mman.h>
#include "../../runtime/hle/framepace.c"

Settings g_settings;
static uint64_t now_ns = 1000000000ull, frame;
static uint32_t interval = 2;
static int protect_calls, fail_protect_call, protection = PROT_READ;
static unsigned checks;
#define CHECK(c) do { checks++; if (!(c)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); abort(); } } while (0)

uint64_t plat_now_ns(void) { return now_ns; }
uint64_t renderer_present_count(void) { return frame; }
int plat_protect(uint64_t addr, uint64_t size, int prot) {
    CHECK(addr == 0x70009d3000ull && size == 4096);
    CHECK(prot == PLAT_PROT_RW || prot == PLAT_PROT_READ);
    protect_calls++;
    if (protect_calls == fail_protect_call) { errno = EACCES; return -1; }
    protection = prot == PLAT_PROT_RW ? PROT_READ | PROT_WRITE : PROT_READ;
    return mprotect((void *)(uintptr_t)addr, size, protection);
}

static void map_page(uint64_t address) {
    void *p = (void *)(uintptr_t)(address & ~0xfffull);
    void *q = mmap(p, 4096, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    CHECK(q == p); // Fail rather than replacing any existing mapping.
}
static volatile float *const period = (volatile float *)0x70009d3dccull;
static volatile uint64_t *const interval_ptr = (volatile uint64_t *)0x7000b92ea0ull;
static void raw_cutscene(bool on) {
    *(volatile uint32_t *)0x70012b9410ull = on ? 0x7003 : 0x7203;
    *(volatile uint32_t *)0x70012d47b4ull = on ? 0 : 0x72;
}
static void step(void) { now_ns += 10000000; frame++; fps_interval_force(); }
static void steps(unsigned n) { while (n--) step(); }
static void check_state(uint32_t want_interval, float want_period, int present_fps) {
    CHECK(interval == want_interval);
    CHECK(fabsf(*period - want_period) <= fabsf(want_period) * 1e-6f);
    CHECK(xcde_present_fps() == present_fps);
    CHECK(protection == PROT_READ);
    // This product is the authored limiter's target in display periods.
    // Its inherited ~0.997 bias is preserved, not normalized away.
    CHECK(fabs((double)interval * *period - (double)want_interval * want_period)
          <= fabs((double)want_interval * want_period) * 1e-6);
}
static float normal_period(float original, int fps) {
    return original * (fps == 0 ? .02f : fps > 60 ? 60.0f / fps : 1.0f);
}
static void setup(int fps, float original) {
    g_settings.fps = fps;
    g_settings.cutscene_mode = 1;
    map_page(0x70009d3dccull); map_page(0x7000b92ea0ull);
    map_page(0x70012b9410ull); map_page(0x70012d47b4ull); map_page(0x7000b70628ull);
    *period = original;
    CHECK(mprotect((void *)0x70009d3000ull, 4096, PROT_READ) == 0);
    raw_cutscene(false);
}

static void put_float(void *object, size_t offset, float value) {
    memcpy((unsigned char *)object + offset, &value, sizeof value);
}
static float get_float(const void *object, size_t offset) {
    float value;
    memcpy(&value, (const unsigned char *)object + offset, sizeof value);
    return value;
}
static void check_gui(unsigned char *gui, float expected) {
    unsigned char before[256];
    memcpy(before, gui, sizeof before);
    xcde_gui_frame_delta((uintptr_t)gui);
    CHECK(get_float(gui, 0x70) == expected);
    // All neighboring fields, including reference FPS and animation scales,
    // must retain their exact bits; the helper owns only the four delta bytes.
    memcpy(before + 0x70, &expected, sizeof expected);
    CHECK(!memcmp(gui, before, sizeof before));
}
static int gui_test(const char *scenario, const char *invalid_value) {
    // No synthetic GOT mapping yet: NULL must return before reading it.
    xcde_gui_frame_delta(0);
    map_page(0x7000b93458ull);
    unsigned char clock_object[0x2100], clock_before[sizeof clock_object];
    unsigned char gui_a[256], gui_b[256], other_before[256];
    for (unsigned i = 0; i < sizeof clock_object; i++) clock_object[i] = (unsigned char)(i * 13 + 7);
    for (unsigned i = 0; i < sizeof gui_a; i++) {
        gui_a[i] = (unsigned char)(i * 11 + 3);
        gui_b[i] = (unsigned char)(i * 17 + 5);
    }
    put_float(gui_a, 0x6c, 30); put_float(gui_b, 0x6c, 120);
    volatile uint64_t *got = (volatile uint64_t *)0x7000b93458ull;
    uint64_t slot = (uintptr_t)clock_object;
    if (!strcmp(scenario, "gui-valid")) {
        // Match the real clock getter's absent-object result, including both
        // absent indirection levels. No stale animation delta may survive.
        check_gui(gui_a, 0); CHECK(*got == 0);
        *got = (uintptr_t)&slot; slot = 0;
        check_gui(gui_a, 0); CHECK(slot == 0);
        slot = (uintptr_t)clock_object;
        static const int rates[] = {30, 60, 120, 144, 165, 1000};
        for (unsigned rate = 0; rate < sizeof rates / sizeof rates[0]; rate++) {
            int fps = rates[rate];
            float elapsed = 1.0f / fps;
            put_float(clock_object, 0x2034, elapsed);
            memcpy(clock_before, clock_object, sizeof clock_object);
            memcpy(other_before, gui_b, sizeof gui_b);
            double elapsed_sum = 0;
            for (int i = 0; i < fps; i++) {
                check_gui(gui_a, elapsed);
                elapsed_sum += get_float(gui_a, 0x70);
            }
            CHECK(fabs(elapsed_sum - 1.0) < 1e-6);
            CHECK(!memcmp(gui_b, other_before, sizeof gui_b));
            CHECK(!memcmp(clock_object, clock_before, sizeof clock_object));
        }
        static const float varying[] = {0, 1.0f / 165, 1.0f / 30, .4f, 1.0f / 120, 1.0f / 60, 1.0f / 144};
        for (unsigned i = 0; i < sizeof varying / sizeof varying[0]; i++) {
            put_float(clock_object, 0x2034, varying[i]);
            memcpy(clock_before, clock_object, sizeof clock_object);
            memcpy(other_before, gui_b, sizeof gui_b);
            check_gui(gui_a, fminf(varying[i], 2.0f / 30));
            CHECK(!memcmp(gui_b, other_before, sizeof gui_b));
            memcpy(other_before, gui_a, sizeof gui_a);
            check_gui(gui_b, fminf(varying[i], 2.0f / 120));
            CHECK(!memcmp(gui_a, other_before, sizeof gui_a));
            CHECK(!memcmp(clock_object, clock_before, sizeof clock_object));
            CHECK(slot == (uintptr_t)clock_object && *got == (uintptr_t)&slot);
        }
        printf("PASS GUI elapsed delta checks=%u constant/variable rates, clamp, contexts, null clock, exact neighboring bytes\n", checks);
        return 0;
    }
    CHECK(invalid_value != NULL);
    *got = (uintptr_t)&slot;
    put_float(clock_object, 0x2034, 1.0f / 165);
    if (!strcmp(scenario, "gui-bad-elapsed")) put_float(clock_object, 0x2034, strtof(invalid_value, NULL));
    else if (!strcmp(scenario, "gui-bad-reference")) put_float(gui_a, 0x6c, strtof(invalid_value, NULL));
    else { fprintf(stderr, "Unknown GUI scenario: %s\n", scenario); return 2; }
    xcde_gui_frame_delta((uintptr_t)gui_a);
    fprintf(stderr, "FAIL: invalid GUI timing was accepted\n");
    return 1;
}

// [XCDE] LINUX-012: exercise the actual repeat helper with synthetic engine
// state. It may return new scalar bits, but must not write borrowed input or
// engine memory, use requested FPS/presentation count, or scale world clocks.
static uint32_t float_bits(float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof bits);
    return bits;
}
static void check_repeat(unsigned char *clock_object, size_t clock_size,
                         uint64_t *slot, float reference, float expected) {
    unsigned char clock_before[0x2100], got_before[4096];
    CHECK(clock_size == sizeof clock_before);
    memcpy(clock_before, clock_object, clock_size);
    memcpy(got_before, (void *)0x7000b93000ull, sizeof got_before);
    uint64_t slot_before = *slot;
    uint32_t reference_bits = float_bits(reference), original_bits = reference_bits;
    uint64_t time_before = now_ns, frame_before = frame;
    uint32_t result = xcde_repeat_delta_bits(reference_bits);
    CHECK(result == float_bits(expected));
    CHECK(reference_bits == original_bits);
    CHECK(!memcmp(clock_object, clock_before, clock_size));
    CHECK(!memcmp((void *)0x7000b93000ull, got_before, sizeof got_before));
    CHECK(*slot == slot_before);
    CHECK(now_ns == time_before && frame == frame_before);
}
static int repeat_test(const char *scenario, const char *invalid_value) {
    map_page(0x7000b93458ull);
    unsigned char clock_object[0x2100];
    for (unsigned i = 0; i < sizeof clock_object; i++) clock_object[i] = (unsigned char)(i * 23 + 11);
    volatile uint64_t *got = (volatile uint64_t *)0x7000b93458ull;
    uint64_t slot = (uintptr_t)clock_object;
    float reference = 1.0f / 30;
    if (!strcmp(scenario, "repeat-valid")) {
        // Both absent indirection levels match the original engine getter's zero.
        check_repeat(clock_object, sizeof clock_object, &slot, reference, 0);
        CHECK(*got == 0);
        *got = (uintptr_t)&slot; slot = 0;
        check_repeat(clock_object, sizeof clock_object, &slot, reference, 0);
        CHECK(slot == 0);
        slot = (uintptr_t)clock_object;
        static const int rates[] = {30, 60, 120, 144, 165, 1000};
        for (unsigned rate = 0; rate < sizeof rates / sizeof rates[0]; rate++) {
            float elapsed = 1.0f / rates[rate];
            put_float(clock_object, 0x2034, elapsed);
            double elapsed_sum = 0;
            for (int i = 0; i < rates[rate]; i++) {
                // Advancing irrelevant host/present clocks must have no effect.
                now_ns += 7000000; frame += 3;
                check_repeat(clock_object, sizeof clock_object, &slot, reference, elapsed);
                elapsed_sum += elapsed;
            }
            CHECK(fabs(elapsed_sum - 1.0) < 1e-6);
        }
        static const float varying[] = {0, -0.0f, 1.0f / 165, 1.0f / 30, .4f, 1.0f / 120, 1.0f / 60, 1.0f / 144};
        static const float references[] = {1.0f / 30, 1.0f / 120, 1.0f / 165};
        for (unsigned i = 0; i < sizeof varying / sizeof varying[0]; i++) {
            put_float(clock_object, 0x2034, varying[i]);
            for (unsigned j = 0; j < sizeof references / sizeof references[0]; j++) {
                float maximum = 2.0f * references[j];
                float expected = varying[i] > maximum ? maximum : varying[i];
                check_repeat(clock_object, sizeof clock_object, &slot, references[j], expected);
            }
        }
        printf("PASS repeat elapsed delta checks=%u exact bits, constant/variable rates, clamp, references, null clock, unchanged input memory\n", checks);
        return 0;
    }
    CHECK(invalid_value != NULL);
    *got = (uintptr_t)&slot;
    put_float(clock_object, 0x2034, 1.0f / 165);
    if (!strcmp(scenario, "repeat-bad-elapsed")) put_float(clock_object, 0x2034, strtof(invalid_value, NULL));
    else if (!strcmp(scenario, "repeat-bad-reference")) reference = strtof(invalid_value, NULL);
    else { fprintf(stderr, "Unknown repeat scenario: %s\n", scenario); return 2; }
    xcde_repeat_delta_bits(float_bits(reference));
    fprintf(stderr, "FAIL: invalid repeat timing was accepted\n");
    return 1;
}

int main(int argc, char **argv) {
    CHECK(argc == 3 || argc == 4);
    const char *scenario = argv[1];
    int fps = atoi(argv[2]);
    if (!strncmp(scenario, "gui-", 4)) {
        g_settings.fps = fps; // The engine clock, not this setting, drives GUI time.
        return gui_test(scenario, argc == 4 ? argv[3] : NULL);
    }
    if (!strncmp(scenario, "repeat-", 7)) {
        g_settings.fps = fps; // Input repeat also follows elapsed engine time.
        return repeat_test(scenario, argc == 4 ? argv[3] : NULL);
    }
    float original = argc == 4 && strcmp(scenario, "protect-failure") ? strtof(argv[3], NULL) : .997f;
    setup(fps, original);
    if (!strcmp(scenario, "first-cutscene")) {
        // The cutscene settles while the game's interval pointer is absent.
        raw_cutscene(true); steps(4);
        CHECK(xcde_cutscene_30()); CHECK(*period == original); CHECK(protect_calls == 0);
        *interval_ptr = (uintptr_t)&interval; step();
        check_state(2, original, 30);
        raw_cutscene(false); steps(4);
        check_state(fps == 30 ? 2 : 1, normal_period(original, fps), fps);
    } else if (!strcmp(scenario, "basic")) {
        *interval_ptr = (uintptr_t)&interval; step();
        check_state(fps == 30 ? 2 : 1, normal_period(original, fps), fps);
        for (int cycle = 0; cycle < 40; cycle++) {
            raw_cutscene(true); steps(3);
            CHECK(!xcde_cutscene_30());
            step(); check_state(2, original, 30);
            // A long pause must not compound the scale or change the target.
            now_ns += 2000000000ull; step(); check_state(2, original, 30);
            raw_cutscene(false); steps(3); CHECK(xcde_cutscene_30());
            step(); check_state(fps == 30 ? 2 : 1, normal_period(original, fps), fps);
        }
    } else if (!strcmp(scenario, "interval-override")) {
        CHECK(getenv("XCDE_FPS_INTERVAL") && getenv("XCDE_FPS_PERIOD_SCALE"));
        *interval_ptr = (uintptr_t)&interval; raw_cutscene(true); steps(5);
        check_state(3, original * .5f, fps); CHECK(!xcde_cutscene_30());
    } else if (!strcmp(scenario, "scale-override")) {
        CHECK(getenv("XCDE_FPS_PERIOD_SCALE"));
        *interval_ptr = (uintptr_t)&interval; step(); check_state(1, original * .25f, fps);
        raw_cutscene(true); steps(4); check_state(2, original, 30);
        raw_cutscene(false); steps(4); check_state(1, original * .25f, fps);
    } else if (!strcmp(scenario, "delayed-cutscene")) {
        CHECK(getenv("XCDE_FPS_AFTER_S") && getenv("XCDE_FPS_AFTER_FRAMES"));
        *interval_ptr = (uintptr_t)&interval; raw_cutscene(true);
        // after is measured at the first call, so .1 s expires on call 11.
        steps(10); CHECK(interval == 2); CHECK(*period == original); CHECK(protect_calls == 0);
        step(); check_state(2, original, 30);
        raw_cutscene(false); steps(4); check_state(1, normal_period(original, fps), fps);
    } else if (!strcmp(scenario, "disabled-override")) {
        CHECK(getenv("XCDE_FPS_INTERVAL"));
        *interval_ptr = (uintptr_t)&interval; raw_cutscene(true); steps(5);
        check_state(2, original, fps); CHECK(protect_calls == 0);
    } else if (!strcmp(scenario, "protect-failure")) {
        CHECK(argc == 4);
        fail_protect_call = atoi(argv[3]); CHECK(fail_protect_call == 1 || fail_protect_call == 2);
        *interval_ptr = (uintptr_t)&interval; step();
        fprintf(stderr, "FAIL: protection failure was not fatal\n"); return 1;
    } else {
        fprintf(stderr, "Unknown scenario: %s\n", scenario); return 2;
    }
    printf("PASS framepace scenario=%s fps=%d checks=%u interval=%u period=%.9g target_ms=%.9g\n",
           scenario, fps, checks, interval, (double)*period, interval * (double)*period * 1000 / 60);
    return 0;
}
