// [XCDE] WINPARITY-001: native Windows fibers, guarded ownership and TLS migration.
// Guest Cpu and host FP state remain independent of the destination host thread.
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "../rt.h"
#include "../platform/guest_fp_windows.h"
#include "fibers.h"

typedef struct HFiber {
    uint64_t key, fn, arg, stack, stack_size;
    void *host;
    Cpu cpu;
    bool running, abandoned;
    struct HFiber *next;
} HFiber;
typedef struct FiberRoot { void *host; bool converted, rescue; } FiberRoot;
#define NBUCKETS 4096
static HFiber *g_buckets[NBUCKETS];
static CRITICAL_SECTION g_lock;
static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;
static size_t g_roots;
static __thread uint64_t t_fiber;
static __thread FiberRoot *t_root;
static BOOL CALLBACK init_lock(PINIT_ONCE o, PVOID p, PVOID *ctx) {
    (void)o; (void)p; (void)ctx; InitializeCriticalSection(&g_lock); return TRUE;
}
static void lock(void) { InitOnceExecuteOnce(&g_once, init_lock, NULL, NULL); EnterCriticalSection(&g_lock); }
static void unlock(void) { LeaveCriticalSection(&g_lock); }
static unsigned bucket(uint64_t key) { return (unsigned)((key >> 4) * 0x9E3779B1u) % NBUCKETS; }
static HFiber *find_locked(uint64_t key) {
    HFiber *f = g_buckets[bucket(key)]; while (f && f->key != key) f = f->next; return f;
}
// Never retain the address of a thread-local object across a migratory switch.
__attribute__((noinline)) static uint64_t cur_get(void) { __asm__ volatile("" ::: "memory"); return t_fiber; }
__attribute__((noinline)) static void cur_set(uint64_t key) { t_fiber = key; __asm__ volatile("" ::: "memory"); }
__attribute__((noinline)) static FiberRoot *root_get(void) { __asm__ volatile("" ::: "memory"); return t_root; }
__attribute__((noinline)) static void root_set(FiberRoot *root) { t_root = root; __asm__ volatile("" ::: "memory"); }
static FiberRoot *ensure_root(void) {
    FiberRoot *root = root_get();
    if (!root) {
        root = calloc(1, sizeof *root);
        if (!root) FATAL("[fiber] root allocation failed\n");
        root->converted = !IsThreadAFiber();
        root->host = root->converted ? ConvertThreadToFiberEx(NULL, FIBER_FLAG_FLOAT_SWITCH) : GetCurrentFiber();
        if (!root->host) FATAL("[fiber] ConvertThreadToFiberEx failed (%lu)\n", GetLastError());
        root_set(root); lock(); ++g_roots; unlock();
    }
    return root;
}
static void switch_to(Cpu *c, uint64_t key) {
    FiberRoot *root = ensure_root();
    lock();
    uint64_t self = cur_get();
    HFiber *from = self ? find_locked(self) : NULL, *to = key ? find_locked(key) : NULL;
    if ((self && !from) || (key && !to)) FATAL("[fiber] unknown switch source or destination\n");
    if ((!from && !to) || (to && (to->running || to->abandoned))) FATAL("[fiber] destination is running, abandoned or self\n");
    if (from) from->running = false;
    if (to) {
        to->running = true;
        to->cpu.tpidr = c->tpidr; to->cpu.tpidrro = c->tpidrro; to->cpu.thread = c->thread;
    }
    cur_set(key);
    // Keep the lock until Win32 has saved the source context. The destination
    // releases it on this same native thread, including after migration.
    SwitchToFiber(to ? to->host : root->host);
    unlock();
    // The kernel requests nonlocal unwinding BEFORE calling longjmp. Switch
    // back to the real root first: MinGW/SEH cannot unwind across fiber stacks.
    FiberRoot *current_root = root_get();
    if (current_root && current_root->rescue) {
        current_root->rescue = false; cur_set(0); kernel_fiber_unwind();
    }
    cur_set(self);
    // The incoming switch has already published the new thread block directly
    // into this persistent Cpu. Do not dereference the old thread's root.
}
static void CALLBACK fiber_main(void *argument) {
    HFiber *f = argument;
    unlock();
    if (!monado_windows_guest_fp_init()) FATAL("[fiber] cannot establish guest FP environment\n");
    for (;;) {
        Cpu *c = &f->cpu;
        c->x[0] = f->arg; c->sp = (f->stack + f->stack_size) & ~15ull; c->excl_addr = ~0ull;
        guest_call(c, f->fn);
        switch_to(c, c->x[0]);
    }
}
void hle__ZN2nn2os15InitializeFiberEPNS0_9FiberTypeEPFS2_PvES3_S3_mi(Cpu *c) {
    uint64_t key = c->x[0];
    if (!key) FATAL("[fiber] null fiber identity\n");
    lock();
    HFiber *f = find_locked(key);
    if (f && (f->running || f->abandoned)) FATAL("[fiber] cannot initialize active or abandoned fiber\n");
    if (!f) {
        f = calloc(1, sizeof *f); if (!f) FATAL("[fiber] allocation failed\n");
        f->key = key; f->next = g_buckets[bucket(key)]; g_buckets[bucket(key)] = f;
    } else if (f->host) { DeleteFiber(f->host); f->host = NULL; }
    f->fn = c->x[1]; f->arg = c->x[2]; f->stack = c->x[3]; f->stack_size = c->x[4];
    memset(&f->cpu, 0, sizeof f->cpu); f->cpu.excl_addr = ~0ull;
    f->host = CreateFiberEx(256 * 1024, (SIZE_T)64 << 20, FIBER_FLAG_FLOAT_SWITCH, fiber_main, f);
    if (!f->host) FATAL("[fiber] CreateFiberEx failed (%lu)\n", GetLastError());
    unlock();
}
void hle__ZN2nn2os13FinalizeFiberEPNS0_9FiberTypeE(Cpu *c) {
    uint64_t key = c->x[0]; lock();
    HFiber **p = &g_buckets[bucket(key)]; while (*p && (*p)->key != key) p = &(*p)->next;
    HFiber *f = *p;
    if (f && f->running) FATAL("[fiber] cannot finalize a running fiber\n");
    if (f) { *p = f->next; if (f->host) DeleteFiber(f->host); free(f); }
    unlock();
}
void hle__ZN2nn2os13SwitchToFiberEPNS0_9FiberTypeE(Cpu *c) { switch_to(c, c->x[0]); }
void hle__ZN2nn2os15GetCurrentFiberEv(Cpu *c) { c->x[0] = cur_get(); }
void fibers_prepare_unwind(void) {
    FiberRoot *root = root_get(); if (!root) return;
    if (GetCurrentFiber() != root->host) {
        lock(); HFiber *f = find_locked(cur_get());
        if (!f || !f->running) FATAL("[fiber] invalid nonlocal unwind ownership\n");
        f->abandoned = true; f->running = false; root->rescue = true; cur_set(0);
        SwitchToFiber(root->host);
        FATAL("[fiber] abandoned context resumed\n");
    }
}
void fibers_thread_cleanup(void) {
    FiberRoot *root = root_get(); if (!root) return;
    if (GetCurrentFiber() != root->host) FATAL("[fiber] cleanup must run on the real Win32 root\n");
    cur_set(0);
    if (root->converted && !ConvertFiberToThread()) FATAL("[fiber] ConvertFiberToThread failed (%lu)\n", GetLastError());
    root_set(NULL); free(root); lock(); --g_roots; unlock();
}
size_t fibers_reset(void) {
    size_t count = 0; lock();
    if (g_roots) FATAL("[fiber] reset requires joined guest roots\n");
    for (size_t i = 0; i < NBUCKETS; ++i) {
        HFiber *f = g_buckets[i];
        while (f) {
            HFiber *next = f->next;
            if (f->running) FATAL("[fiber] reset found a running fiber\n");
            if (f->host) DeleteFiber(f->host);
            free(f); ++count; f = next;
        }
        g_buckets[i] = NULL;
    }
    unlock(); return count;
}
