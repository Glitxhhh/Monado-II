// Synthetic native pctl client lifecycle, using real IPC and kernel dispatch.
// No game input, policy provider, policy enforcement, files, saves or GPU.
#include <stdatomic.h>
#include <sched.h>
#include "../../runtime/kernel.c"
#include "../../runtime/ipc.c"
#include "../../runtime/services/misc.c"

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

typedef struct { uint32_t handle, object; } Endpoint;
typedef struct { uint32_t move, object, bytes; unsigned nm, nc, no; uint64_t value; } Reply;
static const char *scenario = "positive";
static Endpoint retained;
static Service *retained_service;
static Service *raw_closed_service;
static bool bad_domain_size;
static unsigned packet_extra; // 1: copy handle; 2: X buffer; 3: domain input object

static Reply send_request(Cpu *c, Endpoint e, uint32_t type, uint32_t cmd,
                          const void *data, unsigned size, bool pid, uint64_t placeholder,
                          bool close_object) {
    _Alignas(16) uint32_t msg[128] = {0};
    CHECK(size <= 16);
    msg[0] = type | (packet_extra == 2 ? 1u << 16 : 0);
    msg[1] = 8 + (size + 3) / 4 + (e.object ? 4 : 0) + (packet_extra == 3 ? 1 : 0);
    uint32_t *p = msg + 2;
    if (pid || packet_extra == 1) {
        msg[1] |= 0x80000000u;
        *p++ = (pid ? 1 : 0) | (packet_extra == 1 ? 2 : 0);
        if (pid) { *p++ = (uint32_t)placeholder; *p++ = placeholder >> 32; }
        if (packet_extra == 1) *p++ = e.handle;
    }
    if (packet_extra == 2) { *p++ = 0; *p++ = 0; }
    p = (uint32_t *)(((uintptr_t)p + 15) & ~(uintptr_t)15);
    if (e.object) {
        p[0] = (close_object ? 2u : 1u) | (packet_extra == 3 ? 1u << 8 : 0) |
               ((bad_domain_size ? 0x2000u : 16 + size) << 16);
        p[1] = e.object; p += 4;
    }
    memcpy(p, "SFCI", 4); p[2] = cmd;
    if (size) memcpy(p + 4, data, size);
    if (packet_extra == 3) p[4 + (size + 3) / 4] = e.object;
    uint64_t tls = c->tpidrro; c->tpidrro = (uintptr_t)msg; c->x[0] = e.handle;
    rt_svc(c, 0x21, 0); c->tpidrro = tls;
    CHECK(c->x[0] == R_OK);
    Reply r = {0};
    if (type == 2) return r;
    CHECK(msg[0] == 0); p = msg + 2;
    if (msg[1] & 0x80000000u) {
        uint32_t hd = *p++; CHECK(!(hd & 1));
        r.nc = (hd >> 1) & 15; r.nm = (hd >> 5) & 15;
        CHECK(r.nc == 0 && r.nm <= 1);
        if (r.nm) r.move = *p++;
    }
    p = (uint32_t *)(((uintptr_t)p + 15) & ~(uintptr_t)15);
    if (e.object) { r.no = *p; CHECK(r.no <= 1); p += 4; }
    CHECK(!memcmp(p, "SFCO", 4) && p[2] == 0);
    uint32_t raw = msg[1] & 0x3ff;
    CHECK(raw >= 8u + (e.object ? 4u + r.no : 0));
    r.bytes = (raw - 8 - (e.object ? 4 + r.no : 0)) * 4;
    CHECK(r.bytes <= 8); memcpy(&r.value, p + 4, r.bytes);
    if (r.no) r.object = p[4 + r.bytes / 4];
    return r;
}
static Reply request(Cpu *c, Endpoint e, uint32_t cmd, const void *data, unsigned size, bool pid, uint64_t placeholder) {
    return send_request(c, e, 4, cmd, data, size, pid, placeholder, false);
}
static void empty(Reply r) { CHECK(!r.bytes && !r.nc && !r.nm && !r.no); }
static void close_endpoint(Cpu *c, Endpoint e) {
    empty(send_request(c, e, e.object ? 4 : 2, 0, NULL, 0, false, 0, e.object != 0));
    if (!e.object) { c->x[0] = e.handle; rt_svc(c, 0x16, 0); CHECK(c->x[0] == R_OK); }
}
static Service *service_at(Endpoint e) {
    KSession *session = (KSession *)handle_get(e.handle);
    CHECK(session && session->obj.type == KOBJ_SESSION);
    return e.object ? session->domain->objs[e.object] : session->svc;
}
static Endpoint factory(Cpu *c, bool domain, const char *name) {
    Endpoint sm = {.handle = ipc_connect_named_port("sm:")}; CHECK(sm.handle);
    char encoded[8] = {0}; CHECK(strlen(name) <= sizeof encoded); memcpy(encoded, name, strlen(name));
    Reply r = request(c, sm, 1, encoded, sizeof encoded, false, 0);
    CHECK(r.nm == 1 && !r.bytes && !r.nc && !r.no);
    close_endpoint(c, sm);
    Endpoint e = {.handle = r.move};
    if (domain) {
        r = send_request(c, e, 5, 0, NULL, 0, false, 0, false);
        CHECK(r.bytes == 4 && r.value == 1 && !r.nm && !r.nc && !r.no);
        e.object = (uint32_t)r.value;
    }
    return e;
}
static Endpoint create(Cpu *c, Endpoint f, uint32_t cmd, uint64_t placeholder) {
    uint64_t reserved = 0;
    Reply r = request(c, f, cmd, &reserved, sizeof reserved, true, placeholder);
    CHECK(!r.bytes && !r.nc && (f.object ? r.no == 1 && !r.nm : r.nm == 1 && !r.no));
    return (Endpoint){.handle = f.object ? f.handle : r.move, .object = r.object};
}
static void state_is(Service *s, bool initialized) {
    pthread_mutex_lock(&g_klock);
    PctlState *state = s->state;
    CHECK(state && state->owner == s && state->guest_pid == 0x81 && state->initialized == initialized);
    CHECK(state->provider == (initialized ? PCTL_PROVIDER_UNAVAILABLE : PCTL_PROVIDER_UNBOUND));
    pthread_mutex_unlock(&g_klock);
}
static bool is(const char *name) { return !strcmp(scenario, name); }
void guest_call(Cpu *c, uint64_t entry) {
    CHECK(entry == 1 && c->thread == kernel_current_thread());
    if (!is("positive") && !is("stale-init") && !is("stale-query")) {
        bool domain = strstr(scenario, "domain") != NULL;
        Endpoint f = factory(c, domain, "pctl:a");
        uint64_t input[2] = {0};
        if (is("strict-factory")) request(c, f, 0x7fffffff, NULL, 0, false, 0);
        else if (!strncmp(scenario, "factory-", 8)) {
            unsigned size = is("factory-short") ? 4 : is("factory-domain-short") ? 7 : is("factory-extra") ? 12 : 8;
            if (is("factory-reserved")) input[0] = 1;
            if (is("factory-handle")) packet_extra = 1;
            if (is("factory-buffer")) packet_extra = 2;
            if (is("factory-domain-object")) packet_extra = 3;
            bad_domain_size = is("factory-domain-bounds");
            request(c, f, 1, input, size, !is("factory-no-pid"), 0);
        } else {
            Endpoint child = create(c, f, 1, 0);
            uint32_t cmd = is("strict-child") ? 0x7fffffff : is("preinit-query") ? 1031 : 1;
            if (is("init-handle")) packet_extra = 1;
            if (is("init-buffer")) packet_extra = 2;
            if (is("init-domain-object")) packet_extra = 3;
            request(c, child, cmd, input, is("init-data") || is("init-domain-data") ? 4 : 0, is("init-pid"), 0);
        }
        CHECK(!"malformed or unsupported request returned");
    }
    if (is("positive")) {
        const char *names[] = {"pctl", "pctl:a", "pctl:s", "pctl:r"};
        for (unsigned domain = 0; domain < 2; ++domain) for (unsigned n = 0; n < 4; ++n) {
            Endpoint f = factory(c, domain, names[n]);
            Endpoint a = create(c, f, 1, 0), b = create(c, f, 1, UINT64_C(0xfedcba9876543210));
            Endpoint legacy = create(c, f, 0, 0);
            Service *sa = service_at(a), *sb = service_at(b), *sl = service_at(legacy);
            CHECK(sa != sb && sa->state != sb->state);
            state_is(sa, false); state_is(sb, false); state_is(sl, true);
            empty(request(c, a, 1, NULL, 0, false, 0)); state_is(sa, true); state_is(sb, false);
            void *same = sa->state;
            empty(request(c, a, 1, NULL, 0, false, 0)); CHECK(sa->state == same); state_is(sa, true);
            empty(request(c, legacy, 1, NULL, 0, false, 0)); state_is(sl, true);
            if (!domain) {
                Reply clone = send_request(c, b, 5, 2, NULL, 0, false, 0, false);
                CHECK(clone.nm == 1 && !clone.bytes && sb->refs == 2);
                Endpoint bc = {.handle = clone.move}; CHECK(service_at(bc) == sb);
                close_endpoint(c, b); CHECK(sb->refs == 1); state_is(sb, false);
                empty(request(c, bc, 1, NULL, 0, false, 0)); state_is(sb, true);
                close_endpoint(c, bc);
            } else close_endpoint(c, b);
            CHECK(sb->state == NULL && sb->refs == 0);
            close_endpoint(c, a); close_endpoint(c, legacy);
            CHECK(!sa->state && !sl->state && sa->refs == 0 && sl->refs == 0);
            close_endpoint(c, f);
            CHECK(pctl_clients == NULL);
        }
        // The unchanged kernel's synthetic guest identity agrees with binding.
        rt_svc(c, 0x24, 0); CHECK(c->x[0] == R_OK && c->x[1] == 0x81);
    }
    Endpoint f = factory(c, false, "pctl:a");
    retained = create(c, f, 1, 0); retained_service = service_at(retained);
    empty(request(c, retained, 1, NULL, 0, false, 0)); state_is(retained_service, true);
    Endpoint raw_closed = create(c, f, 1, 0);
    raw_closed_service = service_at(raw_closed);
    c->x[0] = raw_closed.handle; rt_svc(c, 0x16, 0);
    CHECK(c->x[0] == R_OK && !handle_get(raw_closed.handle));
    // Raw CloseHandle does not release the Service in the inherited kernel.
    // The state remains owned by the per-boot list until the reset sweep.
    CHECK(raw_closed_service->refs == 1); state_is(raw_closed_service, false);
    close_endpoint(c, f); // keep an actual live client across stopped-thread reset
}
int main(int argc, char **argv) {
    if (argc == 2) scenario = argv[1]; else if (argc != 1) return 2;
    CHECK(diag_strict());
    for (unsigned cycle = 0; cycle < 8; ++cycle) {
        CHECK(pctl_clients == NULL);
        kernel_init();
        KThread *thread = kernel_create_main_thread(1, 16384, 44);
        kernel_run_main(thread, 0, 0, 0, 0);
        uint64_t deadline = plat_now_ns() + UINT64_C(5000000000);
        for (;;) {
            pthread_mutex_lock(&g_klock); bool finished = thread->finished; pthread_mutex_unlock(&g_klock);
            if (finished) break;
            CHECK(plat_now_ns() < deadline); sched_yield();
        }
        kernel_stop_threads(NULL);
        if (!(cycle & 1)) {
            pctl_reset(); CHECK(retained_service->state == NULL && raw_closed_service->state == NULL && pctl_clients == NULL);
            Cpu c = {0};
            if (is("stale-init") || is("stale-query")) {
                request(&c, retained, is("stale-init") ? 1 : 1031, NULL, 0, false, 0);
                CHECK(!"stale object returned");
            }
            close_endpoint(&c, retained); CHECK(retained_service->refs == 0);
            kernel_reset();
        } else {
            kernel_reset(); pctl_reset(); CHECK(retained_service->state == NULL && raw_closed_service->state == NULL && pctl_clients == NULL);
            retained_service->on_close(retained_service); // late cleanup must be harmless
        }
        CHECK(handle_get(retained.handle) == NULL);
        raw_closed_service->on_close(raw_closed_service); // reset already reclaimed it
    }
    printf("pctl Linux (%s): exact normal/domain IPC, all factory aliases, independent clients, PID presence/placeholder, initialization, clone/final close and 8 reset cycles OK (%u checks)\n",
           kernel_det() ? "deterministic" : "real-time", atomic_load(&checks));
    return 0;
}
