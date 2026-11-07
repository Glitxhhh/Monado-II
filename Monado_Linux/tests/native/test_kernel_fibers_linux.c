// [XCDE] LINUX-003: real kernel thread/longjmp/reset + real Linux fiber backend.
// Synthetic guest callbacks only; no game input, memory image or renderer.
#include <stdatomic.h>
#include <sched.h>
#include <fenv.h>
#include <xmmintrin.h>
#include "../../runtime/kernel.c"
#include "../../runtime/hle/fibers.h"

int ipc_handle_request(Cpu *c, uint32_t h) { (void)c; (void)h; return 0; }
void rt_dispatch(Cpu *c) { (void)c; }
GuestFn dispatch_lookup(uint64_t a) { (void)a; return NULL; }
Module g_modules[4]; int g_nmodules;
void crash_handler_install(void) { plat_crash_handler_install(); }
uint32_t ipc_connect_named_port(const char *s) { (void)s; return 0; }
void rt_fatal_trace(void) { abort(); }
void rt_fault_report(const char *s, uint64_t a, uint64_t p, uint64_t l) { (void)s; (void)a; (void)p; (void)l; abort(); }
bool rt_fault_watch(uint64_t a, uint64_t p) { (void)a; (void)p; return false; }
const char *rt_host_fn_name(uint64_t p, uint64_t *o) { (void)p; (void)o; return NULL; }
uint32_t vi_vsync_handle(void) { return 0; }
uint64_t renderer_present_count(void) { return 0; }
const char *loader_symbol_at(uint64_t a, uint64_t *o) { (void)a; (void)o; return NULL; }
void diag_crash_report(void *p) { (void)p; }

extern void hle__ZN2nn2os15InitializeFiberEPNS0_9FiberTypeEPFS2_PvES3_S3_mi(Cpu *);
extern void hle__ZN2nn2os13SwitchToFiberEPNS0_9FiberTypeE(Cpu *);
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); abort(); } } while (0)
static atomic_int entered;
static int mode;
static unsigned char guest_stack[16384];
static KThread *migration_source, *migration_destination;
static KObject gate;
static uint32_t gate_handle;
static atomic_int migrated;
static uint64_t stop_deadline;
__attribute__((noinline)) static uint32_t guest_fp_add(Cpu *c, uint32_t a, uint32_t b) {
    return fb_s(op_s(c, FOP_ADD, fv_s(a), fv_s(b), 0));
}
// These calls must observe the destination pthread after a fiber migrates.
// pthread_self is const-qualified by libc; the real kernel TLS APIs have their own barriers.
__attribute__((noinline)) static pthread_t native_identity(void) {
    __asm__ volatile("" ::: "memory"); return pthread_self();
}

static void switch_fiber(Cpu *c, uint64_t key) {
    c->x[0] = key;
    hle__ZN2nn2os13SwitchToFiberEPNS0_9FiberTypeE(c);
}

static void initialize_fiber(Cpu *c, uint64_t entry) {
    c->x[0] = 0x1000; c->x[1] = entry; c->x[2] = 0;
    c->x[3] = (uint64_t)(uintptr_t)guest_stack; c->x[4] = sizeof guest_stack; c->x[5] = 0;
    hle__ZN2nn2os15InitializeFiberEPNS0_9FiberTypeEPFS2_PvES3_S3_mi(c);
}

static void wait_gate(Cpu *c) {
    uint32_t handles[] = {gate_handle};
    c->x[1] = (uint64_t)(uintptr_t)handles; c->x[2] = 1; c->x[3] = UINT64_MAX;
    rt_svc(c, 0x18, 0); // Real infinite WaitSynchronization, cancelled by reboot.
    CHECK(mode == 3 && c->x[0] == R_OK); // Only migration/ExitThread signals this event.
}

