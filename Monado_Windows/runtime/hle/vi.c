// nn::vi (display/layer) and nv:: (graphics driver init), implemented on the host.
#include <stdlib.h>
#include <time.h>

#include "../hle.h"
#include "../kernel.h"
#include "../diag/diag.h"

static uint32_t vsync_r, vsync_w;
static KEvent *vsync_event;
uint32_t vi_vsync_handle(void) { return vsync_r; }
volatile uint64_t g_vsync_count;  // vsyncs signalled so far (runtime/hle/framepace.c)
uint64_t g_native_window;

// [XCDE] high frame rates: XCDE_VSYNC_HZ=<hz> (default 60) - the game also waits on this event
static uint64_t vsync_period(void) {
    const char *hz = getenv("XCDE_VSYNC_HZ");
    return hz && atoi(hz) >= 30 ? 1000000000ull / (uint64_t)atoi(hz) : 16666667ull;
}

static void det_vsync_tick(void *arg) {  // (scheduler, g_klock held)
    (void)arg;
    g_vsync_count++;
    kernel_signal(vsync_event);
    kernel_timer_at(kernel_vnow(0) + vsync_period(), det_vsync_tick, NULL);
}

static void *vsync_thread(void *arg) {
    (void)arg;
    const uint64_t period = vsync_period();
    plat_thread_realtime(period);
    uint64_t next = plat_now_ns();
    for (;;) {
        next += period;
        plat_sleep_until_ns(next);
        uint64_t now = plat_now_ns();
        if (now > next + period * 3) {
            static int warned;
            if (warned++ < 20) LOG("[vi] vsync timer late by %llums (host throttling?)\n", (unsigned long long)((now - next) / 1000000));
            next = now;  // don't burst-signal to catch up
        }
        __atomic_add_fetch(&g_vsync_count, 1, __ATOMIC_RELAXED);
        pthread_mutex_lock(&g_klock);
        kernel_signal(vsync_event);
        pthread_mutex_unlock(&g_klock);
    }
    return NULL;
}

HLE_FUNC("_ZN2nn2vi10InitializeEv", vi_initialize) { (void)c; }
HLE_FUNC("_ZN2nn2vi8FinalizeEv", vi_finalize) { (void)c; }

HLE_FUNC("_ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE", vi_open_default_display) {
    *(uint64_t *)(uintptr_t)c->x[0] = (uint64_t)hle_alloc(0x100);
    c->x[0] = 0;
}

HLE_FUNC("_ZN2nn2vi12CloseDisplayEPNS0_7DisplayE", vi_close_display) { (void)c; }

HLE_FUNC("_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayE", vi_create_layer) {
    *(uint64_t *)(uintptr_t)c->x[0] = (uint64_t)hle_alloc(0x100);
    c->x[0] = 0;
}

HLE_FUNC("_ZN2nn2vi12DestroyLayerEPNS0_5LayerE", vi_destroy_layer) { (void)c; }
HLE_FUNC("_ZN2nn2vi12SetLayerCropEPNS0_5LayerEiiii", vi_set_layer_crop) { c->x[0] = 0; }

HLE_FUNC("_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE", vi_get_native_window) {
    if (!g_native_window) g_native_window = (uint64_t)hle_alloc(0x100);
    *(uint64_t *)(uintptr_t)c->x[0] = g_native_window;
    c->x[0] = 0;
}

// Reboot: the vsync timer and its event live on (the kernel's handle table doesn't): new handles at the next boot.
RT_ON_RESET(vi_reset) {
    vsync_r = vsync_w = 0;
    g_native_window = 0;  // (HLE heap)
}

HLE_FUNC("_ZN2nn2vi20GetDisplayVsyncEventEPNS_2os15SystemEventTypeEPNS0_7DisplayE", vi_get_vsync_event) {
    if (!vsync_event) {
        vsync_event = kernel_create_event_pair(&vsync_w, &vsync_r);
        if (kernel_det()) {  // [XCDE] T029: vsync is a virtual timer, delivered when the system is idle
            pthread_mutex_lock(&g_klock);
            kernel_timer_at(kernel_vnow(0) + vsync_period(), det_vsync_tick, NULL);
            pthread_mutex_unlock(&g_klock);
        } else {
            pthread_t t;
            pthread_create(&t, NULL, vsync_thread, NULL);
        }
    } else if (!vsync_r) {
        vsync_w = kernel_new_handle(vsync_event);
        vsync_r = kernel_new_handle(vsync_event);
    }
    // nn::os::AttachReadableHandleToSystemEvent(event, handle, managed=false, clear_mode=AutoClear)
    c->x[1] = vsync_r;
    c->x[2] = 0;
    c->x[3] = 1;
    guest_call(c, guest_sym("_ZN2nn2os33AttachReadableHandleToSystemEventEPNS0_15SystemEventTypeEjbNS0_14EventClearModeE"));
    c->x[0] = 0;
}

HLE_FUNC("_ZN2nv18InitializeGraphicsEPvm", nv_initialize_graphics) { (void)c; }
HLE_FUNC("_ZN2nv16FinalizeGraphicsEv", nv_finalize_graphics) { (void)c; }
HLE_FUNC("_ZN2nv20SetGraphicsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_", nv_set_allocator) { (void)c; }
HLE_FUNC("_ZN2nv28SetGraphicsDevtoolsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_", nv_set_dt_allocator) { (void)c; }
