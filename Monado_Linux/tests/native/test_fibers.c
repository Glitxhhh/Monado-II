// T032b (tests first): nn::os fibers over host fibers. Guest functions are simulated by host functions
// `FiberId fn(Cpu*)`: guest_call() below calls them with c->x[0] = arg and takes the next fiber from c->x[0].
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#include <process.h>
#else
#include <pthread.h>
#include <fenv.h>
#include <setjmp.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../../runtime/hle/fibers.h"
#endif

#include "../../runtime/cpu.h"

// ---- hooks the fiber layer expects from the runtime
void guest_call(Cpu *c, uint64_t addr) { ((void (*)(Cpu *))(uintptr_t)addr)(c); }
void rt_fatal_trace(void) { fflush(stdout); abort(); }

// ---- the HLE entry points under test (runtime/hle/fibers.c)
extern void hle__ZN2nn2os15InitializeFiberEPNS0_9FiberTypeEPFS2_PvES3_S3_mi(Cpu *c);
extern void hle__ZN2nn2os13FinalizeFiberEPNS0_9FiberTypeE(Cpu *c);
extern void hle__ZN2nn2os13SwitchToFiberEPNS0_9FiberTypeE(Cpu *c);
extern void hle__ZN2nn2os15GetCurrentFiberEv(Cpu *c);


static int fails;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); __atomic_fetch_add(&fails, 1, __ATOMIC_RELAXED); } } while (0)

static uint64_t fib_init(Cpu *c, uint64_t type, void (*fn)(Cpu *), uint64_t arg, uint64_t stack, uint64_t size) {
    c->x[0] = type; c->x[1] = (uint64_t)(uintptr_t)fn; c->x[2] = arg; c->x[3] = stack; c->x[4] = size; c->x[5] = 0;
    hle__ZN2nn2os15InitializeFiberEPNS0_9FiberTypeEPFS2_PvES3_S3_mi(c);
    return type;
}
static void fib_switch(Cpu *c, uint64_t target) { c->x[0] = target; hle__ZN2nn2os13SwitchToFiberEPNS0_9FiberTypeE(c); }
static uint64_t fib_current(Cpu *c) { hle__ZN2nn2os15GetCurrentFiberEv(c); return c->x[0]; }
static void fib_final(Cpu *c, uint64_t t) { c->x[0] = t; hle__ZN2nn2os13FinalizeFiberEPNS0_9FiberTypeE(c); }

static char log_[256]; static int logn;
static void logc(char ch) { if (logn < 255) log_[logn++] = ch, log_[logn] = 0; }

static uint64_t F_A = 0x1000, F_B = 0x2000, F_C = 0x3000;
static uint8_t stackmem[3][0x4000];

// A: logs 'a', switches to B, (resumed) logs 'A', returns C
static void fn_a(Cpu *c) { logc('a'); CHECK(fib_current(c) == F_A); fib_switch(c, F_B); logc('A'); c->x[0] = F_C; }
// B: logs 'b', switches back to A, then (never resumed again in this test)
static void fn_b(Cpu *c) { logc('b'); CHECK(fib_current(c) == F_B); fib_switch(c, F_A); logc('B'); c->x[0] = 0; }
// C: logs 'c', returns null -> back to the thread
static void fn_c(Cpu *c) { logc('c'); c->x[0] = 0; }

static void test_ping_pong(void) {
    Cpu root; memset(&root, 0, sizeof root);
    root.tpidr = 0x111; root.tpidrro = 0x222;
    logn = 0; log_[0] = 0;
    fib_init(&root, F_A, fn_a, 7, (uint64_t)(uintptr_t)stackmem[0], sizeof stackmem[0]);
    fib_init(&root, F_B, fn_b, 8, (uint64_t)(uintptr_t)stackmem[1], sizeof stackmem[1]);
    fib_init(&root, F_C, fn_c, 9, (uint64_t)(uintptr_t)stackmem[2], sizeof stackmem[2]);
    CHECK(fib_current(&root) == 0);           // a thread outside any fiber
    fib_switch(&root, F_A);                   // thread -> A -> B -> A -> C -> (null) -> thread
    CHECK(strcmp(log_, "abAc") == 0);
    if (strcmp(log_, "abAc")) printf("  got '%s'\n", log_);
    CHECK(fib_current(&root) == 0);
    fib_final(&root, F_A); fib_final(&root, F_B); fib_final(&root, F_C);
}

