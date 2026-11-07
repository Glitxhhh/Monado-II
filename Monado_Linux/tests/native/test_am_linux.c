// [XCDE] LINUX-007: actual AM/IPC/kernel contract with synthetic packets only.
// No modules, game input, GPU, saves or process-suspension claims.
#include <stdatomic.h>
#include <sched.h>
#include "../../runtime/kernel.c"
#include "../../runtime/ipc.c"
#include "../../runtime/services/am.c"
#include "../../runtime/host/applet_lifecycle_ui.h"
#include <SDL3/SDL_init.h>

void rt_dispatch(Cpu *c) { (void)c; abort(); }
GuestFn dispatch_lookup(uint64_t a) { (void)a; return NULL; }
Module g_modules[4]; int g_nmodules;
const uint8_t g_user_id[16];
const char *settings_language(void) { return "en-US"; }
void crash_handler_install(void) { plat_crash_handler_install(); }
void rt_fatal_trace(void) { abort(); }
void rt_fault_report(const char *s, uint64_t a, uint64_t p, uint64_t l) { (void)s; (void)a; (void)p; (void)l; abort(); }
bool rt_fault_watch(uint64_t a, uint64_t p) { (void)a; (void)p; return false; }
const char *rt_host_fn_name(uint64_t p, uint64_t *o) { (void)p; (void)o; return NULL; }
uint32_t vi_vsync_handle(void) { return 0; }
uint64_t renderer_present_count(void) { return 0; }
const char *loader_symbol_at(uint64_t a, uint64_t *o) { (void)a; (void)o; return NULL; }
void diag_crash_report(void *p) { (void)p; }

static atomic_uint checks;
#define CHECK(c) do { atomic_fetch_add(&checks, 1); if (!(c)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); abort(); } } while (0)

typedef struct {
    uint32_t copy, move, object, result, bytes;
    int nc, nm, no;
    uint64_t value;
} Reply;
static bool corrupt_domain_bounds;
static unsigned idle_extra; // malformed idle request: PID/handles/X/A/B/C/domain object

// Use genuine HIPC/CMIF requests and SendSyncRequest. Domain responses have a
// domain header and object IDs rather than moved session handles.
static Reply request_expected(Cpu *c, uint32_t handle, uint32_t type, uint32_t object,
                              uint32_t command, const void *data, size_t size,
                              uint32_t expected_result) {
    _Alignas(16) uint32_t msg[128] = {0};
    CHECK(size <= 16);
    msg[0] = type | (idle_extra == 4 ? 1u << 16 : 0) |
        (idle_extra == 5 ? 1u << 20 : 0) | (idle_extra == 6 ? 1u << 24 : 0);
    msg[1] = 8 + (uint32_t)((size + 3) / 4) + (object ? 4 : 0) + (idle_extra == 8 ? 1 : 0);
    if (idle_extra == 7) msg[1] |= 2u << 10;
    uint32_t *p = msg + 2;
    if (idle_extra >= 1 && idle_extra <= 3) {
        msg[1] |= 0x80000000u;
        *p++ = idle_extra == 1 ? 1 : idle_extra == 2 ? 2 : 32;
        if (idle_extra == 1) { *p++ = 0; *p++ = 0; } else *p++ = handle;
    }
    if (idle_extra == 4) { *p++ = 0; *p++ = 0; }
    if (idle_extra == 5 || idle_extra == 6) { *p++ = 0; *p++ = 0; *p++ = 0; }
    uint32_t *raw_start = p;
    if (idle_extra == 7) { uint32_t *cdesc = raw_start + (msg[1] & 0x3ff); cdesc[0] = cdesc[1] = 0; }
    p = (uint32_t *)(((uintptr_t)p + 15) & ~(uintptr_t)15);
    if (object) {
        p[0] = 1 | (idle_extra == 8 ? 1u << 8 : 0) | ((uint32_t)(16 + size) << 16);
        if (corrupt_domain_bounds) p[0] = 1 | (0x2000u << 16);
        p[1] = object;
        p += 4;
    }
    memcpy(p, "SFCI", 4); p[2] = command;
    if (size) memcpy(p + 4, data, size);
    if (idle_extra == 8) p[4 + (size + 3) / 4] = object;
    uint64_t saved = c->tpidrro;
    c->tpidrro = (uintptr_t)msg;
    c->x[0] = handle;
    rt_svc(c, 0x21, 0);
    c->tpidrro = saved;
    CHECK(c->x[0] == R_OK);
    Reply r = {0};
    CHECK(msg[0] == 0);
    p = msg + 2;
    if (msg[1] & 0x80000000u) {
        uint32_t hd = *p++;
        CHECK(!(hd & 1));
        r.nc = (hd >> 1) & 15; r.nm = (hd >> 5) & 15;
        CHECK(r.nc <= 1 && r.nm <= 1);
        if (r.nc) r.copy = *p++;
        if (r.nm) r.move = *p++;
    }
    p = (uint32_t *)(((uintptr_t)p + 15) & ~(uintptr_t)15);
    if (object) { r.no = p[0]; CHECK(r.no <= 1); p += 4; }
    CHECK(!memcmp(p, "SFCO", 4));
    r.result = p[2];
    uint32_t raw = msg[1] & 0x3ff;
    CHECK(raw >= 8u + (object ? 4u + r.no : 0));
    r.bytes = (raw - 8 - (object ? 4 + r.no : 0)) * 4;
    CHECK(r.bytes <= 8);
    memcpy(&r.value, p + 4, r.bytes);
    if (r.no) r.object = p[4 + r.bytes / 4];
    CHECK(r.result == expected_result);
    return r;
}

