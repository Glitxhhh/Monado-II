// [XCDE] LINUX-003: synthetic Linux host-platform contracts; no game input or plat_init.
#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../../runtime/platform.h"

// Synthetic diagnostic callbacks are the only runtime substitutions. Real SDL is linked.
const char *rt_host_fn_name(uint64_t pc, uint64_t *off) { (void)pc; (void)off; return NULL; }
bool rt_fault_watch(uint64_t addr, uint64_t pc) { (void)addr; (void)pc; return false; }
static __thread stack_t expected_signal_stack;
static __thread int expected_signal;
void rt_fault_report(const char *what, uint64_t addr, uint64_t pc, uint64_t lr) {
    (void)addr;
    char marker;
    uintptr_t p = (uintptr_t)&marker, lo = (uintptr_t)expected_signal_stack.ss_sp;
    int signal_number = 0;
    if (sscanf(what, "signal %d", &signal_number) != 1 || signal_number != expected_signal ||
        !pc || p < lo || p >= lo + expected_signal_stack.ss_size || lr != 0) _exit(91);
    _exit(77);
}

// A weak reference lets the inherited implementation expose its missing cleanup contract.
extern void plat_crash_handler_uninstall(void) __attribute__((weak));

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s (errno=%d)\n", __FILE__, __LINE__, #c, errno); failures++; } } while (0)

static int child_status(pid_t child) {
    int status = 0;
    pid_t r;
    do { r = waitpid(child, &status, 0); } while (r < 0 && errno == EINTR);
    CHECK(r == child);
    return status;
}

static void test_memory(void) {
    size_t page = (size_t)sysconf(_SC_PAGESIZE), size = page * 3;
    unsigned char *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(p != MAP_FAILED);
    if (p == MAP_FAILED) return;
    p[0] = 0x5a;
    CHECK(!plat_reserve_guest((uintptr_t)p, (uintptr_t)p + size));
    CHECK(p[0] == 0x5a);  // MAP_FIXED_NOREPLACE must not destroy someone else's mapping.
    CHECK(munmap(p, size) == 0);
    CHECK(plat_reserve_guest((uintptr_t)p, (uintptr_t)p + size));
    uint64_t priv, rss, resident;
    plat_mem_info(&priv, &rss, &resident);
    CHECK(priv > 0 && rss >= priv && resident == 0);
    p[0] = 0x43; p[size - 1] = 0xa3;
    plat_mem_info(&priv, &rss, &resident);
    CHECK(priv >= page * 2 && rss >= priv && resident == page * 2);
    plat_mem_map();
    CHECK(plat_reset_guest((uintptr_t)p, (uintptr_t)p + size));
    plat_mem_info(&priv, &rss, &resident);
    CHECK(resident == 0);  // observing residency must not fault pages into RAM
    CHECK(p[0] == 0 && p[size - 1] == 0);
    CHECK(plat_protect((uintptr_t)p, page, PLAT_PROT_READ) == 0);
    pid_t child = fork();
    if (!child) { p[0] = 1; _exit(0); }
    CHECK(child > 0);
    if (child > 0) { int status = child_status(child); CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV); }
    CHECK(plat_protect((uintptr_t)p, page, PLAT_PROT_RW) == 0);
    p[0] = 0x11;
    CHECK(plat_protect((uintptr_t)p + page, page, PLAT_PROT_NONE) == 0);
    child = fork();
    if (!child) { volatile unsigned char value = p[page]; (void)value; _exit(0); }
    CHECK(child > 0);
    if (child > 0) { int status = child_status(child); CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV); }
    CHECK(munmap(p, size) == 0);
}

static volatile sig_atomic_t signal_hits;
static void interrupt_handler(int sig) { (void)sig; signal_hits++; }
static void *interrupt_thread(void *p) {
    pthread_t target = *(pthread_t *)p;
    usleep(15000);
    pthread_kill(target, SIGUSR1);
    usleep(15000);
    pthread_kill(target, SIGUSR1);
    return NULL;
}
static void test_time(void) {
    struct sigaction action = {.sa_handler = interrupt_handler};
    sigemptyset(&action.sa_mask);
    CHECK(sigaction(SIGUSR1, &action, NULL) == 0);  // deliberately no SA_RESTART
    uint64_t start = plat_now_ns(), end = start + 100000000;
    pthread_t sender, target = pthread_self();
    CHECK(pthread_create(&sender, NULL, interrupt_thread, &target) == 0);
    plat_sleep_until_ns(end);
    CHECK(plat_now_ns() >= end);
    CHECK(pthread_join(sender, NULL) == 0);
    CHECK(signal_hits > 0);
    plat_sleep_until_ns(start);  // an elapsed deadline must also return
}