// the thread block (tpidr/tpidrro) of the host thread that runs the fiber now must be what the fiber sees
static uint64_t seen_tp, seen_tprw;
static void fn_probe(Cpu *c) { seen_tp = c->tpidr; seen_tprw = c->tpidrro; c->x[0] = 0; }
static void test_thread_block(void) {
    Cpu root; memset(&root, 0, sizeof root);
    root.tpidr = 0xAAAA; root.tpidrro = 0xBBBB;
    fib_init(&root, F_A, fn_probe, 0, (uint64_t)(uintptr_t)stackmem[0], sizeof stackmem[0]);
    fib_switch(&root, F_A);
    CHECK(seen_tp == 0xAAAA && seen_tprw == 0xBBBB);
    fib_final(&root, F_A);
}

// a fiber suspended on thread 1 is resumed on thread 2 and must see thread 2's block (ssbu #43)
#ifdef _WIN32
static HANDLE ev_suspended, ev_done;
static uint64_t tp_before, tp_after;
static uint64_t shared_fiber = 0x5000;
static void fn_migrant(Cpu *c) {
    tp_before = c->tpidr;
    fib_switch(c, 0);              // suspend: back to the thread that started us
    tp_after = c->tpidr;           // resumed, on another thread
    c->x[0] = 0;
}
static unsigned __stdcall thread1(void *p) {
    (void)p;
    Cpu root; memset(&root, 0, sizeof root);
    root.tpidr = 0x1111;
    fib_init(&root, shared_fiber, fn_migrant, 0, (uint64_t)(uintptr_t)stackmem[1], sizeof stackmem[1]);
    fib_switch(&root, shared_fiber);   // runs to its own suspend
    SetEvent(ev_suspended);
    WaitForSingleObject(ev_done, 10000);
    return 0;
}
static unsigned __stdcall thread2(void *p) {
    (void)p;
    Cpu root; memset(&root, 0, sizeof root);
    root.tpidr = 0x2222;
    fib_switch(&root, shared_fiber);   // resume on this thread
    fib_final(&root, shared_fiber);
    return 0;
}
static void test_migration(void) {
    ev_suspended = CreateEvent(NULL, TRUE, FALSE, NULL);
    ev_done = CreateEvent(NULL, TRUE, FALSE, NULL);
    HANDLE t1 = (HANDLE)_beginthreadex(NULL, 0, thread1, NULL, 0, NULL);
    WaitForSingleObject(ev_suspended, 10000);
    HANDLE t2 = (HANDLE)_beginthreadex(NULL, 0, thread2, NULL, 0, NULL);
    WaitForSingleObject(t2, 10000);
    SetEvent(ev_done);
    WaitForSingleObject(t1, 10000);
    CHECK(tp_before == 0x1111);
    CHECK(tp_after == 0x2222);
    if (tp_after != 0x2222) printf("  tp_before=%llx tp_after=%llx\n", (unsigned long long)tp_before, (unsigned long long)tp_after);
}

