// nn::os fibers over host (Windows) fibers (T032b, FR-022).  [XCDE]
//
// The SDK switches fibers by saving/restoring registers and `ret`-ing into the other context. Recompiled code runs on
// the host stack, so it cannot do that: each guest fiber gets a host fiber (its own host stack) and its own guest
// register file (Cpu). The guest stack (given to InitializeFiber) stays guest memory.
//
//   InitializeFiber(FiberType*, FiberFunction, void *arg, void *stack, size_t stack_size, int flags)
//   SwitchToFiber(FiberType*)      FinalizeFiber(FiberType*)      GetCurrentFiber() -> FiberType*
//   FiberFunction: FiberType *(*)(void *arg) -- returns the fiber to switch to when it ends (null: back to the thread)
//
// Lessons applied (research R10): a fiber may be resumed on another host thread than the one that suspended it, so
// its guest thread block (tpidr/tpidrro/thread) is handed over from the switching thread on every switch (ssbu #43);
// the per-thread "current fiber" is read through non-inlined accessors, because a compiler may keep a thread-local
// address across a switch that moves the code to another thread.
#include <stdlib.h>
#include <string.h>
#include "fibers.h"

#if defined(_WIN32)
#include <windows.h>

#include "../rt.h"

typedef struct HFiber {
    uint64_t key;                 // guest FiberType*
    uint64_t fn, arg, stack, stack_size;
    void *host;                   // the host fiber
    Cpu cpu;                      // this fiber's guest registers
    uint64_t tpidr, tpidrro;      // thread block of the host thread that runs it now (set by whoever switches to it)
    void *thread;
    struct HFiber *next;          // hash chain
} HFiber;

#define NBUCKETS 4096
static HFiber *g_buckets[NBUCKETS];
static CRITICAL_SECTION g_lock;
static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK init_lock(PINIT_ONCE o, PVOID p, PVOID *ctx) { (void)o; (void)p; (void)ctx; InitializeCriticalSection(&g_lock); return TRUE; }
static void lock(void) { InitOnceExecuteOnce(&g_once, init_lock, NULL, NULL); EnterCriticalSection(&g_lock); }
static void unlock(void) { LeaveCriticalSection(&g_lock); }

static unsigned bucket(uint64_t key) { return (unsigned)((key >> 4) * 0x9E3779B1u) % NBUCKETS; }
static HFiber *find(uint64_t key) {
    lock();
    HFiber *f = g_buckets[bucket(key)];
    while (f && f->key != key) f = f->next;
    unlock();
    return f;
}

// ---- per host thread state, through non-inlined accessors
static __thread uint64_t t_fiber;       // guest FiberType* running on this host thread now (0: the thread itself)
static __thread void *t_root;           // the host fiber of the thread itself
__attribute__((noinline)) static uint64_t cur_get(void) { __asm__ volatile("" ::: "memory"); return t_fiber; }
__attribute__((noinline)) static void cur_set(uint64_t k) { t_fiber = k; __asm__ volatile("" ::: "memory"); }
__attribute__((noinline)) static void *root_get(void) { __asm__ volatile("" ::: "memory"); return t_root; }
__attribute__((noinline)) static void root_set(void *r) { t_root = r; __asm__ volatile("" ::: "memory"); }

static void *ensure_root(void) {
    void *r = root_get();
    if (!r) {
        r = IsThreadAFiber() ? GetCurrentFiber() : ConvertThreadToFiber(NULL);
        root_set(r);
    }
    return r;
}

// hand the caller's thread block to the fiber about to run on this host thread
static void hand_over(HFiber *to, const Cpu *from) {
    to->tpidr = from->tpidr;
    to->tpidrro = from->tpidrro;
    to->thread = from->thread;
}
static void take_over(Cpu *c, const HFiber *f) {
    c->tpidr = f->tpidr;
    c->tpidrro = f->tpidrro;
    c->thread = f->thread;
}