static void test_executable_path(void) {
    char full[PATH_MAX], actual[PATH_MAX], small[2] = {'x', 'y'}, zero = 'x';
    CHECK(plat_exe_path(full, sizeof full));
    ssize_t n = readlink("/proc/self/exe", actual, sizeof actual - 1);
    CHECK(n > 0);
    if (n <= 0) return;
    actual[n] = 0;
    CHECK(!strcmp(full, actual));
    CHECK(!plat_exe_path(small, sizeof small));
    CHECK(!plat_exe_path(&zero, 0) && zero == 'x');
    char *exact = malloc((size_t)n + 1);
    CHECK(exact != NULL);
    if (exact) { CHECK(plat_exe_path(exact, (size_t)n + 1)); CHECK(!strcmp(exact, actual)); free(exact); }
}

static void test_affinity_and_name(void) {
    cpu_set_t saved, one;
    CHECK(sched_getaffinity(0, sizeof saved, &saved) == 0);
    CPU_ZERO(&one);
    for (int i = 0; i < CPU_SETSIZE; i++) if (CPU_ISSET(i, &saved)) { CPU_SET(i, &one); break; }
    CHECK(sched_setaffinity(0, sizeof one, &one) == 0);
    CHECK(plat_cpu_count() == 1);
    CHECK(sched_setaffinity(0, sizeof saved, &saved) == 0);
    char before[16], after[16];
    CHECK(pthread_getname_np(pthread_self(), before, sizeof before) == 0);
    plat_thread_name("monado-platform-name");
    CHECK(pthread_getname_np(pthread_self(), after, sizeof after) == 0);
    CHECK(!strcmp(after, "monado-platform"));
    CHECK(pthread_setname_np(pthread_self(), before) == 0);
}

static void test_processes(const char *exe) {
    const char *args[] = {exe, "--child-exit", NULL};
    PlatProc p = plat_spawn(args, "child.log", false);
    CHECK(p > 0);
    if (p > 0) CHECK(plat_proc_wait(p, true) == 37);
    p = plat_spawn(args, "child.log", true);
    CHECK(p > 0);
    if (p > 0) CHECK(plat_proc_wait(p, true) == 37);
    char output[128] = {0};
    FILE *f = fopen("child.log", "rb");
    CHECK(f != NULL);
    if (f) { size_t n = fread(output, 1, sizeof output - 1, f); output[n] = 0; fclose(f); }
    CHECK(!strcmp(output, "out\nerr\nout\nerr\n"));
    const char *absent[] = {"/definitely-missing-monado-test-program", NULL};
    CHECK(plat_spawn(absent, NULL, false) == 0);
    CHECK(plat_spawn(args, "missing-directory/child.log", false) == 0);

    const char *waiting[] = {exe, "--child-wait", NULL};
    p = plat_spawn(waiting, NULL, false);
    CHECK(p > 0);
    if (p > 0) {
        CHECK(plat_proc_wait(p, false) == -1);
        pthread_t sender, target = pthread_self();
        CHECK(pthread_create(&sender, NULL, interrupt_thread, &target) == 0);
        int result = plat_proc_wait(p, true);
        CHECK(result == 37);
        CHECK(pthread_join(sender, NULL) == 0);
        if (result != 37) (void)child_status((pid_t)p);  // keep the before-fix failure isolated and reaped
    }

    // Invalid zero handles must not kill our caller's process group: isolate the reproducer first.
    pid_t child = fork();
    if (!child) { if (setsid() < 0) _exit(92); plat_proc_kill(0); _exit(0); }
    CHECK(child > 0);
    if (child > 0) { int status = child_status(child); CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0); }

    // A zero wait handle must not reap an unrelated child in the same process group.
    child = fork();
    if (!child) _exit(42);
    CHECK(child > 0);
    if (child > 0) {
        CHECK(plat_proc_wait(0, true) == 255);
        int status = 0; pid_t r = waitpid(child, &status, 0);
        CHECK(r == child && WIFEXITED(status) && WEXITSTATUS(status) == 42);
    }
}