static Reply request(Cpu *c, uint32_t handle, uint32_t type, uint32_t object,
                     uint32_t command, const void *data, size_t size) {
    return request_expected(c, handle, type, object, command, data, size, 0);
}

typedef struct { uint32_t handle, object; } Endpoint;
static Endpoint open_interface(Cpu *c, bool domain, uint32_t interface_command) {
    uint32_t sm = ipc_connect_named_port("sm:");
    CHECK(sm != 0);
    Reply r = request(c, sm, 4, 0, 1, "appletOE", 8);
    CHECK(r.nm == 1 && !r.nc && !r.bytes);
    Endpoint e = {.handle = r.move};
    if (domain) {
        r = request(c, e.handle, 5, 0, 0, NULL, 0);
        CHECK(r.bytes == 4 && r.value == 1 && !r.nm && !r.nc);
        e.object = (uint32_t)r.value;
    }
    r = request(c, e.handle, 4, e.object, 0, NULL, 0);
    CHECK(!r.bytes && !r.nc && (domain ? r.no == 1 && !r.nm : r.nm == 1));
    if (domain) e.object = r.object; else e.handle = r.move;
    r = request(c, e.handle, 4, e.object, interface_command, NULL, 0);
    CHECK(!r.bytes && !r.nc && (domain ? r.no == 1 && !r.nm : r.nm == 1));
    if (domain) e.object = r.object; else e.handle = r.move;
    return e;
}

static uint32_t event_handle(Cpu *c, Endpoint e) {
    Reply r = request(c, e.handle, 4, e.object, 91, NULL, 0);
    CHECK(r.nc == 1 && !r.nm && !r.no && !r.bytes);
    CHECK(handle_get(r.copy) && handle_get(r.copy)->type == KOBJ_EVENT);
    return r.copy;
}
static void ticks_zero(Cpu *c, Endpoint e) {
    Reply r = request(c, e.handle, 4, e.object, 90, NULL, 0);
    CHECK(r.bytes == 8 && !r.value && !r.nc && !r.nm && !r.no);
}
static void svc(Cpu *c, uint32_t number, uint32_t handle, uint32_t expected) {
    c->x[0] = handle; rt_svc(c, number, 0); CHECK(c->x[0] == expected);
}
static void poll_event(Cpu *c, uint32_t handle, uint32_t expected) {
    c->x[1] = (uintptr_t)&handle; c->x[2] = 1; c->x[3] = 0;
    rt_svc(c, 0x18, 0);
    CHECK(c->x[0] == expected);
    if (expected == R_OK) CHECK(c->x[1] == 0);
}

