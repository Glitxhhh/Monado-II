// Kernel object types shared by kernel.c and the IPC/service layer.
#pragma once
#include <stdio.h>
#include <setjmp.h>

#include "rt.h"

#define STACK_BASE 0x7200000000ull
#define STACK_SIZE 0x0040000000ull

typedef enum { KOBJ_THREAD = 1, KOBJ_EVENT, KOBJ_SESSION, KOBJ_SHMEM, KOBJ_TMEM } KType;
typedef enum { WAIT_NONE = 0, WAIT_MUTEX, WAIT_CONDVAR, WAIT_ARBITER } WaitKind;

typedef struct KObject {
    KType type;
    int refs;
    bool signaled;
} KObject;

typedef KObject KEvent;

struct KThread {
    KObject obj;  // signaled when the thread exits
    Cpu cpu;
    pthread_t host;
    uint64_t entry, tlr, id;
    uint32_t handle;
    int prio, core;
    bool cancel, in_sync_wait;
    // wait state (protected by g_klock)
    WaitKind wait_kind;
    uint64_t wait_addr, wait_mutex;
    uint32_t wait_tag, wait_result;
    KThread *wnext;
    KThread *snext;        // in WaitSynchronization (g_syncwaiters)
    pthread_cond_t cond;   // each thread sleeps on its own condition; wakers signal it directly
    // in-place reboot (kernel_stop_threads): thread_main's setjmp point, where the thread unwinds to and ends
    jmp_buf unwind;
    bool started, finished;
    KThread *anext;        // every thread of this boot (g_klock)
    // [XCDE] T029 deterministic scheduler (kernel.c): 0 not scheduled, 1 runnable, 2 holds the baton, 3 blocked
    int det_state, det_svcs;
    uint64_t det_seq;
    bool det_granted, det_timed_out;
    int64_t det_deadline;  // virtual ns, -1 none
    uint32_t last_svc;     // [XCDE] watchdog: last SVC number, its caller (lr) and when (host ns)
    uint64_t last_lr, last_svc_ns;
};

typedef struct KSharedMem {
    KObject obj;
    uint64_t size, guest_addr;  // guest_addr: where the guest mapped it (0 if not mapped)
    void (*on_map)(struct KSharedMem *s);  // HLE owner fills initial contents
    void *owner;
} KSharedMem;

KSharedMem *kernel_create_shared_memory(uint64_t size, void (*on_map)(KSharedMem *), uint32_t *handle);

typedef struct {
    KObject obj;
    uint64_t addr, size;
} KTransferMem;

extern pthread_mutex_t g_klock;

// [XCDE] T029: deterministic mode (XCDE_DETERMINISTIC=1). One guest thread runs at a time (the "baton"); it changes hands only
// where the running thread blocks, sleeps, yields, exits or has made many SVCs. Time is virtual: it advances only when every
// thread is blocked, jumping to the earliest timer / timeout. Host-driven events (vsync, audio buffers) are virtual timers.
bool kernel_det(void);
void kernel_det_mutex_lock(pthread_mutex_t *m);  // HLE mutexes held across guest calls (T122)
uint64_t kernel_vnow(int tick);  // virtual ns; tick != 0 advances it by 1 us (a query inside a busy period)
void kernel_timer_cancel(void *arg);  // drop the pending timers of `arg` (caller holds g_klock)
void kernel_timer_at(uint64_t when_vns, void (*fn)(void *), void *arg);  // caller holds g_klock; fn runs with g_klock held
extern KThread *g_waitlist;

KObject *handle_get(uint32_t h);
uint32_t kernel_new_handle(KObject *o);
void kernel_signal(KObject *o);  // caller holds g_klock
KEvent *kernel_create_event_pair(uint32_t *writable, uint32_t *readable);
KThread *kernel_current_thread(void);
void kernel_dump_threads(FILE *out);  // [XCDE] watchdog: one line per thread (state, last SVC, wait address)
uint64_t kernel_heap_size(void);  // bytes from HEAP_BASE set by SetHeapSize
