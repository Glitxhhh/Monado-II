// T031 (tests first): Horizon semantics of WaitProcessWideKeyAtomic (condition variable wait).
// On a timeout the SVC returns WITHOUT the mutex: the caller (nn::os) takes it again itself. Taking it in the SVC
// as well left the thread waiting on its own lock forever (LASRecomp #74/#88; seen in the first XCDE boot).
// kernel.c is included so its static SVC helpers can be called directly.
#include "../../runtime/kernel.c"

// ---- stubs for what kernel.c expects from the rest of the runtime
int ipc_handle_request(Cpu *c, uint32_t handle) { (void)c; (void)handle; return 0; }
void rt_dispatch(Cpu *c) { (void)c; }
GuestFn dispatch_lookup(uint64_t addr) { (void)addr; return NULL; }
Module g_modules[4]; int g_nmodules;
void guest_call(Cpu *c, uint64_t addr) { (void)c; (void)addr; }
void crash_handler_install(void) {}
uint32_t ipc_connect_named_port(const char *name) { (void)name; return 0; }
void rt_fatal_trace(void) { abort(); }
void rt_fault_report(const char *w, uint64_t a, uint64_t p, uint64_t l) { (void)w; (void)a; (void)p; (void)l; abort(); }
bool rt_fault_watch(uint64_t a, uint64_t p) { (void)a; (void)p; return false; }
const char *rt_host_fn_name(uint64_t pc, uint64_t *o) { (void)pc; (void)o; return NULL; }
uint32_t vi_vsync_handle(void) { return 0; }
const char *loader_symbol_at(uint64_t a, uint64_t *o) { (void)a; (void)o; return NULL; }
void diag_crash_report(void *ep) { (void)ep; }

static int fails;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); fails++; } } while (0)

int main(void) {
    static uint32_t mutex_word, key_word;
    KThread th; memset(&th, 0, sizeof th);
    pthread_cond_init(&th.cond, NULL);
    th.handle = 0x77;
    t_cur = &th;

    // 1. timed wait that runs out: returns R_TIMEOUT and the mutex is NOT held afterwards
    mutex_word = th.handle | 0;           // the caller owns the mutex (value = owner handle, no waiters)
    uint32_t r = svc_wait_process_wide_key((uint64_t)(uintptr_t)&mutex_word, (uint64_t)(uintptr_t)&key_word, th.handle, 20 * 1000000ll);
    CHECK(r == R_TIMEOUT);
    CHECK(mutex_word == 0);               // released, and not re-taken by the SVC
    CHECK(th.wait_kind == WAIT_NONE);     // not left waiting on its own lock

    // 2. the caller can take the mutex itself afterwards (what nn::os does): ArbitrateLock must not block
    mutex_word = th.handle | WAITERS_BIT; // simulate "mutex contended by others"? no: nobody owns it
    mutex_word = 0;
    CHECK(svc_arbitrate_lock(th.handle, (uint64_t)(uintptr_t)&mutex_word, th.handle) == R_OK);

    // 3. zero timeout behaves the same way
    mutex_word = th.handle;
    r = svc_wait_process_wide_key((uint64_t)(uintptr_t)&mutex_word, (uint64_t)(uintptr_t)&key_word, th.handle, 0);
    CHECK(r == R_TIMEOUT);
    CHECK(mutex_word == 0);

    printf(fails ? "FAILED (%d)\n" : "OK\n", fails);
    return fails ? 1 : 0;
}
