// [XCDE] LINUX-003: synchronous stackful contexts for the Linux x86-64 fiber backend.
#pragma once
#if !defined(__linux__) || !defined(__x86_64__)
#error This backend is only validated for Linux x86-64.
#endif
#include <stdbool.h>
#include <stddef.h>
#include <ucontext.h>

typedef struct FiberContext {
    ucontext_t context;
    void *mapping;
    size_t mapping_size;
    void (*entry)(void *);
    void *argument;
} FiberContext;

// A root context may be zero-initialized; its first switch saves its native stack.
bool fiber_context_create(FiberContext *context, size_t stack_size, void (*entry)(void *), void *argument);
void fiber_context_destroy(FiberContext *context);
// Caller owns both contexts until switching is complete; switching is a synchronous call boundary.
// The destination keeps its FP environment, but adopts the current host thread's signal mask.
int fiber_context_switch(FiberContext *from, FiberContext *to);