void guest_call(Cpu *c, uint64_t entry) {
    if (entry == 20) { // Actual kernel pthread entry from a deliberately dirty parent.
        CHECK(fegetround() == FE_TONEAREST);
        CHECK((_mm_getcsr() & 0xffc0) == 0x1f80); // all masked, nearest, no FTZ/DAZ
        CHECK(c->fpcr == 0 && c->fpsr == 0);
        CHECK(guest_fp_add(c, 0x3f800000, 0x33800000) == 0x3f800000);
        CHECK(guest_fp_add(c, 1, 0x00800000) == 0x00800001);
        return;
    }
    if (entry == 1) {
        initialize_fiber(c, 2);
        switch_fiber(c, 0x1000);
        CHECK(mode == 0); // normal fiber return reaches root; exit/unwind must bypass it
        return;
    }
    if (entry == 10) {
        CHECK(kernel_current_thread() == migration_source);
        initialize_fiber(c, 12);
        switch_fiber(c, 0x1000); // Fiber suspends here before another KThread resumes it.
        CHECK(fegetround() == FE_TONEAREST);
        wait_gate(c); // Keep the original pthread alive, with its own native root.
        return;
    }
    if (entry == 11) {
        CHECK(kernel_current_thread() == migration_destination);
        switch_fiber(c, 0x1000);
        CHECK(!"migrated fiber ExitThread/stop must unwind its destination root");
    }
    if (entry == 12) {
        CHECK(c->thread == migration_source && kernel_current_thread() == migration_source);
        CHECK(c->tpidr == 0x1111 && c->tpidrro == migration_source->tlr);
        CHECK(kernel_current_cpu() == &migration_source->cpu);
        CHECK(kernel_current_thread_handle() == migration_source->handle);
        pthread_t original_host = native_identity();
        volatile uint64_t stack_word = 0x123456789abcdef0ull;
        c->x[19] = 0x987654321ull;
        CHECK(fesetround(FE_UPWARD) == 0);
        switch_fiber(c, 0);
        CHECK(!pthread_equal(original_host, native_identity()));
        CHECK(c->thread == migration_destination && kernel_current_thread() == migration_destination);
        CHECK(c->tpidr == 0x2222 && c->tpidrro == migration_destination->tlr);
        CHECK(kernel_current_cpu() == &migration_destination->cpu);
        CHECK(kernel_current_thread_handle() == migration_destination->handle);
        CHECK(stack_word == 0x123456789abcdef0ull && c->x[19] == 0x987654321ull);
        CHECK(fegetround() == FE_UPWARD);
        atomic_store(&migrated, 1);
        if (mode == 3) {
            c->x[0] = gate_handle; rt_svc(c, 0x11, 0); CHECK(c->x[0] == R_OK);
            rt_svc(c, 0x0A, 0); // Must target the new KThread's setjmp buffer.
            abort();
        }
        CHECK(mode == 4);
        for (;;) { rt_unwind_point(); sched_yield(); }
    }
    CHECK(entry == 2);
    CHECK(c->thread == kernel_current_thread());
    CHECK(c->tpidrro == kernel_current_thread()->tlr);
    atomic_store(&entered, 1);
    if (mode == 0) { c->x[0] = 0; return; }
    if (mode == 1) { rt_svc(c, 0x0A, 0); abort(); }
    if (mode == 5) { wait_gate(c); abort(); }
    for (;;) { rt_unwind_point(); sched_yield(); }
}

static void wait_thread_state(KThread *t, bool finished) {
    uint64_t deadline = plat_now_ns() + 5000000000ull;
    for (;;) {
        pthread_mutex_lock(&g_klock);
        bool ready = finished ? t->finished : t->in_sync_wait && (!g_det || t->det_state == 3);
        if (!finished) CHECK(!t->finished);
        pthread_mutex_unlock(&g_klock);
        if (ready) return;
        CHECK(plat_now_ns() < deadline); sched_yield();
    }
}

static void bounded_stop_idle(void) { CHECK(plat_now_ns() < stop_deadline); }

static void stop_and_reset(void) {
    stop_deadline = plat_now_ns() + 5000000000ull;
    kernel_stop_threads(bounded_stop_idle);
    CHECK(fibers_reset() == 0);
    CHECK(g_syncwaiters == NULL && g_waitlist == NULL);
    kernel_reset();
}

static void setup_gate(void) {
    gate = (KObject){.type = KOBJ_EVENT};
    gate_handle = kernel_new_handle(&gate);
}

