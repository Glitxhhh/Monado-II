// [XCDE] LINUX-099: execute real drain/reset/budget policy with authored seams.
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include FG_HEADER
#include POLICY_HEADER

static FgPresenter fg_presenter = FG_PRESENTER_INITIALIZER;
static FgPacingClock fg_output_clock;
static uint64_t fg_second_deadline;
static struct { uint64_t last_output; } fg_timing;
static _Atomic float g_framegen_cap_fps;
static unsigned ui_calls, fatal_calls;
static VkResult fatal_value;
static jmp_buf fatal_jump;
static _Noreturn void authored_fatal(const char *format, ...) {
    va_list args; va_start(args, format); fatal_value = va_arg(args, int); va_end(args);
    ++fatal_calls; longjmp(fatal_jump, 1);
}
#define FATAL(...) authored_fatal(__VA_ARGS__)
// A future regression which consumes rendering events from a drain is detected.
static void fg_result_accept(const FgPresentResult *result) { (void)result; ++ui_calls; }
void vk_framegen_drain(void);
#include DRAIN_FRAGMENT

static void seed(VkResult first, VkResult second, bool valid) {
    pthread_mutex_lock(&fg_presenter.lock);
    assert(!fg_presenter.started && !fg_presenter.busy);
    memset(&fg_presenter.result, 0, sizeof fg_presenter.result);
    fg_presenter.result.valid = valid;
    fg_presenter.result.result[0] = first; fg_presenter.result.result[1] = second;
    fg_presenter.result.call_ns[0] = 100; fg_presenter.result.call_ns[1] = 200;
    fg_presenter.result.meter_ns[0] = 1000; fg_presenter.result.meter_ns[1] = 2000;
    fg_presenter.result.job.sequence = 42;
    fg_presenter.result.job.image[0] = 3; fg_presenter.result.job.image[1] = 4;
    pthread_mutex_unlock(&fg_presenter.lock);
    fg_output_clock.active = true; fg_output_clock.last_output_ns = 777;
    fg_second_deadline = 888; fg_timing.last_output = 999;
    atomic_store(&g_framegen_cap_fps, 90.f); ui_calls = fatal_calls = 0;
}
static bool action(bool reset) {
    if (!setjmp(fatal_jump)) { if (reset) fg_present_reset(); else vk_framegen_drain(); return false; }
    return true;
}
static void intact(void) {
    assert(fg_presenter.result.valid);
    assert(fg_output_clock.active && fg_output_clock.last_output_ns == 777);
    assert(fg_second_deadline == 888 && fg_timing.last_output == 999);
    assert(atomic_load(&g_framegen_cap_fps) == 90.f && ui_calls == 0);
}
static void check_safe_results(void) {
    const VkResult allowed[] = {VK_SUCCESS, VK_SUBOPTIMAL_KHR, VK_ERROR_OUT_OF_DATE_KHR};
    unsigned cases = 0;
    for (unsigned a = 0; a < 3; ++a) for (unsigned b = 0; b < 3; ++b) {
        seed(allowed[a], allowed[b], true);
        FgPresentResult expected = fg_presenter.result, inspected = {0};
        assert(!action(false)); intact(); assert(fatal_calls == 0);
        assert(fg_presenter_inspect(&fg_presenter, false, &inspected));
        assert(!memcmp(&expected, &inspected, sizeof expected)); intact();
        assert(fg_presenter_collect(&fg_presenter, false, &inspected));
        assert(!memcmp(&expected, &inspected, sizeof expected));
        assert(!fg_presenter.result.valid && !ui_calls); ++cases;
        seed(allowed[a], allowed[b], true); assert(!action(true));
        assert(!fg_presenter.result.valid && !fg_output_clock.active);
        assert(!fg_second_deadline && !fg_timing.last_output && atomic_load(&g_framegen_cap_fps) == 0.f && !ui_calls); ++cases;
    }
    seed(VK_ERROR_DEVICE_LOST, VK_TIMEOUT, false); assert(!action(false) && !fatal_calls); assert(!action(true) && !fatal_calls);
    printf("actual drain/reset allowed, nonconsuming event and empty cases: %u passed\n", cases + 2);
}
static void check_failures(void) {
    const VkResult errors[] = {VK_TIMEOUT, VK_ERROR_DEVICE_LOST, VK_ERROR_SURFACE_LOST_KHR,
                              VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY, VK_ERROR_UNKNOWN};
    unsigned cases = 0;
    for (unsigned i = 0; i < sizeof errors / sizeof *errors; ++i) for (unsigned image = 0; image < 2; ++image) {
        seed(image ? VK_SUCCESS : errors[i], image ? errors[i] : VK_SUCCESS, true);
        FgPresentResult expected = fg_presenter.result;
        assert(action(false) && "drain must validate failed status before returning");
        assert(fatal_calls == 1 && fatal_value == errors[i]); intact();
        assert(!memcmp(&expected, &fg_presenter.result, sizeof expected));
        assert(action(true) && "reset must validate before discarding failed status");
        assert(fatal_calls == 2 && fatal_value == errors[i]); intact(); ++cases;
    }
    printf("actual drain/reset fail-closed timeout/device/surface cases: %u passed\n", cases * 2);
}

