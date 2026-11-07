// Stall watchdog (T027, FR-061): when no frame has been presented for XCDE_STALL seconds (default 5; 0 disables) it writes a
// complete report -- every guest thread (state, last SVC and the guest function that made it, mutex owners, deadlock notes)
// -- to stderr and to crash/stall_<n>.txt, and repeats every <s> seconds while the stall lasts. It takes no locks it could
// block on, so it also works when the hang is inside the kernel lock.
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#else
// [XCDE] LINUX-003: process-lifetime pthread watchdog, with monotonic sleep.
#include <pthread.h>
#include <sys/stat.h>
#include <errno.h>
#include <limits.h>
#include <string.h>
#endif

#include "../kernel.h"
#include "../rt.h"
#include "diag.h"

#ifdef _WIN32
static DWORD WINAPI stall_main(LPVOID p) {
#else
static void *stall_main(void *p) {
#endif
    unsigned secs = (unsigned)(uintptr_t)p, still = 0, reports = 0;
    uint64_t last = 0;
    for (;;) {
#ifdef _WIN32
        Sleep(1000);
#else
        plat_sleep_until_ns(plat_now_ns() + 1000000000ull);
#endif
        uint64_t n = diag_frame();
        if (n != last || n == 0) { last = n; still = 0; continue; }  // (loading before the first frame is not a stall)
        if (++still < secs || (still - secs) % secs) continue;
        LOG("[stall] no frame for %u s (last frame %llu): report %u\n", still, (unsigned long long)n, ++reports);
        kernel_dump_threads(stderr);
        extern void backend_stall_report(FILE *f);  // [XCDE] T240: the render worker too
        backend_stall_report(stderr);
#ifdef _WIN32
        CreateDirectoryA("crash", NULL);
#else
        if (mkdir("crash", 0755) != 0 && errno != EEXIST)
            LOG("[stall] cannot create report directory: %s\n", strerror(errno));
#endif
        char path[64];
        snprintf(path, sizeof path, "crash/stall_%u.txt", reports);
        FILE *f = fopen(path, "w");
        if (f) {
            fprintf(f, "no frame for %u s (last frame %llu)\n", still, (unsigned long long)n);
            kernel_dump_threads(f);
            backend_stall_report(f);
            fclose(f);
        }
    }
    return 0;
}

void diag_watchdog_start(void) {
    const char *e = getenv("XCDE_STALL");
#ifdef _WIN32
    unsigned secs = e ? (unsigned)atoi(e) : 5;
#else
    unsigned secs = 5;
    if (e) {
        char *end;
        errno = 0;
        unsigned long parsed = strtoul(e, &end, 10);
        if (*e < '0' || *e > '9' || *end || errno || parsed > UINT_MAX)
            FATAL("XCDE_STALL must specify an interval in seconds (0..%u)\n", UINT_MAX);
        secs = (unsigned)parsed;
    }
#endif
    if (secs) {
#ifdef _WIN32
        CreateThread(NULL, 0, stall_main, (LPVOID)(uintptr_t)secs, 0, NULL);
#else
        pthread_t thread;
        int rc = pthread_create(&thread, NULL, stall_main, (void *)(uintptr_t)secs);
        if (rc) FATAL("cannot start stall watchdog: %s\n", strerror(rc));
        rc = pthread_detach(thread);
        if (rc) FATAL("cannot detach stall watchdog: %s\n", strerror(rc));
#endif
    }
}