static void test_reexec(const char *exe) {
    char *large = malloc(40001);
    CHECK(large != NULL);
    if (!large) return;
    memset(large, 'q', 40000); large[40000] = 0;
    const char *args[] = {exe, "--reexec-long", large, NULL};
    PlatProc p = plat_spawn(args, "reexec-long.log", false);
    CHECK(p > 0);
    if (p > 0) CHECK(plat_proc_wait(p, true) == 0);
    free(large);
    const char *many[304] = {exe, "--reexec-many"};
    for (int i = 2; i < 303; i++) many[i] = "same argument with spaces";
    many[303] = NULL;
    p = plat_spawn(many, "reexec-many.log", false);
    CHECK(p > 0);
    if (p > 0) CHECK(plat_proc_wait(p, true) == 0);
}

static void test_process_group(const char *exe) {
    // Adopt the synthetic grandchild so the test can reap every process it creates.
    pid_t fixture = fork();
    if (!fixture) {
        if (prctl(PR_SET_CHILD_SUBREAPER, 1)) _exit(80);
        const char *args[] = {exe, "--child-tree", NULL};
        PlatProc p = plat_spawn(args, "process-tree.log", false);
        if (!p) _exit(81);
        long descendant = 0;
        uint64_t deadline = plat_now_ns() + 2000000000;
        while (plat_now_ns() < deadline) {
            FILE *f = fopen("process-tree.log", "r");
            if (f) { int n = fscanf(f, "%ld", &descendant); fclose(f); if (n == 1 && descendant > 0) break; }
            usleep(1000);
        }
        plat_proc_kill(p);
        if (descendant <= 0) _exit(82);
        int status = 0;
        pid_t r = 0;
        deadline = plat_now_ns() + 2000000000;
        while (plat_now_ns() < deadline) {
            r = waitpid((pid_t)descendant, &status, WNOHANG);
            if (r > 0 || (r < 0 && errno != EINTR)) break;
            usleep(1000);
        }
        if (!r) {  // clean up the owned descendant even if group termination regresses
            kill((pid_t)descendant, SIGKILL);
            while (waitpid((pid_t)descendant, &status, 0) < 0 && errno == EINTR) {}
            _exit(85);
        }
        if (r != descendant || !WIFSIGNALED(status) || WTERMSIG(status) != SIGKILL) _exit(83);
        if (waitpid((pid_t)p, &status, WNOHANG) != -1 || errno != ECHILD) _exit(84);
        _exit(0);
    }
    CHECK(fixture > 0);
    if (fixture > 0) { int status = child_status(fixture); CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0); }
}

static void *crash_thread(void *explicit_cleanup) {
    plat_crash_handler_install();
    stack_t first, second;
    if (sigaltstack(NULL, &first) || (first.ss_flags & SS_DISABLE)) return (void *)1;
    plat_crash_handler_install();
    if (sigaltstack(NULL, &second) || first.ss_sp != second.ss_sp) return (void *)2;
    if (explicit_cleanup) {
        plat_crash_handler_uninstall();
        if (sigaltstack(NULL, &second) || !(second.ss_flags & SS_DISABLE)) return (void *)3;
    }  // the other thread exercises the pthread destructor path
    return NULL;
}