static void CALLBACK fiber_main(void *p) {
    HFiber *f = p;
    for (;;) {
        Cpu *c = &f->cpu;
        take_over(c, f);
        c->x[0] = f->arg;
        c->sp = (f->stack + f->stack_size) & ~15ull;
        c->excl_addr = ~0ull;
        guest_call(c, f->fn);                 // returns the fiber to switch to next, or null
        uint64_t next = c->x[0];
        HFiber *n = next ? find(next) : NULL;
        if (next && !n) FATAL("[fiber] function of fiber %llx returned unknown fiber %llx\n", (unsigned long long)f->key, (unsigned long long)next);
        if (n) hand_over(n, c);
        cur_set(next);
        SwitchToFiber(n ? n->host : root_get());
        // only reached if a finished fiber is switched to again: it starts over with its function
    }
}

// ---- the HLE entry points (names fixed: gen.py --hle binds the SDK functions to hle_<mangled>)
void hle__ZN2nn2os15InitializeFiberEPNS0_9FiberTypeEPFS2_PvES3_S3_mi(Cpu *c) {
    uint64_t key = c->x[0];
    lock();
    HFiber *f = g_buckets[bucket(key)];
    while (f && f->key != key) f = f->next;
    if (!f) {
        f = calloc(1, sizeof *f);
        f->key = key;
        f->next = g_buckets[bucket(key)];
        g_buckets[bucket(key)] = f;
    } else if (f->host) {
        DeleteFiber(f->host);  // re-initialized without Finalize: start fresh
        f->host = NULL;
    }
    unlock();
    f->fn = c->x[1], f->arg = c->x[2], f->stack = c->x[3], f->stack_size = c->x[4];
    memset(&f->cpu, 0, sizeof f->cpu);
    f->cpu.excl_addr = ~0ull;
    f->host = CreateFiberEx(256 * 1024, (SIZE_T)64 << 20, 0, fiber_main, f);
    if (!f->host) FATAL("[fiber] CreateFiber failed (%lu)\n", GetLastError());
}

void hle__ZN2nn2os13FinalizeFiberEPNS0_9FiberTypeE(Cpu *c) {
    uint64_t key = c->x[0];
    lock();
    HFiber **pp = &g_buckets[bucket(key)];
    while (*pp && (*pp)->key != key) pp = &(*pp)->next;
    HFiber *f = *pp;
    if (f) *pp = f->next;
    unlock();
    if (f) {
        if (f->host) DeleteFiber(f->host);
        free(f);
    }
}

void hle__ZN2nn2os13SwitchToFiberEPNS0_9FiberTypeE(Cpu *c) {
    uint64_t key = c->x[0];
    void *root = ensure_root();
    HFiber *n = key ? find(key) : NULL;
    if (key && !n) FATAL("[fiber] SwitchToFiber to unknown fiber %llx\n", (unsigned long long)key);
    uint64_t self = cur_get();
    if (n) hand_over(n, c);
    cur_set(key);
    SwitchToFiber(n ? n->host : root);
    // resumed: possibly on another host thread than the one that suspended us. The one that switched to us wrote
    // its thread block into our record (or, for the thread's own context, nothing changed).
    cur_set(self);
    if (self) {
        HFiber *me = find(self);
        if (me) take_over(c, me);
    }
}

void hle__ZN2nn2os15GetCurrentFiberEv(Cpu *c) { c->x[0] = cur_get(); }

#elif defined(__linux__) && defined(__x86_64__)
// [XCDE] LINUX-003: preserve the HLE contract with guarded libc host contexts.
#include <errno.h>
#include <fenv.h>
#include <pthread.h>
#include "fiber_context_linux.h"
#include "../rt.h"