static void check_focus(Cpu *c, Endpoint common) {
    Reply r = request(c, common.handle, 4, common.object, 9, NULL, 0);
    CHECK(r.bytes == 4 && r.value == 1 && !r.nc && !r.nm && !r.no); // u8, padded to word
}
static void acquire(Cpu *c, Endpoint window) {
    Reply r = request(c, window.handle, 4, window.object, 10, NULL, 0);
    CHECK(!r.bytes && !r.nc && !r.nm && !r.no);
}
static void no_message(Cpu *c, Endpoint common) {
    Reply r = request_expected(c, common.handle, 4, common.object, 1, NULL, 0, AM_NO_MESSAGE);
    CHECK(!r.bytes && !r.nc && !r.nm && !r.no);
}
static void test_foreground(Cpu *c, uint32_t suspended_handle) {
    Endpoint common = open_interface(c, false, 0), dcommon = open_interface(c, true, 0);
    Endpoint window = open_interface(c, false, 2), dwindow = open_interface(c, true, 2);
    Reply r = request(c, common.handle, 4, common.object, 0, NULL, 0);
    CHECK(r.nc == 1 && !r.nm && !r.no && !r.bytes);
    uint32_t message = r.copy;
    CHECK(handle_get(message) && handle_get(message)->type == KOBJ_EVENT);
    r = request(c, dcommon.handle, 4, dcommon.object, 0, NULL, 0);
    CHECK(r.nc == 1 && r.copy != message && handle_get(r.copy) == handle_get(message));
    svc(c, 0x16, message, R_OK);
    CHECK(!handle_get(message) && handle_get(r.copy));
    message = r.copy; // one returned message handle can close independently
    // Startup must already be focused before acquisition, as required by SDK
    // initialization. Acquisition must not consume the initial notification.
    check_focus(c, common); check_focus(c, dcommon); poll_event(c, message, R_OK);
    acquire(c, window); acquire(c, dwindow); acquire(c, window);
    check_focus(c, common); check_focus(c, dcommon); poll_event(c, message, R_OK);
    r = request(c, dcommon.handle, 4, dcommon.object, 1, NULL, 0);
    CHECK(r.bytes == 4 && r.value == 15 && !r.nc && !r.nm && !r.no);
    poll_event(c, message, R_TIMEOUT); no_message(c, common);
    // Repeated acquisition cannot create phantom focus/resume messages or
    // signal the suspended-time event. This repeats after every boot reset.
    acquire(c, dwindow); acquire(c, window); acquire(c, dwindow);
    check_focus(c, common); check_focus(c, dcommon);
    no_message(c, common); no_message(c, dcommon);
    poll_event(c, message, R_TIMEOUT); poll_event(c, suspended_handle, R_TIMEOUT);
}

