// [XCDE] LINUX-003: deterministic diagnostic-worker test, no game or renderer.
// Fake only the clock/frame/report inputs; execute the real watchdog pthread.
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "../../runtime/diag/diag.h"

static atomic_uint ticks, kernel_reports, backend_reports;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ended = PTHREAD_COND_INITIALIZER;
static int done;
static unsigned creates;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); abort(); } } while (0)

uint64_t plat_now_ns(void) { return (uint64_t)atomic_load(&ticks) * 1000000000ull; }
void plat_sleep_until_ns(uint64_t when) {
    CHECK(when == plat_now_ns() + 1000000000ull);
    unsigned n = atomic_fetch_add(&ticks, 1) + 1;
    if (n == 8) {
        pthread_mutex_lock(&lock);
        done = 1;
        pthread_cond_signal(&ended);
        pthread_mutex_unlock(&lock);
        pthread_exit(NULL);
    }
}
uint64_t diag_frame(void) {
    // Loading (zero), first frame, a stall, progress, and another stall.
    static const uint64_t frames[] = {0, 0, 1, 1, 1, 2, 2, 2};
    unsigned n = atomic_load(&ticks);
    CHECK(n < sizeof frames / sizeof frames[0]);
    return frames[n];
}
void kernel_dump_threads(FILE *f) { atomic_fetch_add(&kernel_reports, 1); fputs("kernel sentinel\n", f); }
void backend_stall_report(FILE *f) { atomic_fetch_add(&backend_reports, 1); fputs("renderer sentinel\n", f); }
void rt_fatal_trace(void) { abort(); }
// Rename pthread_create only in the watchdog object, making disabled coverage
// deterministic without waiting for a mistakenly-created worker to be scheduled.
int test_pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*entry)(void *), void *arg) {
    creates++;
    return pthread_create(thread, attr, entry, arg);
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "invalid")) {
        diag_watchdog_start();
        fputs("invalid watchdog interval was accepted\n", stderr);
        return 1;
    }
    if (argc == 2 && !strcmp(argv[1], "disabled")) {
        CHECK(setenv("XCDE_STALL", "0", 1) == 0);
        diag_watchdog_start();
        CHECK(creates == 0);
        CHECK(atomic_load(&ticks) == 0);
        puts("watchdog disabled: OK");
        return 0;
    }
    CHECK(setenv("XCDE_STALL", "2", 1) == 0);
    diag_watchdog_start();
    CHECK(creates == 1);
    pthread_mutex_lock(&lock);
    struct timespec until;
    CHECK(clock_gettime(CLOCK_REALTIME, &until) == 0);
    until.tv_sec += 5;
    while (!done) CHECK(pthread_cond_timedwait(&ended, &lock, &until) == 0);
    pthread_mutex_unlock(&lock);
    CHECK(atomic_load(&kernel_reports) == 4);
    CHECK(atomic_load(&backend_reports) == 4);
    for (unsigned n = 1; n <= 2; ++n) {
        char path[64], text[512] = {0};
        snprintf(path, sizeof path, "crash/stall_%u.txt", n);
        FILE *f = fopen(path, "r");
        CHECK(f != NULL);
        CHECK(fread(text, 1, sizeof text - 1, f) > 0);
        CHECK(fclose(f) == 0);
        CHECK(strstr(text, "no frame for 2 s") != NULL);
        CHECK(strstr(text, "kernel sentinel") != NULL);
        CHECK(strstr(text, "renderer sentinel") != NULL);
    }
    CHECK(access("crash/stall_3.txt", F_OK) != 0);
    puts("watchdog loading/progress/stall reports: OK");
    return 0;
}