typedef struct HFiber {
    uint64_t key, fn, arg, stack, stack_size;
    FiberContext host;
    Cpu cpu;
    bool running, abandoned;
    struct HFiber *next;
} HFiber;
#define NBUCKETS 4096
static HFiber *g_buckets[NBUCKETS];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static size_t g_roots;
static __thread HFiber *t_fiber;
typedef struct FiberRoot {
    FiberContext host;
    fenv_t environment;
    bool environment_saved;
} FiberRoot;
static __thread FiberRoot *t_root;

// Access the destination pthread's TLS after every migration, never a cached address.
__attribute__((noinline)) static HFiber *cur_get(void) { __asm__ volatile("" ::: "memory"); return t_fiber; }
__attribute__((noinline)) static void cur_set(HFiber *f) { t_fiber = f; __asm__ volatile("" ::: "memory"); }
__attribute__((noinline)) static FiberRoot *root_get(void) { __asm__ volatile("" ::: "memory"); return t_root; }
__attribute__((noinline)) static void root_set(FiberRoot *r) { t_root = r; __asm__ volatile("" ::: "memory"); }
static void lock(void) { if (pthread_mutex_lock(&g_lock)) FATAL("[fiber] registry lock failed\n"); }
static void unlock(void) { if (pthread_mutex_unlock(&g_lock)) FATAL("[fiber] registry unlock failed\n"); }
static unsigned bucket(uint64_t key) { return (unsigned)((key >> 4) * 0x9E3779B1u) % NBUCKETS; }
static HFiber *find_locked(uint64_t key) {
    HFiber *f = g_buckets[bucket(key)];
    while (f && f->key != key) f = f->next;
    return f;
}
static FiberRoot *ensure_root(void) {
    FiberRoot *root = root_get();
    if (!root) {
        root = calloc(1, sizeof *root);
        if (!root) FATAL("[fiber] root allocation failed\n");
        root_set(root);
        lock(); g_roots++; unlock();
    }
    return root;
}
static void hand_over(HFiber *to, const Cpu *from) {
    to->cpu.tpidr = from->tpidr;
    to->cpu.tpidrro = from->tpidrro;
    to->cpu.thread = from->thread;
}
static void switch_to(Cpu *cpu, uint64_t key) {
    FiberRoot *root = ensure_root();
    lock();
    HFiber *from = cur_get(), *to = key ? find_locked(key) : NULL;
    if (key && !to) FATAL("[fiber] SwitchToFiber to unknown fiber %llx\n", (unsigned long long)key);
    if (!from && !to) FATAL("[fiber] root cannot switch to itself\n");
    if (to && (to->running || to->abandoned))
        FATAL("[fiber] target %llx is running or abandoned\n", (unsigned long long)key);
    if (from) from->running = false;
    if (to) { to->running = true; hand_over(to, cpu); }
    cur_set(to);
    if (!from) {
        if (fegetenv(&root->environment) != 0) FATAL("[fiber] cannot save root FP environment\n");
        root->environment_saved = true;
    }
    // Keep the mutex until the destination is executing: only then is the source
    // context saved and safe to migrate/finalize. It is unlocked on this same
    // pthread by the resumed switch_to frame or the first-entry trampoline.
    if (fiber_context_switch(from ? &from->host : &root->host, to ? &to->host : &root->host) != 0) {
        int error = errno; unlock(); FATAL("[fiber] context switch failed (%d)\n", error);
    }
    unlock();
    // Do not touch `to` or `root`: either could belong to a previous host thread.
    // The incoming switch wrote the new thread fields into our persistent Cpu.
}
static void fiber_main(void *argument) {
    HFiber *fiber = argument;
    unlock();
    for (;;) {
        Cpu *cpu = &fiber->cpu;
        cpu->x[0] = fiber->arg;
        cpu->sp = (fiber->stack + fiber->stack_size) & ~15ull;
        cpu->excl_addr = ~0ull;
        guest_call(cpu, fiber->fn);
        switch_to(cpu, cpu->x[0]);
        // A finished fiber explicitly resumed again restarts its entry function.
    }
}
void hle__ZN2nn2os15InitializeFiberEPNS0_9FiberTypeEPFS2_PvES3_S3_mi(Cpu *cpu) {
    uint64_t key = cpu->x[0];
    if (!key || !cpu->x[1] || !cpu->x[4] || cpu->x[3] > UINT64_MAX - cpu->x[4])
        FATAL("[fiber] invalid initialization arguments\n");
    lock();
    HFiber *fiber = find_locked(key);
    if (fiber && fiber->running) FATAL("[fiber] cannot reinitialize running fiber %llx\n", (unsigned long long)key);
    if (!fiber) {
        fiber = calloc(1, sizeof *fiber);
        if (!fiber) FATAL("[fiber] allocation failed\n");
        fiber->key = key;
        fiber->next = g_buckets[bucket(key)];
        g_buckets[bucket(key)] = fiber;
    } else fiber_context_destroy(&fiber->host);
    fiber->fn = cpu->x[1]; fiber->arg = cpu->x[2]; fiber->stack = cpu->x[3]; fiber->stack_size = cpu->x[4];
    memset(&fiber->cpu, 0, sizeof fiber->cpu);
    fiber->cpu.excl_addr = ~0ull;
    fiber->abandoned = false;
    if (!fiber_context_create(&fiber->host, (size_t)64 << 20, fiber_main, fiber))
        FATAL("[fiber] context allocation failed (%d)\n", errno);
    unlock();
}
void hle__ZN2nn2os13FinalizeFiberEPNS0_9FiberTypeE(Cpu *cpu) {
    uint64_t key = cpu->x[0];
    lock();
    HFiber **slot = &g_buckets[bucket(key)];
    while (*slot && (*slot)->key != key) slot = &(*slot)->next;
    HFiber *fiber = *slot;
    if (fiber) {
        if (fiber->running) FATAL("[fiber] cannot finalize running fiber %llx\n", (unsigned long long)key);
        *slot = fiber->next;
        fiber_context_destroy(&fiber->host);
        free(fiber);
    }
    unlock();
}
void hle__ZN2nn2os13SwitchToFiberEPNS0_9FiberTypeE(Cpu *cpu) { switch_to(cpu, cpu->x[0]); }
void hle__ZN2nn2os15GetCurrentFiberEv(Cpu *cpu) {
    HFiber *fiber = cur_get(); cpu->x[0] = fiber ? fiber->key : 0;
}
void fibers_thread_cleanup(void) {
    HFiber *current = cur_get();
    char stack_marker;
    if (current && (uintptr_t)&stack_marker >= (uintptr_t)current->host.mapping &&
        (uintptr_t)&stack_marker < (uintptr_t)current->host.mapping + current->host.mapping_size)
        FATAL("[fiber] cleanup must run on the native root stack\n");
    lock();
    if (current) { current->running = false; current->abandoned = true; }
    FiberRoot *root = root_get();
    if (root) {
        // longjmp bypasses swapcontext's FP restore. Reestablish the native root
        // environment before its exit/cleanup code runs after an abandoned fiber.
        if (current && root->environment_saved && fesetenv(&root->environment) != 0)
            FATAL("[fiber] cannot restore root FP environment\n");
        free(root); root_set(NULL); g_roots--;
    }
    cur_set(NULL);
    unlock();
}
size_t fibers_reset(void) {
    lock();
    if (g_roots) FATAL("[fiber] reset before all fiber threads cleaned up\n");
    size_t count = 0;
    for (size_t i = 0; i < NBUCKETS; ++i) {
        HFiber *fiber = g_buckets[i];
        while (fiber) {
            HFiber *next = fiber->next;
            if (fiber->running) FATAL("[fiber] reset with a running fiber\n");
            fiber_context_destroy(&fiber->host); free(fiber); count++; fiber = next;
        }
        g_buckets[i] = NULL;
    }
    unlock();
    return count;
}
#else
#error No host fiber implementation for this platform.
#endif