static _Atomic uint64_t clock_ns;
static unsigned presents;
static VkResult ready_status, present_status[2];
static pthread_mutex_t gate_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cv = PTHREAD_COND_INITIALIZER;
static bool hold_ready, entered_ready, release_ready;
static _Atomic bool drain_entered, drain_done;
static uint64_t mock_now(void) { return atomic_fetch_add(&clock_ns, 1000) + 1000; }
static uint64_t mock_meter(void) { return mock_now() + 1000000000ull; }
static bool mock_wait(uint64_t deadline) { atomic_store(&clock_ns, deadline); return true; }
static VkResult mock_ready(const FgPresentJob *job) {
    assert(job->sequence == 42);
    pthread_mutex_lock(&gate_lock); entered_ready = true; pthread_cond_broadcast(&gate_cv);
    while (hold_ready && !release_ready) pthread_cond_wait(&gate_cv, &gate_lock);
    pthread_mutex_unlock(&gate_lock); return ready_status;
}
static VkResult mock_present(const FgPresentJob *job, unsigned image) {
    assert(job->image[0] == 3 && job->image[1] == 4 && image < 2); ++presents; return present_status[image];
}
static void start_job(VkResult ready, VkResult first, VkResult second, bool hold) {
    seed(VK_SUCCESS, VK_SUCCESS, false);
    atomic_store(&clock_ns, 1000000); presents = 0; ready_status = ready;
    present_status[0] = first; present_status[1] = second;
    pthread_mutex_lock(&gate_lock); hold_ready = hold; entered_ready = release_ready = false; pthread_mutex_unlock(&gate_lock);
    FgPresentOps ops = {mock_now, mock_meter, mock_wait, mock_ready, mock_present};
    FgPresentJob job = {.sequence = 42, .interval_ms = 1.f, .image = {3, 4}};
    assert(fg_presenter_start(&fg_presenter, &ops));
    assert(fg_presenter_enqueue(&fg_presenter, &job, NULL));
}
static void *drain_thread(void *unused) {
    (void)unused; atomic_store(&drain_entered, true); vk_framegen_drain(); atomic_store(&drain_done, true); return NULL;
}
static void check_busy_and_real_worker(void) {
    start_job(VK_SUCCESS, VK_SUCCESS, VK_SUCCESS, true);
    pthread_mutex_lock(&gate_lock);
    while (!entered_ready) pthread_cond_wait(&gate_cv, &gate_lock);
    pthread_mutex_unlock(&gate_lock);
    atomic_store(&drain_entered, false); atomic_store(&drain_done, false);
    pthread_t waiter; assert(!pthread_create(&waiter, NULL, drain_thread, NULL));
    while (!atomic_load(&drain_entered)) { struct timespec delay = {0, 1000000}; nanosleep(&delay, NULL); }
    struct timespec delay = {0, 5000000}; nanosleep(&delay, NULL);
    assert(!atomic_load(&drain_done));
    pthread_mutex_lock(&gate_lock); release_ready = true; pthread_cond_broadcast(&gate_cv); pthread_mutex_unlock(&gate_lock);
    assert(!pthread_join(waiter, NULL)); assert(atomic_load(&drain_done));
    assert(fg_presenter.result.valid && presents == 2 && !ui_calls);
    FgPresentResult result = {0}; assert(fg_presenter_collect(&fg_presenter, false, &result));
    assert(result.result[0] == VK_SUCCESS && result.result[1] == VK_SUCCESS && result.meter_ns[1] > result.meter_ns[0]);
    fg_presenter_stop(&fg_presenter);
    const VkResult errors[] = {VK_TIMEOUT, VK_ERROR_DEVICE_LOST, VK_ERROR_SURFACE_LOST_KHR};
    unsigned cases = 0;
    for (unsigned i = 0; i < 3; ++i) for (unsigned where = 0; where < 3; ++where) {
        VkResult ready = where == 0 ? errors[i] : VK_SUCCESS;
        start_job(ready, where == 1 ? errors[i] : VK_SUCCESS, where == 2 ? errors[i] : VK_SUCCESS, false);
        assert(action(false) && fatal_value == errors[i]);
        assert(fg_presenter.result.valid && !ui_calls);
        assert(presents == (where ? 2u : 0u));
        fg_presenter_stop(&fg_presenter); ++cases;
    }
    printf("actual worker busy drain plus queued ready/present failures: %u passed\n", cases + 1);
}