#else
// [XCDE] LINUX-003: repeatedly resume one host stack on two distinct pthreads.
static pthread_mutex_t migration_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t migration_cond = PTHREAD_COND_INITIALIZER;
static int migration_turn;
static __thread unsigned migration_thread;
__attribute__((noinline)) static unsigned native_thread(void) { __asm__ volatile("" ::: "memory"); return migration_thread; }
static void fn_migrant(Cpu *c) {
    volatile uint64_t words[128];
    for (unsigned i = 0; i < 128; ++i) words[i] = 0x123456789abcdef0ull ^ i;
    c->x[19] = 0x123456789aull; c->v[11] = ((__uint128_t)0x1234 << 64) | 0x5678;
    c->fpcr = 0x123; c->fpsr = 0x456;
    CHECK(fesetround(FE_UPWARD) == 0);
    for (int i = 0; i < 1000; ++i) {
        unsigned id = (unsigned)i % 2;
        CHECK(native_thread() == id + 1);
        CHECK(c->tpidr == 0x1000 + id && c->tpidrro == 0x2000 + id);
        CHECK(c->thread == (void *)(uintptr_t)(0x3000 + id));
        CHECK(fib_current(c) == F_A);
        CHECK(c->x[19] == 0x123456789aull);
        CHECK(c->v[11] == (((__uint128_t)0x1234 << 64) | 0x5678));
        CHECK(c->fpcr == 0x123 && c->fpsr == 0x456);
        CHECK(c->sp == (((uint64_t)(uintptr_t)stackmem[0] + sizeof stackmem[0]) & ~15ull));
        CHECK(fegetround() == FE_UPWARD);
        for (unsigned k = 0; k < 128; ++k) CHECK(words[k] == (0x123456789abcdef0ull ^ k));
        fib_switch(c, 0);
    }
    c->x[0] = 0;
}
static void *migration_worker(void *arg) {
    unsigned id = (unsigned)(uintptr_t)arg;
    migration_thread = id + 1;
    Cpu root = {0}; root.tpidr = 0x1000 + id; root.tpidrro = 0x2000 + id;
    root.thread = (void *)(uintptr_t)(0x3000 + id);
    int rounding = id ? FE_TOWARDZERO : FE_DOWNWARD;
    CHECK(fesetround(rounding) == 0);
    for (int i = (int)id; i < 1000; i += 2) {
        pthread_mutex_lock(&migration_lock);
        while (migration_turn != i) pthread_cond_wait(&migration_cond, &migration_lock);
        pthread_mutex_unlock(&migration_lock);
        fib_switch(&root, F_A);
        CHECK(fib_current(&root) == 0);
        CHECK(fegetround() == rounding);
        pthread_mutex_lock(&migration_lock); migration_turn++; pthread_cond_broadcast(&migration_cond);
        pthread_mutex_unlock(&migration_lock);
    }
    fibers_thread_cleanup();
    return NULL;
}
static void test_migration(void) {
    Cpu root = {0};
    fib_init(&root, F_A, fn_migrant, 0, (uint64_t)(uintptr_t)stackmem[0], sizeof stackmem[0]);
    migration_turn = 0;
    pthread_t a, b;
    CHECK(pthread_create(&a, NULL, migration_worker, (void *)0) == 0);
    CHECK(pthread_create(&b, NULL, migration_worker, (void *)1) == 0);
    CHECK(pthread_join(a, NULL) == 0); CHECK(pthread_join(b, NULL) == 0);
    fib_final(&root, F_A);
}
#endif

// many create/switch/finalize cycles: no growth, no crash
static void fn_quick(Cpu *c) { c->x[0] = 0; }
static void test_churn(void) {
    Cpu root; memset(&root, 0, sizeof root);
    static uint8_t st[0x4000];
    for (int i = 0; i < 20000; i++) {
        uint64_t t = 0x100000 + (uint64_t)(i % 512) * 0x100;
        fib_init(&root, t, fn_quick, 0, (uint64_t)(uintptr_t)st, sizeof st);
        fib_switch(&root, t);
        fib_final(&root, t);
    }
    CHECK(1);
}

