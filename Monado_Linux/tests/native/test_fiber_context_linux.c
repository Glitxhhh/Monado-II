// [XCDE] LINUX-003: game-free proof of context migration and native-thread state.
#define _GNU_SOURCE
#include <stdlib.h>
#include <fenv.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/resource.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../../runtime/hle/fiber_context_linux.h"

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); abort(); } } while (0)

static FiberContext fiber;
static __thread FiberContext *root;
static __thread unsigned sentinel;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static int phase;
__attribute__((noinline)) static unsigned tls_value(void) { __asm__ volatile("" ::: "memory"); return sentinel; }
__attribute__((noinline)) static FiberContext *current_root(void) { __asm__ volatile("" ::: "memory"); return root; }
static void mask_check(int blocked, int unblocked) {
    sigset_t mask;
    CHECK(pthread_sigmask(SIG_SETMASK, NULL, &mask) == 0);
    CHECK(sigismember(&mask, blocked) == 1);
    CHECK(sigismember(&mask, unblocked) == 0);
}
static void run_fiber(void *p) {
    CHECK(p == &fiber);
    volatile uint64_t stack_words[128];
    for (unsigned i = 0; i < 128; ++i) stack_words[i] = 0x123456789abcdef0ull ^ i;
    CHECK(tls_value() == 111);
    mask_check(SIGUSR1, SIGUSR2);
    CHECK(fesetround(FE_UPWARD) == 0);
    CHECK(feclearexcept(FE_ALL_EXCEPT) == 0); CHECK(feraiseexcept(FE_INEXACT) == 0);
    CHECK(fiber_context_switch(&fiber, current_root()) == 0);
    CHECK(tls_value() == 222);
    mask_check(SIGUSR2, SIGUSR1);
    CHECK(fegetround() == FE_UPWARD);
    CHECK((fetestexcept(FE_ALL_EXCEPT) & FE_INEXACT) != 0);
    for (unsigned i = 0; i < 128; ++i) CHECK(stack_words[i] == (0x123456789abcdef0ull ^ i));
    CHECK(fiber_context_switch(&fiber, current_root()) == 0);
    CHECK(!"unexpected third resume");
}
static void *thread_main(void *p) {
    uintptr_t id = (uintptr_t)p;
    FiberContext native = {0}; root = &native; sentinel = id == 1 ? 111 : 222;
    sigset_t mask; sigemptyset(&mask); sigaddset(&mask, id == 1 ? SIGUSR1 : SIGUSR2);
    CHECK(pthread_sigmask(SIG_SETMASK, &mask, NULL) == 0);
    int rounding = id == 1 ? FE_DOWNWARD : FE_TOWARDZERO;
    CHECK(fesetround(rounding) == 0);
    CHECK(feclearexcept(FE_ALL_EXCEPT) == 0); CHECK(feraiseexcept(FE_DIVBYZERO) == 0);
    if (id == 2) {
        pthread_mutex_lock(&mutex);
        while (phase != 1) pthread_cond_wait(&cond, &mutex);
        pthread_mutex_unlock(&mutex);
    }
    CHECK(fiber_context_switch(&native, &fiber) == 0);
    CHECK(fegetround() == rounding);
    CHECK((fetestexcept(FE_ALL_EXCEPT) & FE_DIVBYZERO) != 0);
    CHECK(tls_value() == (id == 1 ? 111u : 222u));
    if (id == 1) {
        pthread_mutex_lock(&mutex); phase = 1; pthread_cond_signal(&cond);
        while (phase != 2) pthread_cond_wait(&cond, &mutex);
        pthread_mutex_unlock(&mutex);
    } else {
        pthread_mutex_lock(&mutex); phase = 2; pthread_cond_signal(&cond); pthread_mutex_unlock(&mutex);
    }
    return NULL;
}
static void guard_entry(void *p) { volatile unsigned char *guard = p; *guard = 1; _exit(99); }
int main(void) {
    CHECK(fiber_context_create(&fiber, 64u << 20, run_fiber, &fiber));
    pthread_t a, b;
    CHECK(pthread_create(&a, NULL, thread_main, (void *)1) == 0);
    CHECK(pthread_create(&b, NULL, thread_main, (void *)2) == 0);
    CHECK(pthread_join(a, NULL) == 0); CHECK(pthread_join(b, NULL) == 0);
    void *old_mapping = fiber.mapping;
    unsigned char residency;
    CHECK(mincore(old_mapping, (size_t)sysconf(_SC_PAGESIZE), &residency) == 0);
    fiber_context_destroy(&fiber);
    CHECK(!fiber.mapping && !fiber.mapping_size);
    CHECK(mincore(old_mapping, (size_t)sysconf(_SC_PAGESIZE), &residency) == -1 && errno == ENOMEM);
    FiberContext invalid = {0};
    CHECK(!fiber_context_create(&invalid, 0, run_fiber, NULL));
    CHECK(!fiber_context_create(&invalid, SIZE_MAX, run_fiber, NULL));
    for (int upper = 0; upper < 2; ++upper) {
        pid_t child = fork(); CHECK(child >= 0);
        if (!child) {
            struct rlimit limit = {0, 0}; setrlimit(RLIMIT_CORE, &limit);
            FiberContext native = {0}, guard = {0};
            CHECK(fiber_context_create(&guard, 1u << 20, guard_entry, NULL));
            guard.argument = upper ? (char *)guard.mapping + guard.mapping_size - 1 : guard.mapping;
            fiber_context_switch(&native, &guard); _exit(98);
        }
        int status; CHECK(waitpid(child, &status, 0) == child);
        CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV);
    }
    pid_t child = fork(); CHECK(child >= 0);
    if (!child) {
        struct rlimit limit = {4096, 4096};
        CHECK(setrlimit(RLIMIT_AS, &limit) == 0);
        FiberContext allocation = {0};
        CHECK(!fiber_context_create(&allocation, 64u << 20, run_fiber, NULL));
        _exit(0);
    }
    int status; CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    puts("fiber context: migration, stack/TLS/FP/signal state and guard page OK");
    return 0;
}