static void test_migration_lifecycle(void) {
    for (mode = 3; mode <= 4; ++mode) {
        for (int iteration = 0; iteration < 2; ++iteration) {
            kernel_init(); setup_gate(); atomic_store(&migrated, 0);
            migration_source = kernel_create_main_thread(10, 16384, 44);
            migration_destination = thread_new(11, 0, (uint64_t)(uintptr_t)(guest_stack + sizeof guest_stack), 44, 0);
            migration_destination->handle = kernel_new_handle(&migration_destination->obj);
            migration_source->cpu.tpidr = 0x1111; migration_destination->cpu.tpidr = 0x2222;
            kernel_run_main(migration_source, 0, 0, 0, 0);
            wait_thread_state(migration_source, false);
            kernel_run_main(migration_destination, 0, 0, 0, 0);
            uint64_t deadline = plat_now_ns() + 5000000000ull;
            while (!atomic_load(&migrated)) { CHECK(plat_now_ns() < deadline); sched_yield(); }
            if (mode == 3) {
                wait_thread_state(migration_destination, true);
                wait_thread_state(migration_source, true);
            }
            stop_and_reset();
        }
    }
}

static void test_stop_blocked_fiber(void) {
    mode = 5;
    for (int iteration = 0; iteration < 2; ++iteration) {
        kernel_init(); setup_gate();
        KThread *t = kernel_create_main_thread(1, 16384, 44);
        kernel_run_main(t, 0, 0, 0, 0);
        wait_thread_state(t, false); // Observe the real queued wait, not just an entry flag.
        stop_and_reset();
    }
}

static void test_contended_diagnostic(void) {
    FILE *out = tmpfile(); CHECK(out != NULL);
    pthread_mutex_lock(&g_klock);
    uint64_t begin = plat_now_ns();
    kernel_dump_threads(out);
    CHECK(plat_now_ns() - begin < 3000000000ull);
    CHECK(fflush(out) == 0 && fseek(out, 0, SEEK_SET) == 0);
    char message[256] = {0}; CHECK(fread(message, 1, sizeof message - 1, out) > 0);
    CHECK(strstr(message, "thread details unavailable without a safe snapshot") != NULL);
    pthread_mutex_unlock(&g_klock);
    CHECK(fclose(out) == 0);
}

static void test_polluted_parent_fp(void) {
    fenv_t saved;
    CHECK(fegetenv(&saved) == 0);
    CHECK(fesetround(FE_UPWARD) == 0);
    CHECK(feraiseexcept(FE_INEXACT) == 0);
    _mm_setcsr(_mm_getcsr() | (1u << 15) | (1u << 6));
    unsigned parent_control = _mm_getcsr() & ~0x3fu;
    kernel_init();
    KThread *thread = kernel_create_main_thread(20, 16384, 44);
    kernel_run_main(thread, 0, 0, 0, 0);
    wait_thread_state(thread, true);
    stop_and_reset();
    CHECK(fegetround() == FE_UPWARD);
    CHECK((_mm_getcsr() & ~0x3fu) == parent_control);
    CHECK(fetestexcept(FE_INEXACT) == FE_INEXACT);
    CHECK(fesetenv(&saved) == 0);
}

int main(void) {
    test_polluted_parent_fp();
    for (mode = 0; mode < 3; mode++) {
        for (int iteration = 0; iteration < 10; ++iteration) {
            kernel_init();
            atomic_store(&entered, 0);
            KThread *t = kernel_create_main_thread(1, 16384, 44);
            kernel_run_main(t, 0, 0, 0, 0);
            uint64_t deadline = plat_now_ns() + 5000000000ull;
            while (!atomic_load(&entered)) { CHECK(plat_now_ns() < deadline); sched_yield(); }
            if (mode < 2) {
                for (;;) {
                    pthread_mutex_lock(&g_klock);
                    bool finished = t->finished;
                    pthread_mutex_unlock(&g_klock);
                    if (finished) break;
                    CHECK(plat_now_ns() < deadline); sched_yield();
                }
            }
            kernel_stop_threads(NULL);
            // Stop/join owns cleanup: no stale root or suspended/abandoned fiber remains.
            CHECK(fibers_reset() == 0);
            kernel_reset();
        }
    }
    test_migration_lifecycle();
    test_stop_blocked_fiber();
    test_contended_diagnostic();
    printf("kernel fibers (%s): polluted-parent FP entry + 30 return/ExitThread/stop-reset + 4 migrated lifecycle + 2 blocked-wait cycles; bounded diagnostics OK\n",
           kernel_det() ? "deterministic" : "real-time");
    return 0;
}