#ifndef _WIN32
// [XCDE] LINUX-003: interrupted guest stacks are reclaimed only after root unwind.
static __thread jmp_buf unwind_target;
static void fn_unwind(Cpu *c) {
    (void)c; CHECK(fesetround(FE_UPWARD) == 0); longjmp(unwind_target, 1);
}
static void *unwind_worker(void *p) {
    (void)p;
    Cpu root = {0};
    CHECK(fesetround(FE_DOWNWARD) == 0);
    fib_init(&root, F_A, fn_unwind, 0, (uint64_t)(uintptr_t)stackmem[0], sizeof stackmem[0]);
    if (!setjmp(unwind_target)) fib_switch(&root, F_A);
    fibers_thread_cleanup();
    CHECK(fegetround() == FE_DOWNWARD);
    CHECK(fib_current(&root) == 0);
    return NULL;
}
static void test_unwind_reset(void) {
    fibers_thread_cleanup();  // main's previous completed root
    for (int i = 0; i < 20; ++i) {
        pthread_t worker; CHECK(pthread_create(&worker, NULL, unwind_worker, NULL) == 0);
        CHECK(pthread_join(worker, NULL) == 0);
        CHECK(fibers_reset() == 1);
        CHECK(fibers_reset() == 0);
    }
}
static int starts;
static void fn_count(Cpu *c) { starts++; c->x[0] = 0; }
static void test_restart_and_reinitialize(void) {
    Cpu root = {0}; starts = 0;
    fib_init(&root, F_A, fn_count, 0, (uint64_t)(uintptr_t)stackmem[0], sizeof stackmem[0]);
    fib_switch(&root, F_A); fib_switch(&root, F_A); CHECK(starts == 2);
    fib_init(&root, F_A, fn_count, 0, (uint64_t)(uintptr_t)stackmem[0], sizeof stackmem[0]);
    fib_switch(&root, F_A); CHECK(starts == 3);
    fib_final(&root, F_A); fib_final(&root, F_A); // inherited harmless missing-finalize behavior
}
static void fn_self_switch(Cpu *c) { fib_switch(c, F_A); }
static void fn_self_finalize(Cpu *c) { fib_final(c, F_A); }
static void fn_self_reinitialize(Cpu *c) { fib_init(c, F_A, fn_count, 0, (uint64_t)(uintptr_t)stackmem[0], sizeof stackmem[0]); }
static void fn_bad_cleanup(Cpu *c) { (void)c; fibers_thread_cleanup(); }
static int running_ready;
static void fn_running(Cpu *c) {
    (void)c; __atomic_store_n(&running_ready, 1, __ATOMIC_RELEASE);
    for (;;) usleep(1000);
}
static void *running_worker(void *unused) {
    (void)unused; Cpu root = {0};
    fib_init(&root, F_A, fn_running, 0, (uint64_t)(uintptr_t)stackmem[0], sizeof stackmem[0]);
    fib_switch(&root, F_A); return NULL;
}
static void death_case(int which) {
    Cpu root = {0};
    if (which == 0) fib_switch(&root, 0xdeadbeef);
    if (which == 1) fib_switch(&root, 0);
    if (which == 6) {
        pthread_t worker; CHECK(pthread_create(&worker, NULL, running_worker, NULL) == 0);
        while (!__atomic_load_n(&running_ready, __ATOMIC_ACQUIRE)) usleep(1000);
        fib_final(&root, F_A);  // another pthread is executing this stack
    }
    if (which == 7) {
        pthread_t worker; CHECK(pthread_create(&worker, NULL, unwind_worker, NULL) == 0);
        CHECK(pthread_join(worker, NULL) == 0);
        fib_switch(&root, F_A);  // longjmp abandoned the saved continuation
    }
    if (which == 8) {
        fib_init(&root, F_A, fn_count, 0, (uint64_t)(uintptr_t)stackmem[0], sizeof stackmem[0]);
        fib_switch(&root, F_A); fibers_reset();  // native root still registered
    }
    void (*fn)(Cpu *) = which == 2 ? fn_self_switch : which == 3 ? fn_self_finalize :
        which == 4 ? fn_self_reinitialize : fn_bad_cleanup;
    fib_init(&root, F_A, fn, 0, (uint64_t)(uintptr_t)stackmem[0], sizeof stackmem[0]);
    fib_switch(&root, F_A);
}
static void test_invalid_operations(void) {
    for (int which = 0; which < 9; ++which) {
        fflush(NULL);
        pid_t child = fork(); CHECK(child >= 0);
        if (!child) {
            struct rlimit limit = {0, 0}; setrlimit(RLIMIT_CORE, &limit); alarm(10);
            death_case(which); _exit(99);
        }
        int status; CHECK(waitpid(child, &status, 0) == child);
        CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
    }
}
#endif

int main(void) {
    test_ping_pong();
    test_thread_block();
    test_migration();
    test_churn();
#ifndef _WIN32
    test_restart_and_reinitialize();
    test_unwind_reset();
    test_invalid_operations();
    fibers_thread_cleanup();
    CHECK(fibers_reset() == 0);
#endif
    printf(fails ? "FAILED (%d)\n" : "OK\n", fails);
    return fails ? 1 : 0;
}