static void test_crash_stack(void) {
    stack_t before, first, second;
    struct sigaction original_handler, after_cleanup;
    CHECK(sigaction(SIGILL, NULL, &original_handler) == 0);
    if (plat_crash_handler_uninstall) plat_crash_handler_uninstall();
    CHECK(sigaction(SIGILL, NULL, &after_cleanup) == 0);
    CHECK(after_cleanup.sa_handler == original_handler.sa_handler);
    CHECK(sigaltstack(NULL, &before) == 0);
    plat_crash_handler_install();
    CHECK(sigaltstack(NULL, &first) == 0 && !(first.ss_flags & SS_DISABLE));
    plat_crash_handler_install();
    CHECK(sigaltstack(NULL, &second) == 0 && first.ss_sp == second.ss_sp);
    CHECK(plat_crash_handler_uninstall != NULL);
    if (plat_crash_handler_uninstall) {
        plat_crash_handler_uninstall();
        CHECK(sigaltstack(NULL, &second) == 0);
        CHECK((second.ss_flags & SS_DISABLE) == (before.ss_flags & SS_DISABLE));
        if (!(before.ss_flags & SS_DISABLE)) CHECK(second.ss_sp == before.ss_sp);

        // Preserve an alternate stack belonging to a caller embedding the runtime.
        stack_t caller = {.ss_sp = malloc(SIGSTKSZ), .ss_size = SIGSTKSZ};
        CHECK(caller.ss_sp != NULL);
        if (caller.ss_sp) {
            CHECK(sigaltstack(&caller, NULL) == 0);
            plat_crash_handler_install();
            CHECK(sigaltstack(NULL, &second) == 0 && second.ss_sp != caller.ss_sp);
            plat_crash_handler_uninstall();
            CHECK(sigaltstack(NULL, &second) == 0 && second.ss_sp == caller.ss_sp && second.ss_size == caller.ss_size);
            CHECK(sigaltstack(&before, NULL) == 0);
            free(caller.ss_sp);
        }
        pthread_t threads[2];
        for (int i = 0; i < 2; i++) CHECK(pthread_create(&threads[i], NULL, crash_thread, (void *)(uintptr_t)i) == 0);
        for (int i = 0; i < 2; i++) { void *result = NULL; CHECK(pthread_join(threads[i], &result) == 0 && result == NULL); }
    }
    // SIGILL with a deliberately unusable host SP must report the original fault on the alt stack.
    pid_t child = fork();
    if (!child) {
        plat_crash_handler_install();
        sigaltstack(NULL, &expected_signal_stack);
        expected_signal = SIGILL;
#if defined(__x86_64__)
        __asm__ volatile("xor %%rsp, %%rsp; ud2" ::: "memory");
#else
        raise(SIGILL);
#endif
        _exit(93);
    }
    CHECK(child > 0);
    if (child > 0) { int status = child_status(child); CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 77); }
}

int main(int argc, char **argv) {
    struct rlimit no_core = {0, 0}; setrlimit(RLIMIT_CORE, &no_core);
    if (argc >= 2 && !strcmp(argv[1], "--child-exit")) {
        fputs("out\n", stdout); fflush(stdout); fputs("err\n", stderr); return 37;
    }
    if (argc >= 2 && !strcmp(argv[1], "--child-wait")) { usleep(120000); return 37; }
    if (argc >= 2 && !strcmp(argv[1], "--child-tree")) {
        pid_t child = fork();
        if (child < 0) return 79;
        if (child) { printf("%ld\n", (long)child); fflush(stdout); }
        for (;;) pause();
    }
    if (argc >= 2 && !strncmp(argv[1], "--reexec-", 9)) {
        const char *phase = getenv("MONADO_PLATFORM_REEXEC_PID");
        if (!phase) {
            char pid[32]; snprintf(pid, sizeof pid, "%ld", (long)getpid());
            if (setenv("MONADO_PLATFORM_REEXEC_PID", pid, 1)) return 94;
            plat_exec(NULL); perror("plat_exec"); return 95;
        }
        if (strtol(phase, NULL, 10) != getpid()) return 96;
        if (!strcmp(argv[1], "--reexec-long")) return argc == 3 && strlen(argv[2]) == 40000 ? 0 : 97;
        return argc == 303 && !strcmp(argv[302], "same argument with spaces") ? 0 : 98;
    }
    char exe[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0) return 99;
    exe[n] = 0;
    test_memory();
    test_time();
    test_executable_path();
    test_affinity_and_name();
    test_processes(exe);
    test_reexec(exe);
    test_process_group(exe);
    test_crash_stack();
    printf("platform Linux: %s (%d failures)\n", failures ? "FAIL" : "OK", failures);
    return failures != 0;
}