static void empty_reply(Reply r) { CHECK(!r.bytes && !r.nc && !r.nm && !r.no); }
// [XCDE] LINUX-010: application-wide enum state, across distinct interfaces.
static void test_idle_policy(Cpu *c, Endpoint normal, Endpoint domain) {
    Reply r = request(c, normal.handle, 4, normal.object, 63, NULL, 0);
    CHECK(r.bytes == 4 && r.value == 0 && !r.nc && !r.nm && !r.no);
    const uint32_t modes[] = {1, 2, 2, 0, 1, 0, 2};
    for (unsigned i = 0; i < sizeof modes / sizeof *modes; ++i) {
        Endpoint write = i & 1 ? normal : domain, read = i & 1 ? domain : normal;
        empty_reply(request(c, write.handle, 4, write.object, 62, modes + i, 4));
        r = request(c, read.handle, 4, read.object, 63, NULL, 0);
        CHECK(r.bytes == 4 && r.value == modes[i] && !r.nc && !r.nm && !r.no);
        ticks_zero(c, normal);
    }
    Endpoint fresh = open_interface(c, true, 1);
    r = request(c, fresh.handle, 4, fresh.object, 63, NULL, 0);
    CHECK(r.bytes == 4 && r.value == 2 && !r.nc && !r.nm && !r.no);
    Endpoint common = open_interface(c, false, 0);
    no_message(c, common); // no invented focus, sleep or resume event
}
static void test_policy(Cpu *c, Endpoint self) {
    Endpoint common = open_interface(c, true, 0);
    for (unsigned bits = 0; bits < 8; ++bits) {
        uint8_t flags[3] = {bits & 1, (bits >> 1) & 1, (bits >> 2) & 1};
        empty_reply(request(c, self.handle, 4, self.object, 13, flags, 3));
        pthread_mutex_lock(&g_klock);
        CHECK(!memcmp(am.focus_policy, flags, 3));
        pthread_mutex_unlock(&g_klock);
        check_focus(c, common); no_message(c, common); ticks_zero(c, self);
    }
    const uint32_t commands[] = {11, 12, 14, 16};
    for (unsigned i = 0; i < sizeof commands / sizeof *commands; ++i) {
        for (uint8_t flag = 0; flag < 2; ++flag) {
            empty_reply(request(c, self.handle, 4, self.object, commands[i], &flag, 1));
            pthread_mutex_lock(&g_klock);
            bool stored = commands[i] == 11 ? am.operation_notify : commands[i] == 12 ? am.performance_notify :
                commands[i] == 14 ? am.resume_notify : am.out_of_focus_suspending;
            CHECK(stored == (flag != 0));
            pthread_mutex_unlock(&g_klock);
            check_focus(c, common); no_message(c, common); ticks_zero(c, self);
        }
    }
    unsigned mode = getenv("XCDE_HANDHELD") ? 0 : 1;
    Reply r = request(c, common.handle, 4, common.object, 5, NULL, 0);
    CHECK(r.bytes == 4 && r.value == mode);
    r = request(c, common.handle, 4, common.object, 6, NULL, 0);
    CHECK(r.bytes == 4 && r.value == mode);
    // Changing configuration in the fixture does not create a native mode
    // transition. Production takes a snapshot once at applet initialization.
    if (mode) CHECK(setenv("XCDE_HANDHELD", "1", 1) == 0); else CHECK(unsetenv("XCDE_HANDHELD") == 0);
    r = request(c, common.handle, 4, common.object, 5, NULL, 0); CHECK(r.value == mode);
    r = request(c, common.handle, 4, common.object, 6, NULL, 0); CHECK(r.value == mode);
    if (mode) CHECK(unsetenv("XCDE_HANDHELD") == 0); else CHECK(setenv("XCDE_HANDHELD", "1", 1) == 0);
    no_message(c, common);
}

static int cycle;
static atomic_int sdl_phase;
static void test_exit(Cpu *c, Endpoint self) {
    Endpoint common = open_interface(c, false, 0);
    CHECK(!am_host_exit_ready(0));
    empty_reply(request(c, self.handle, 4, self.object, 1, NULL, 0));
    empty_reply(request(c, self.handle, 4, self.object, 1, NULL, 0)); // boolean lock, idempotent
    if (cycle % 4 == 3) {
        atomic_store(&sdl_phase, 1);
        uint64_t end = plat_now_ns() + 5000000000ull;
        while (atomic_load(&sdl_phase) != 2) { CHECK(plat_now_ns() < end); sched_yield(); }
        CHECK(!am_host_exit_ready(plat_now_ns()));
        check_focus(c, common);
        Reply r = request(c, common.handle, 4, common.object, 1, NULL, 0); CHECK(r.value == 4);
        no_message(c, common);
        empty_reply(request(c, self.handle, 4, self.object, 2, NULL, 0));
        atomic_store(&sdl_phase, 3);
        return;
    }
    // Explicit injected timestamps exercise exact boundaries without waiting
    // 15 seconds or tying the native-close deadline to the virtual guest clock.
    uint64_t start = UINT64_MAX - 20000000000ull;
    if (cycle % 4 == 0) empty_reply(request(c, self.handle, 4, self.object, 2, NULL, 0));
    am_request_host_exit(start);
    am_request_host_exit(start + 1000000000ull);
    Reply r = request(c, common.handle, 4, common.object, 1, NULL, 0);
    CHECK(r.bytes == 4 && r.value == 4); no_message(c, common);
    if (cycle % 4 == 0) { CHECK(am_host_exit_ready(start)); return; }
    CHECK(!am_host_exit_ready(start - 1));
    CHECK(!am_host_exit_ready(start + AM_EXIT_GRACE_NS - 1));
    if (cycle % 4 == 1) {
        empty_reply(request(c, self.handle, 4, self.object, 2, NULL, 0));
        CHECK(am_host_exit_ready(start + 1));
    } else {
        CHECK(am_host_exit_ready(start + AM_EXIT_GRACE_NS));
        CHECK(am_host_exit_ready(start + AM_EXIT_GRACE_NS + 1));
    }
    no_message(c, common);
}

