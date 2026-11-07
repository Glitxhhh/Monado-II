// [XCDE] LINUX-003: synthetic startup/re-exec fixture, never a game execution.
// Compile only main_boot with plat_{reserve_guest,init,exec} renamed to these
// test adapters, then link the real Linux platform. The adapters simulate
// reservation/exec failures and avoid user logging; real plat_exec preserves
// PID, argv and environment. All guest-entry callbacks fail if reached.
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../../runtime/kernel.h"
#include "../../runtime/diag/diag.h"
#include "../../runtime/paths.h"
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "fixture failure: %s\n", #c); abort(); } } while (0)

bool test_plat_reserve_guest(uint64_t lo, uint64_t hi) {
    CHECK(lo == GUEST_LO && hi == GUEST_HI);
    const char *retry = getenv("XCDE_RELAUNCH");
    int attempt = retry ? atoi(retry) : 0;
    FILE *f = fopen(getenv("TEST_BOOT_LOG"), "a");
    CHECK(f != NULL);
    fprintf(f, "attempt %d pid %ld\n", attempt, (long)getpid());
    CHECK(fclose(f) == 0);
    return attempt >= atoi(getenv("TEST_RESERVE_FAILURES"));
}
void test_plat_init(void) {
    // Most cases isolate startup semantics; one explicitly exercises real
    // platform logging after the actual path setup and write guard.
    if (getenv("TEST_REAL_PLAT_INIT")) plat_init();
}
void test_plat_exec(char *const argv[]) {
    if (getenv("TEST_EXEC_FAILURE")) { errno = ENOENT; return; }
    plat_exec(argv);
}
int vk_precompile_main(const char *root) {
    const char *expected = getenv("TEST_BOOT_EXPECTED_INPUT");
    const char *state = getenv("TEST_BOOT_EXPECTED_STATE");
    CHECK(expected && state);
    CHECK(!strcmp(root, expected));
    CHECK(!strcmp(getenv("TEST_BOOT_SENTINEL"), "environment preserved"));
    CHECK(!strcmp(rt_input_root(), expected));
    CHECK(!strcmp(rt_data_root(), state));
    char path[PATH_MAX], cwd[PATH_MAX];
    CHECK(getcwd(cwd, sizeof cwd) && !strcmp(cwd, state));
    FILE *out = fopen("cache/synthetic-precompile", "wb");
    CHECK(out != NULL); CHECK(fputs("synthetic cache\n", out) >= 0); CHECK(fclose(out) == 0);
    CHECK(snprintf(path, sizeof path, "%s/romfs/original", root) < (int)sizeof path);
    int fd = open(path, O_RDONLY); CHECK(fd >= 0);
    char bytes[16] = {0}; CHECK(read(fd, bytes, sizeof bytes) == 8); CHECK(close(fd) == 0);
    CHECK(!strcmp(bytes, "sentinel"));
    errno = 0; fd = open(path, O_WRONLY | O_TRUNC);
    CHECK(fd < 0 && (errno == EACCES || errno == EPERM));
    puts("synthetic precompile paths/guard verified; argv/environment preserved");
    return 37;
}
int diag_minidump_helper(int n, char **a) { (void)n; (void)a; return 2; }
void diag_watchdog_start(void) { CHECK(getenv("TEST_BOOT_WATCHDOG") != NULL); }
void kernel_dump_threads(FILE *f) { (void)f; abort(); }
void loader_load(const char *p) { (void)p; fputs("fixture unexpectedly reached loader\n", stderr); abort(); }
uint64_t loader_find_symbol(const char *s) { (void)s; abort(); }
void kernel_init(void) { abort(); }
void dispatch_init(void) { abort(); }
void guest_call(Cpu *c, uint64_t a) { (void)c; (void)a; abort(); }
uint64_t hle_register(const char *s, HostFn f) { (void)s; (void)f; abort(); }
KThread *kernel_create_main_thread(uint64_t a, uint64_t n, int p) { (void)a; (void)n; (void)p; abort(); }
void kernel_run_main(KThread *t, uint64_t a, uint64_t b, uint64_t c, uint64_t d) { (void)t; (void)a; (void)b; (void)c; (void)d; abort(); }
int ui_run(void) { abort(); }
void rt_fatal_trace(void) { abort(); }
const char *rt_host_fn_name(uint64_t p, uint64_t *o) { (void)p; (void)o; return NULL; }
bool rt_fault_watch(uint64_t a, uint64_t p) { (void)a; (void)p; return false; }
void rt_fault_report(const char *s, uint64_t a, uint64_t p, uint64_t l) { (void)s; (void)a; (void)p; (void)l; abort(); }
Module g_modules[4]; int g_nmodules;