// Only the native entry prelude is extracted. The next operation is the WSI
// acquire seam: a low acquisition budget must have drained pending ownership.
static unsigned fg_acquire_budget;
static bool fake_pending;
static unsigned collect_calls, wait_calls, blocking_waits;
static char calls[16];
static void probe_collect(bool wait) {
    ++collect_calls; strcat(calls, wait ? "W" : "N");
    if (wait) { ++wait_calls; blocking_waits += fake_pending; fake_pending = false; }
}
static void probe_acquire(void) {
    if (fg_acquire_budget < 3) assert(!fake_pending);
    strcat(calls, "A");
}
#define fg_present_collect probe_collect
#include BEGIN_FRAGMENT
#undef fg_present_collect
static void check_budget(void) {
    assert(fg_policy_image_count(3, 0, true) == 6);
    assert(fg_policy_acquire_budget(6, 3) == 4);
    assert(fg_policy_image_count(2, 0, true) == 5 && fg_policy_acquire_budget(5, 2) == 4);
    assert(fg_policy_image_count(1, 0, true) == 4 && fg_policy_acquire_budget(4, 1) == 4);
    assert(fg_policy_image_count(3, 4, true) == 4 && fg_policy_acquire_budget(4, 3) == 2);
    assert(fg_policy_image_count(3, 5, true) == 5 && fg_policy_acquire_budget(5, 3) == 3);
    assert(fg_policy_image_count(6, 0, true) == 8 && fg_policy_acquire_budget(8, 6) == 3);
    assert(fg_policy_acquire_budget(2, 3) == 0 && fg_policy_acquire_budget(3, 3) == 1);
    unsigned cases = 0;
    for (unsigned minimum = 1; minimum <= 8; ++minimum) {
        unsigned off = fg_policy_image_count(minimum, 0, false);
        assert(off == (minimum < 8 ? minimum + 1 : 8));
        for (unsigned maximum = minimum; maximum <= 8; ++maximum) {
            unsigned count = fg_policy_image_count(minimum, maximum, true);
            assert(count >= minimum && count <= maximum && count <= 8);
            if (maximum >= minimum + 3) assert(fg_policy_acquire_budget(count, minimum) >= 4);
            ++cases;
        }
    }
    const float clear[4] = {0};
    for (unsigned budget = 0; budget <= 4; ++budget) for (unsigned pending = 0; pending < 2; ++pending) {
        fg_acquire_budget = budget; fake_pending = pending; collect_calls = wait_calls = blocking_waits = 0; calls[0] = 0;
        assert(probe_frame_begin(clear));
        assert(!strcmp(calls, budget < 3 ? "NWA" : "NA"));
        assert(collect_calls == (budget < 3 ? 2u : 1u));
        assert(wait_calls == (budget < 3 ? 1u : 0u));
        assert(blocking_waits == (budget < 3 && pending ? 1u : 0u));
        // Off/no pending output never turns this boundary into a real wait.
        if (!pending) assert(!blocking_waits);
        ++cases;
    }
    printf("actual image-count/acquisition budget and begin boundary: %u cases passed\n", cases + 8);
}
int main(int argc, char **argv) {
    assert(argc == 2);
    if (!strcmp(argv[1], "safe")) check_safe_results();
    else if (!strcmp(argv[1], "errors")) check_failures();
    else if (!strcmp(argv[1], "worker")) check_busy_and_real_worker();
    else if (!strcmp(argv[1], "budget")) check_budget();
    else assert(!"unknown scenario");
    puts("actual frame-generation drain/budget policy PASS"); return 0;
}