static KObject *previous_event, *last_event;
static uint32_t retained_handle;
static int strict_negative;
static const char *idle_case;
static bool domain_size_probe;
static bool blocked_close;
static bool timer_signal_probe, early_close;
static atomic_bool close_sent;
static atomic_uint timer_fired;
static void timer_signal(void *object) {
    CHECK(kernel_det() && g_baton == NULL);
    kernel_signal(object); // called from inside the active deterministic dispatcher
    CHECK(g_baton == NULL); // callback must not recursively grant a second baton
    atomic_fetch_add(&timer_fired, 1);
}
static void payload_probe(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s; (void)resp;
    CHECK(req->data_size == req->cmd);
}
void guest_call(Cpu *c, uint64_t entry) {
    CHECK(entry == 1 && kernel_current_thread() == c->thread);
    if (timer_signal_probe) {
        CHECK(kernel_det());
        KObject event = {.type = KOBJ_EVENT};
        uint32_t handle = kernel_new_handle(&event);
        pthread_mutex_lock(&g_klock);
        kernel_timer_at(kernel_vnow(0) + 1000000, timer_signal, &event);
        pthread_mutex_unlock(&g_klock);
        c->x[1] = (uintptr_t)&handle; c->x[2] = 1; c->x[3] = UINT64_MAX;
        rt_svc(c, 0x18, 0);
        CHECK(c->x[0] == R_OK && atomic_load(&timer_fired) == 1);
        pthread_mutex_lock(&g_klock); CHECK(g_baton == kernel_current_thread()); pthread_mutex_unlock(&g_klock);
        svc(c, 0x16, handle, R_OK);
        return;
    }
    if (domain_size_probe) {
        uint32_t handle = session_new(service_new("PayloadProbe", payload_probe, NULL));
        Reply r = request(c, handle, 5, 0, 0, NULL, 0);
        CHECK(r.value == 1);
        uint8_t bytes[4] = {0, 0, 1, 0};
        for (uint32_t size = 0; size <= sizeof bytes; ++size)
            request(c, handle, 4, 1, size, bytes, size);
        // Normal CMIF payloads retain their rounded word padding.
        handle = session_new(service_new("PayloadProbeNormal", payload_probe, NULL));
        for (uint32_t size = 0; size <= sizeof bytes; ++size)
            request(c, handle, 4, 0, (size + 3) & ~3u, bytes, size);
        return;
    }
    Endpoint normal = open_interface(c, false, 1), domain = open_interface(c, true, 1);
    if (idle_case) {
        Endpoint e = strstr(idle_case, "domain") ? domain : normal;
        uint32_t values[2] = {strstr(idle_case, "range") ? 3 : strstr(idle_case, "negative") ? UINT32_MAX : 1, 0};
        uint32_t command = strstr(idle_case, "get") ? 63 : 62;
        size_t size = command == 63 ? 0 : 4;
        if (strstr(idle_case, "short")) size = e.object ? 3 : 0;
        if (strstr(idle_case, "extra-data")) size += 4;
        const char *extra = strstr(idle_case, "extra-");
        if (extra && extra[6] >= '1' && extra[6] <= '8') idle_extra = (unsigned)(extra[6] - '0');
        empty_reply(request(c, e.handle, 4, e.object, command, values, size));
        CHECK(!strcmp(idle_case, "--idle-set"));
        puts("idle setter baseline request returned");
        return;
    }
    if (early_close) {
        Endpoint common = open_interface(c, true, 0);
        Reply r = request(c, common.handle, 4, common.object, 1, NULL, 0); CHECK(r.value == 15);
        r = request(c, common.handle, 4, common.object, 1, NULL, 0); CHECK(r.value == 4);
        no_message(c, common); CHECK(am_host_exit_ready(0));
        return;
    }
    if (blocked_close) {
        Endpoint common = open_interface(c, false, 0);
        Reply r = request(c, common.handle, 4, 0, 1, NULL, 0); CHECK(r.value == 15);
        empty_reply(request(c, normal.handle, 4, 0, 1, NULL, 0));
        r = request(c, common.handle, 4, 0, 0, NULL, 0);
        uint32_t message = r.copy;
        c->x[1] = (uintptr_t)&message; c->x[2] = 1; c->x[3] = UINT64_MAX;
        rt_svc(c, 0x18, 0); // Actual infinite kernel wait, not a zero-time poll.
        CHECK(atomic_load(&close_sent) && c->x[0] == R_OK && c->x[1] == 0);
        r = request(c, common.handle, 4, 0, 1, NULL, 0); CHECK(r.value == 4);
        CHECK(!am_host_exit_ready(plat_now_ns()));
        empty_reply(request(c, domain.handle, 4, domain.object, 2, NULL, 0));
        LOG("[fixture] blocked guest consumed ExitRequest and unlocked\n");
        return;
    }
    if (strict_negative) {
        CHECK(diag_strict());
        Endpoint e = strict_negative == 2 ? open_interface(c, true, 2) : strict_negative >= 5 ? domain : normal;
        uint32_t command = strict_negative >= 3 ? 13 : 0x7fffffff;
        uint8_t focus_policy[3] = {0, 0, strict_negative == 3 ? 2 : 1};
        size_t size = strict_negative == 4 ? 0 : strict_negative == 5 ? 2 : strict_negative >= 3 ? 3 : 0;
        corrupt_domain_bounds = strict_negative == 6;
        request(c, e.handle, 4, e.object, command,
                size ? focus_policy : NULL, size);
        CHECK(!"strict unsupported command returned");
    }
    ticks_zero(c, normal); ticks_zero(c, domain);
    uint32_t a = event_handle(c, normal), b = event_handle(c, domain);
    CHECK(a != b && handle_get(a) == handle_get(b));
    KObject *event = handle_get(a);
    CHECK(event != previous_event && event->refs == 2 && !event->signaled);
    test_foreground(c, a);
    test_idle_policy(c, normal, domain);
    pthread_mutex_lock(&g_klock);
    CHECK(!am.exit_locked && !am.exit_requested && !am.operation_notify && !am.performance_notify && !am.resume_notify &&
          !am.out_of_focus_suspending && am.focus_policy[0] == 0 && am.focus_policy[1] == 0 && am.focus_policy[2] == 1);
    pthread_mutex_unlock(&g_klock);
    test_policy(c, normal); test_policy(c, domain);
    poll_event(c, a, R_TIMEOUT);
    svc(c, 0x17, a, R_INVALID_STATE);
    svc(c, 0x16, a, R_OK);
    CHECK(!handle_get(a) && handle_get(b) == event && event->refs == 1);
    // Signal is test-originated: this verifies the real kernel event contract,
    // not an implementation or demonstration of applet process suspension.
    pthread_mutex_lock(&g_klock); kernel_signal(event); pthread_mutex_unlock(&g_klock);
    poll_event(c, b, R_OK); poll_event(c, b, R_OK); // waits do not autoclear
    svc(c, 0x17, b, R_OK); poll_event(c, b, R_TIMEOUT);
    svc(c, 0x17, b, R_INVALID_STATE);
    svc(c, 0x16, b, R_OK); CHECK(event->refs == 0);
    retained_handle = event_handle(c, normal);
    CHECK(handle_get(retained_handle) == event && event->refs == 1);
    uint64_t start = rt_ticks();
    c->x[0] = 2000000; rt_svc(c, 0x0b, 0); // ordinary real/virtual guest SleepThread
    CHECK(rt_ticks() > start);
    ticks_zero(c, normal); poll_event(c, retained_handle, R_TIMEOUT);
    test_exit(c, normal);
    pthread_mutex_lock(&g_klock); kernel_signal(event); pthread_mutex_unlock(&g_klock);
    last_event = event; // Leave a signaled, live old handle across service reset.
}

