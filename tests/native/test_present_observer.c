// [XCDE] LINUX-099: actual downstream observer accounting/lifecycle, no GPU/game.
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <pthread.h>
#include <stdatomic.h>
#include "../../Monado_Linux/runtime/nvn/vulkan/present_observer_linux.c"
static int queue_key;
static void *queue_words[1] = {&queue_key};
static VkResult mock_result = VK_SUCCESS;
static _Atomic unsigned calls;
static VKAPI_ATTR VkResult VKAPI_CALL mock_present(VkQueue q, const VkPresentInfoKHR *info) {
    assert(q == (VkQueue)queue_words && info); ++calls; return mock_result;
}
static ObserverDevice device;
static void reset(void) {
    pthread_mutex_lock(&observer_lock);
    instances = NULL; devices = &device; memset(&device, 0, sizeof device);
    device.key = &queue_key; device.present = mock_present;
    active_swapchains = 1; accepted = failed = 0; bookkeeping_fault = false;
    reset_window_locked(); calls = 0; mock_result = VK_SUCCESS;
    pthread_mutex_unlock(&observer_lock);
}
static MonadoPresentObserverSnapshot read_at(uint64_t n) {
    MonadoPresentObserverSnapshot s; assert(monado_present_observer_snapshot(n, &s, sizeof s));
    assert(s.abi == 1 && s.bytes == sizeof s && s.reserved == 0 && s.window_ns == OBS_WINDOW_NS);
    return s;
}
static void rate(void) {
    reset(); uint64_t start = UINT64_C(1000000000);
    for (unsigned n = 0; n <= 120; ++n) { pthread_mutex_lock(&observer_lock); accept_locked(start + n * UINT64_C(8333333)); pthread_mutex_unlock(&observer_lock); }
    MonadoPresentObserverSnapshot s = read_at(start + UINT64_C(1000000000));
    assert((s.flags & (MONADO_PRESENT_OBSERVER_ATTACHED | MONADO_PRESENT_OBSERVER_SEEN | MONADO_PRESENT_OBSERVER_READY)) == 7);
    assert(s.accepted == 121 && s.failed == 0 && s.output_fps == 120.0);
    // Application FPS is not an input: sparse/bunched output events stay honest.
    reset(); for (unsigned n = 0; n < 31; ++n) { accept_locked(start + n * UINT64_C(16666667)); accept_locked(start + n * UINT64_C(16666667)); }
    s = read_at(start + UINT64_C(500000010)); assert((s.flags & MONADO_PRESENT_OBSERVER_READY) && s.output_fps == 120.0 && s.accepted == 62);
    printf("PASS genuine independent120output and bunchedaccepted events\n");
}
static void results(void) {
    reset(); VkSwapchainKHR sc[3] = {(VkSwapchainKHR)(uintptr_t)1, (VkSwapchainKHR)(uintptr_t)2, (VkSwapchainKHR)(uintptr_t)3};
    uint32_t image[3] = {0}; VkResult each[3] = {VK_SUCCESS, VK_SUBOPTIMAL_KHR, VK_ERROR_OUT_OF_DATE_KHR};
    VkPresentInfoKHR p = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .swapchainCount = 3, .pSwapchains = sc, .pImageIndices = image, .pResults = each};
    mock_result = VK_ERROR_OUT_OF_DATE_KHR;
    assert(observer_present((VkQueue)queue_words, &p) == VK_ERROR_OUT_OF_DATE_KHR);
    MonadoPresentObserverSnapshot s = read_at(observer_now()); assert(calls == 1 && s.accepted == 2 && s.failed == 1);
    p.pResults = NULL; assert(observer_present((VkQueue)queue_words, &p) == VK_ERROR_OUT_OF_DATE_KHR);
    s = read_at(observer_now()); assert(s.accepted == 2 && s.failed == 4);
    mock_result = VK_SUBOPTIMAL_KHR; assert(observer_present((VkQueue)queue_words, &p) == VK_SUBOPTIMAL_KHR);
    s = read_at(observer_now()); assert(s.accepted == 5 && s.failed == 4);
    assert(observer_present((VkQueue)queue_words, &p) == mock_result && p.pSwapchains == sc && p.pImageIndices == image && !p.pNext);
    printf("PASS perimage-success/suboptimal/partial/failure andunchangedpresentcontract\n");
}
static void edges(void) {
    reset(); MonadoPresentObserverSnapshot s = read_at(1); assert(s.flags == MONADO_PRESENT_OBSERVER_ATTACHED && !s.output_fps);
    assert(!monado_present_observer_snapshot(1, NULL, sizeof s)); memset(&s, 0x5a, sizeof s);
    assert(!monado_present_observer_snapshot(1, &s, sizeof s - 1) && s.abi == UINT32_C(0x5a5a5a5a));
    accept_locked(UINT64_C(1000000000)); s = read_at(UINT64_C(1000100000)); assert(!(s.flags & MONADO_PRESENT_OBSERVER_READY));
    s = read_at(UINT64_C(3000000000)); assert(!(s.flags & MONADO_PRESENT_OBSERVER_READY));
    accept_locked(UINT64_C(3000000000)); assert(first_ns == UINT64_C(3000000000));
    accept_locked(UINT64_C(2000000000)); assert(first_ns == UINT64_C(2000000000));
    reset(); accept_locked(UINT64_C(500000000));
    for (unsigned i = 0; i < OBS_EVENTS + 1; ++i) accept_locked(UINT64_C(1000000000) + i);
    s = read_at(UINT64_C(1000005000));
    assert((s.flags & MONADO_PRESENT_OBSERVER_SATURATED) && !(s.flags & MONADO_PRESENT_OBSERVER_READY));
    s = read_at(UINT64_C(1600000000)); assert(!(s.flags & MONADO_PRESENT_OBSERVER_READY));
    reset(); first_ns = 1; accept_locked(UINT64_C(600000000)); bookkeeping_fault = true;
    s = read_at(UINT64_C(600000001)); assert((s.flags & MONADO_PRESENT_OBSERVER_FAULT) && !(s.flags & MONADO_PRESENT_OBSERVER_READY));
    reset(); accept_locked(1); active_swapchains = 0; s = read_at(UINT64_C(500000001)); assert(!(s.flags & MONADO_PRESENT_OBSERVER_READY));
    printf("PASS ABI/warmup/pause/backwardclock/saturation/fault/detachedunknowns\n");
}
static void *writer(void *unused) {
    (void)unused; VkSwapchainKHR sc = (VkSwapchainKHR)(uintptr_t)1; uint32_t image = 0;
    VkPresentInfoKHR p = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .swapchainCount = 1, .pSwapchains = &sc, .pImageIndices = &image};
    for (unsigned i = 0; i < 5000; ++i) assert(observer_present((VkQueue)queue_words, &p) == VK_SUCCESS);
    return NULL;
}
static void concurrent(void) {
    reset(); pthread_t a, b; assert(!pthread_create(&a, NULL, writer, NULL) && !pthread_create(&b, NULL, writer, NULL));
    uint64_t previous = 0;
    for (unsigned i = 0; i < 1000; ++i) { MonadoPresentObserverSnapshot s = read_at(observer_now()); assert(s.accepted >= previous); previous = s.accepted; }
    assert(!pthread_join(a, NULL) && !pthread_join(b, NULL));
    MonadoPresentObserverSnapshot s = read_at(observer_now()); assert(s.accepted == 10000 && !s.failed);
    printf("PASS twoasyncoutputthreads/readers10000accepted (notTSanqualification)\n");
}
int main(void) { rate(); results(); edges(); concurrent(); return 0; }
