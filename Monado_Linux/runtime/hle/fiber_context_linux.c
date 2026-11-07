// [XCDE] LINUX-003: glibc synchronous contexts; guarded, lazily backed native stacks.
// swapcontext is a C call boundary: caller-saved vector registers are spilled by
// the compiler, while the guest's entire register file remains in Cpu. The libc
// context also preserves the host floating-point environment. No TLS base is
// moved: resumed code must reread current-thread state through noinline accessors.
#define _GNU_SOURCE
#include "fiber_context_linux.h"
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static void context_entry(unsigned low, unsigned high) {
    uintptr_t address = (uintptr_t)low | ((uintptr_t)high << 32);
    FiberContext *context = (FiberContext *)address;
    context->entry(context->argument);
    abort();  // a fiber entry must explicitly switch back, never follow stale uc_link
}

bool fiber_context_create(FiberContext *context, size_t stack_size, void (*entry)(void *), void *argument) {
    long page = sysconf(_SC_PAGESIZE);
    if (page <= 0 || !entry || stack_size < (size_t)page ||
        stack_size > SIZE_MAX - 3 * (size_t)page) { errno = EINVAL; return false; }
    size_t size = (stack_size + (size_t)page - 1) / (size_t)page * (size_t)page;
    size_t total = size + 2 * (size_t)page;
    void *mapping = mmap(NULL, total, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_STACK, -1, 0);
    if (mapping == MAP_FAILED) return false;
    void *stack = (char *)mapping + page;
    if (mprotect(stack, size, PROT_READ | PROT_WRITE) != 0) {
        int error = errno; munmap(mapping, total); errno = error; return false;
    }
    memset(context, 0, sizeof *context);
    if (getcontext(&context->context) != 0) {
        int error = errno; munmap(mapping, total); errno = error; return false;
    }
    context->mapping = mapping;
    context->mapping_size = total;
    context->entry = entry;
    context->argument = argument;
    context->context.uc_stack.ss_sp = stack;
    context->context.uc_stack.ss_size = size;
    context->context.uc_link = NULL;
    uintptr_t pointer = (uintptr_t)context;
    makecontext(&context->context, (void (*)(void))context_entry, 2,
                (unsigned)(uint32_t)pointer, (unsigned)(uint32_t)(pointer >> 32));
    return true;
}

void fiber_context_destroy(FiberContext *context) {
    if (context->mapping) munmap(context->mapping, context->mapping_size);
    memset(context, 0, sizeof *context);
}

int fiber_context_switch(FiberContext *from, FiberContext *to) {
    // Signal disposition belongs to the host thread. A migrated fiber must not
    // restore the source pthread's blocked-signal mask on its destination.
    int error = pthread_sigmask(SIG_SETMASK, NULL, &to->context.uc_sigmask);
    if (error) { errno = error; return -1; }
    return swapcontext(&from->context, &to->context);
}