static void wait_finished(KThread *thread) {
    uint64_t end = plat_now_ns() + 5000000000ull;
    for (;;) {
        if (blocked_close && !atomic_load(&close_sent)) {
            pthread_mutex_lock(&g_klock);
            bool blocked = thread->in_sync_wait && (!kernel_det() || thread->det_state == 3);
            pthread_mutex_unlock(&g_klock);
            if (blocked) {
                LOG("[fixture] guest blocked in WaitSynchronization; host close signal now\n");
                atomic_store(&close_sent, true);
                am_request_host_exit(plat_now_ns());
            }
        }
        if (atomic_load(&sdl_phase) == 1) {
            SDL_Event e = {.type = SDL_EVENT_WINDOW_FOCUS_LOST};
            CHECK(SDL_PushEvent(&e));
            e.type = SDL_EVENT_QUIT; CHECK(SDL_PushEvent(&e));
            bool saw_quit = false, saw_focus = false;
            while (SDL_PollEvent(&e)) {
                if (e.type == SDL_EVENT_QUIT) saw_quit = true;
                if (e.type == SDL_EVENT_WINDOW_FOCUS_LOST) saw_focus = true;
                CHECK(!applet_ui_should_exit(&e));
            }
            CHECK(saw_quit && saw_focus && !applet_ui_should_exit(NULL));
            atomic_store(&sdl_phase, 2);
        }
        pthread_mutex_lock(&g_klock); bool done = thread->finished; pthread_mutex_unlock(&g_klock);
        if (done) return;
        CHECK(plat_now_ns() < end); sched_yield();
    }
}

