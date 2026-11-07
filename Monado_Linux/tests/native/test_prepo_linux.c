// [XCDE] LINUX-010: synthetic play-report request through actual IPC/kernel.
// No game input, reports, saves, network, files or GPU; refusal must be nonzero.
#include <stdatomic.h>
#include <sched.h>
#include <sys/mman.h>
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
typedef struct { uint32_t result, move, bytes; unsigned nm, nc, no; uint64_t value; } Reply;
static const char *scenario = "positive";
static uint32_t expected_result;
static unsigned buffer_mode; // independent unreadable event(X) and report(A) pointers
static uint8_t *guarded_input;
static const uintptr_t buffer_base = UINT64_C(0x6f12345000);
static unsigned extra; // 1: copy handle, 2: output buffer, 3: domain object
static bool report_buffers;

static void encode_a(uint32_t *p, uint64_t addr, uint64_t size) {
    p[0] = (uint32_t)size; p[1] = (uint32_t)addr;
    p[2] = (uint32_t)(((addr >> 32) & 15) << 28) |
           (uint32_t)(((addr >> 36) & 7) << 2) | (uint32_t)(((size >> 32) & 15) << 24);
}
static Reply send(Cpu *c, Endpoint e, uint32_t type, uint32_t cmd,
                  const void *data, unsigned size, bool pid, uint64_t placeholder) {
    _Alignas(16) uint32_t msg[128] = {0};
    CHECK(size <= 32);
    bool event_buffer = report_buffers && strcmp(scenario, "missing-event");
    bool report_buffer = report_buffers && strcmp(scenario, "missing-report");
    msg[0] = type | (event_buffer ? 1u << 16 : 0) | (report_buffer ? 1u << 20 : 0) | (extra == 2 ? 1u << 24 : 0);
    msg[1] = 8 + (size + 3) / 4 + (e.object ? 4 : 0) + (extra == 3 ? 1 : 0);
    uint32_t *p = msg + 2;
    if (pid || extra == 1) {
        msg[1] |= 0x80000000u;
        *p++ = (pid ? 1 : 0) | (extra == 1 ? 2 : 0);
        if (pid) { *p++ = (uint32_t)placeholder; *p++ = placeholder >> 32; }
        if (extra == 1) *p++ = e.handle;
    }
    // X is the event/string and A is the report data: distinct buffers,
    // not an auto-select pair. Permission denial must precede either read.
    uint64_t event_addr = buffer_mode & 1 ? buffer_base : buffer_base + 4096;
    uint64_t report_addr = buffer_mode & 2 ? buffer_base : buffer_base + 8192;
    if (!strcmp(scenario, "null-buffers")) event_addr = report_addr = 0;
    if (event_buffer) {
        uint64_t size = !strcmp(scenario, "oversized-buffers") ? 0xffff : 16;
        p[0] = (uint32_t)(size << 16) | (uint32_t)(((event_addr >> 32) & 15) << 12) |
               (uint32_t)(((event_addr >> 36) & 7) << 6); p[1] = (uint32_t)event_addr; p += 2;
    }
    if (report_buffer) {
        encode_a(p, report_addr, !strcmp(scenario, "oversized-buffers") ? UINT64_C(0xfffffffff) : 64); p += 3;
    }
    if (extra == 2) { encode_a(p, buffer_base + 12288, 64); p += 3; }
    p = (uint32_t *)(((uintptr_t)p + 15) & ~(uintptr_t)15);
    if (e.object) {
        p[0] = 1u | (extra == 3 ? 1u << 8 : 0) | ((!strcmp(scenario, "domain-bounds") ? 0x2000u : 16 + size) << 16);
        p[1] = e.object; p += 4;
    }
    memcpy(p, "SFCI", 4); p[2] = cmd;
    if (size) memcpy(p + 4, data, size);
    if (extra == 3) p[4 + (size + 3) / 4] = e.object;
    uint64_t saved = c->tpidrro; c->tpidrro = (uintptr_t)msg; c->x[0] = e.handle;
    rt_svc(c, 0x21, 0); c->tpidrro = saved;
    CHECK(c->x[0] == R_OK);
    Reply r = {0};
    if (type == 2) return r;
    CHECK(msg[0] == 0); p = msg + 2;
    if (msg[1] & 0x80000000u) {
        uint32_t hd = *p++; CHECK(!(hd & 1)); r.nc = (hd >> 1) & 15; r.nm = (hd >> 5) & 15;
        CHECK(r.nc == 0 && r.nm <= 1); if (r.nm) r.move = *p++;
    }
    p = (uint32_t *)(((uintptr_t)p + 15) & ~(uintptr_t)15);
    if (e.object) { r.no = *p; CHECK(r.no == 0); p += 4; }
    CHECK(!memcmp(p, "SFCO", 4)); r.result = p[2];
    uint32_t raw = msg[1] & 0x3ff; CHECK(raw >= 8u + (e.object ? 4u : 0));
    r.bytes = (raw - 8 - (e.object ? 4 : 0)) * 4;
    CHECK(r.bytes <= 8); memcpy(&r.value, p + 4, r.bytes);
    return r;
}
static void empty(Reply r) { CHECK(!r.bytes && !r.nc && !r.nm && !r.no); }
static Endpoint connect(Cpu *c, bool domain) {
    Endpoint sm = {.handle=ipc_connect_named_port("sm:")}; CHECK(sm.handle);
    const char name[8] = "prepo:u";
    Reply r = send(c, sm, 4, 1, name, sizeof name, false, 0);
    CHECK(!r.result && r.nm == 1 && !r.bytes && !r.nc && !r.no);
    Endpoint e = {.handle=r.move};
    empty(send(c, sm, 2, 0, NULL, 0, false, 0));
    c->x[0] = sm.handle; rt_svc(c, 0x16, 0); CHECK(c->x[0] == R_OK);
    if (domain) {
        r = send(c, e, 5, 0, NULL, 0, false, 0);
        CHECK(!r.result && r.bytes == 4 && r.value == 1 && !r.nm && !r.nc && !r.no); e.object = 1;
    }
    return e;
}
static bool is(const char *s) { return !strcmp(scenario, s); }
void guest_call(Cpu *c, uint64_t entry) {
    CHECK(entry == 1 && c->thread == kernel_current_thread());
    const bool malformed = is("short") || is("domain-short") || is("extra-data") || is("empty-data") ||
        is("missing-pid") || is("missing-buffers") || is("missing-event") || is("missing-report") ||
        is("copy-handle") || is("output-buffer") || is("domain-object");
    const bool transport_failure = malformed || is("unknown") || is("unknown-domain") || is("domain-bounds");
    for (unsigned domain = 0; domain < 2; ++domain) for (unsigned mode = 0; mode < 4; ++mode) {
        if (transport_failure && domain != (strstr(scenario, "domain") != NULL)) continue;
        report_buffers = false; extra = 0; Endpoint e = connect(c, domain);
        uint64_t payload[4] = {0x1020304050607080,0x8877665544332211,0,0};
        report_buffers = !is("missing-buffers"); buffer_mode = mode;
        if (is("copy-handle")) extra = 1;
        if (is("output-buffer")) extra = 2;
        if (is("domain-object")) extra = 3;
        unsigned size = is("short") ? 20 : is("domain-short") ? 23 : is("extra-data") ? 28 : is("empty-data") ? 0 : 24;
        uint32_t command = is("unknown") || is("unknown-domain") || is("unknown-nonstrict") ? 0x7fffffffu : 10103;
        Reply r = send(c, e, 4, command, payload, size, !is("missing-pid"), UINT64_C(0xfedcba9876543210));
        if (transport_failure) CHECK(!"structurally invalid or unsupported request returned");
        CHECK(r.result == expected_result && r.result != 0); empty(r);
        for (unsigned i = 4096; i < 16384; ++i) CHECK(guarded_input[i] == (uint8_t)(i ^ 0xa5));
        extra = 0;
        report_buffers = false;
        // A refusal must not corrupt the session: a standard control query
        // continues to work and each endpoint remains independently usable.
        Endpoint normal = e; normal.object = 0;
        r = send(c, normal, 5, 3, NULL, 0, false, 0);
        CHECK(!r.result && r.bytes == 4 && r.value == 0x8000 && !r.nm && !r.nc && !r.no);
    }
}
int main(int argc, char **argv) {
    if (argc < 2 || argc > 3) return 2;
    expected_result = (uint32_t)strtoul(argv[1], NULL, 0); CHECK(expected_result);
    if (argc == 3) scenario = argv[2];
    CHECK(diag_strict() == !is("unknown-nonstrict"));
    guarded_input = mmap((void *)buffer_base, 16384, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    CHECK(guarded_input == (void *)buffer_base);
    for (unsigned i = 0; i < 16384; ++i) guarded_input[i] = (uint8_t)(i ^ 0xa5);
    CHECK(!mprotect(guarded_input, 4096, PROT_NONE));
    for (unsigned cycle = 0; cycle < 4; ++cycle) {
        kernel_init(); KThread *thread = kernel_create_main_thread(1, 16384, 44);
        kernel_run_main(thread, 0, 0, 0, 0);
        uint64_t deadline = plat_now_ns() + UINT64_C(5000000000);
        for (;;) {
            pthread_mutex_lock(&g_klock); bool finished = thread->finished; pthread_mutex_unlock(&g_klock);
            if (finished) break; CHECK(plat_now_ns() < deadline); sched_yield();
        }
        kernel_stop_threads(NULL); kernel_reset();
    }
    CHECK(!munmap(guarded_input, 16384));
    printf("prepo Linux (%s): explicit refusal 0x%x, normal/domain IPC, distinct X-event/A-report buffers, protected reads and unchanged buffers and 4 reset cycles OK (%u checks)\n",
           kernel_det() ? "deterministic" : "real-time", expected_result, atomic_load(&checks));
    return 0;
}