static pthread_barrier_t init_barrier;
static void *concurrent_init(void *out) {
    int rc = pthread_barrier_wait(&init_barrier);
    CHECK(rc == 0 || rc == PTHREAD_BARRIER_SERIAL_THREAD);
    uint32_t *handles = out;
    handles[0] = am_event_handle(false);
    handles[1] = am_event_handle(true);
    return NULL;
}
static void test_concurrent_init(void) {
    pthread_t threads[8]; uint32_t handles[8][2];
    CHECK(pthread_barrier_init(&init_barrier, NULL, 8) == 0);
    for (unsigned i = 0; i < 8; ++i) CHECK(pthread_create(&threads[i], NULL, concurrent_init, handles[i]) == 0);
    for (unsigned i = 0; i < 8; ++i) CHECK(pthread_join(threads[i], NULL) == 0);
    CHECK(pthread_barrier_destroy(&init_barrier) == 0);
    pthread_mutex_lock(&g_klock);
    CHECK(am.initialized && am.message_count == 1 && am.messages[0] == 15);
    CHECK(am.message_event->signaled && !am.suspended_event->signaled);
    CHECK(am.message_event->refs == 8 && am.suspended_event->refs == 8);
    pthread_mutex_unlock(&g_klock);
    Cpu c = {0};
    for (unsigned i = 0; i < 8; ++i) {
        CHECK(handle_get(handles[i][0]) == am.message_event && handle_get(handles[i][1]) == am.suspended_event);
        svc(&c, 0x16, handles[i][0], R_OK); svc(&c, 0x16, handles[i][1], R_OK);
    }
    CHECK(am.message_event->refs == 0 && am.suspended_event->refs == 0);
}

static void *concurrent_idle_policy(void *opaque) {
    const uintptr_t seed = (uintptr_t)opaque;
    for (unsigned i = 0; i < 128; ++i) {
        uint32_t mode = (uint32_t)((i + seed) % 3);
        IpcReq set = {.cmd = 62, .data = (const uint8_t *)&mode, .data_size = 4};
        IpcResp response = {0};
        self_controller(NULL, &set, &response);
        CHECK(!response.result && !response.data_size && !response.nobjects && !response.ncopy && !response.nmove);
        IpcReq get = {.cmd = 63}; response = (IpcResp){0};
        self_controller(NULL, &get, &response);
        uint32_t observed; memcpy(&observed, response.data, 4);
        CHECK(!response.result && response.data_size == 4 && observed <= 2);
    }
    return NULL;
}
static void test_concurrent_idle_policy(void) {
    pthread_t threads[4];
    for (uintptr_t i = 0; i < 4; ++i) CHECK(!pthread_create(threads + i, NULL, concurrent_idle_policy, (void *)i));
    for (unsigned i = 0; i < 4; ++i) CHECK(!pthread_join(threads[i], NULL));
    // Restore the initial enum without resetting other initialized AM state.
    uint32_t zero = 0;
    IpcReq set = {.cmd = 62, .data = (const uint8_t *)&zero, .data_size = 4};
    IpcResp response = {0}; self_controller(NULL, &set, &response); CHECK(!response.result);
    puts("concurrent idle policy set/query OK");
}

int main(int argc, char **argv) {
    if (argc == 2) {
        if (!strncmp(argv[1], "--idle-", 7)) idle_case = argv[1];
        else if (!strcmp(argv[1], "--domain-size-probe")) domain_size_probe = true;
        else if (!strcmp(argv[1], "--blocked-close")) blocked_close = true;
        else if (!strcmp(argv[1], "--timer-signal")) timer_signal_probe = true;
        else if (!strcmp(argv[1], "--early-close")) early_close = true;
        else if (!strcmp(argv[1], "--strict-negative")) strict_negative = 1;
        else if (!strcmp(argv[1], "--strict-window-negative")) strict_negative = 2;
        else if (!strcmp(argv[1], "--malformed-boolean")) strict_negative = 3;
        else if (!strcmp(argv[1], "--malformed-short")) strict_negative = 4;
        else if (!strcmp(argv[1], "--malformed-domain-short")) strict_negative = 5;
        else if (!strcmp(argv[1], "--malformed-domain-bounds")) strict_negative = 6;
        else return 2;
    } else if (argc != 1) return 2;
    CHECK(diag_strict());
    CHECK(SDL_Init(SDL_INIT_EVENTS));
    if (early_close) { am_request_host_exit(0); am_request_host_exit(1); CHECK(am_host_exit_ready(0)); }
    for (int i = 0; i < 8; ++i) {
        cycle = i; atomic_store(&sdl_phase, 0);
        kernel_init();
        if (!i && !idle_case && !strict_negative && !domain_size_probe && !blocked_close && !timer_signal_probe && !early_close) {
            test_concurrent_init(); test_concurrent_idle_policy();
        }
        KThread *thread = kernel_create_main_thread(1, 16384, 44);
        kernel_run_main(thread, 0, 0, 0, 0);
        wait_finished(thread);
        if (atomic_load(&sdl_phase) == 3) CHECK(applet_ui_should_exit(NULL));
        kernel_stop_threads(NULL); // Actual stop/join before either reset order.
        if (idle_case) { kernel_reset(); am_reset(); SDL_Quit(); return 0; }
        if (domain_size_probe) { kernel_reset(); SDL_Quit(); puts("domain payload size OK"); return 0; }
        if (timer_signal_probe || early_close) {
            kernel_reset(); am_reset(); SDL_Quit();
            puts(timer_signal_probe ? "timer signal grants baton after callback OK" : "early native close before IPC initialization OK");
            return 0;
        }
        if (blocked_close) {
            CHECK(am_host_exit_ready(plat_now_ns()));
            kernel_reset(); am_reset(); SDL_Quit();
            puts("blocked ExitRequest wake/cleanup OK"); return 0;
        }
        CHECK(last_event && last_event->refs == 1 && last_event->signaled);
        if (!(i & 1)) {
            am_reset();
            CHECK(handle_get(retained_handle) == last_event);
            CHECK(last_event->refs == 1 && last_event->signaled);
            // A close after the hook must still operate on the original object.
            Cpu c = {0}; svc(&c, 0x16, retained_handle, R_OK);
            CHECK(last_event->refs == 0);
            kernel_reset();
        } else {
            kernel_reset(); am_reset();
            CHECK(last_event->refs == 1); // inherited reset retains objects/refs
        }
        CHECK(!handle_get(retained_handle));
        previous_event = last_event;
    }
    SDL_Quit();
    printf("AM Linux (%s): native SDL close/exit lock/deadline, active-only and idle policies, foreground acquisition/focus/queue invariants, IPC/domain copy handles, event lifetime/clear, zero suspend ticks after guest sleep, 8 reset cycles OK (%u checks)\n",
           kernel_det() ? "deterministic" : "real-time", atomic_load(&checks));
    return 0;
}
