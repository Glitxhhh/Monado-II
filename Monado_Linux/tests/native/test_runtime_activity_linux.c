// [XCDE] LINUX-018: actual runtime lease, independent authored processes only.
#define _GNU_SOURCE
#include <dirent.h>
#include <limits.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/wait.h>
#include "../../runtime/platform/linux_activity.h"
#include "../../runtime/platform/linux_state_guard.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %s:%d: %s (errno %d)\n", __FILE__, __LINE__, #x, errno); exit(1); } } while (0)
static char self[PATH_MAX];
static int descriptors(void) {
    DIR *dir = opendir("/proc/self/fd"); CHECK(dir); int n = 0;
    while (readdir(dir)) n++;
    CHECK(!closedir(dir)); return n;
}
static void reap(pid_t pid, int expected, bool signaled) {
    int status; while (waitpid(pid, &status, 0) < 0) CHECK(errno == EINTR);
    CHECK(signaled ? WIFSIGNALED(status) && WTERMSIG(status) == expected
                   : WIFEXITED(status) && WEXITSTATUS(status) == expected);
}
static void probe(const char *root, bool expected) {
    pid_t pid = fork(); CHECK(pid >= 0);
    if (!pid) { execl(self, self, "--probe", root, expected ? "1" : "0", NULL); _exit(127); }
    reap(pid, 0, false);
}
static pid_t holder(const char *root, bool exec_after_acquire) {
    int ready[2]; CHECK(!pipe(ready));
    pid_t pid = fork(); CHECK(pid >= 0);
    if (!pid) {
        close(ready[0]); char descriptor[32]; snprintf(descriptor, sizeof descriptor, "%d", ready[1]);
        execl(self, self, exec_after_acquire ? "--exec-holder" : "--holder", root, descriptor, NULL); _exit(127);
    }
    close(ready[1]); char byte = 0; CHECK(read(ready[0], &byte, 1) == 1 && byte == 'R'); close(ready[0]);
    return pid;
}
static void write_file(const char *path, const char *text) {
    FILE *file = fopen(path, "wx"); CHECK(file); CHECK(fputs(text, file) >= 0); CHECK(!fclose(file));
}
int main(int argc, char **argv) {
    CHECK(argc == 3 || argc == 4); CHECK(realpath(argv[0], self));
    const char *mode = argv[1], *root = argv[2]; char error[512];
    if (!strcmp(mode, "--exec-probe")) {
        CHECK(argc == 4);
        int descriptor = atoi(root), ready = atoi(argv[3]);
        CHECK(fcntl(descriptor, F_GETFD) == -1 && errno == EBADF);
        CHECK(write(ready, "R", 1) == 1); close(ready); usleep(200000); return 0;
    }
    CHECK(root[0] == '/');
    CHECK(rt_linux_state_guard(root, error, sizeof error));
    if (!strcmp(mode, "--probe")) {
        CHECK(argc == 4); bool acquired = rt_linux_activity_acquire(root, error, sizeof error);
        CHECK(acquired == (atoi(argv[3]) != 0));
        if (!acquired) CHECK(strstr(error, "another game or shader precompiler"));
        return 0;
    }
    if (!strcmp(mode, "--holder") || !strcmp(mode, "--exec-holder")) {
        CHECK(argc == 4); CHECK(rt_linux_activity_acquire(root, error, sizeof error));
        int ready = atoi(argv[3]);
        if (!strcmp(mode, "--exec-holder")) {
            char descriptor[32]; snprintf(descriptor, sizeof descriptor, "%d", rt_linux_activity_fd);
            execl(self, self, "--exec-probe", descriptor, argv[3], NULL); _exit(127);
        }
        CHECK(write(ready, "R", 1) == 1); close(ready); for (;;) pause();
    }
    int before = descriptors(); char lock[PATH_MAX], original[PATH_MAX];
    CHECK(snprintf(lock, sizeof lock, "%s/.runtime.lock", root) < (int)sizeof lock);
    CHECK(snprintf(original, sizeof original, "%s/preserved.txt", root) < (int)sizeof original);
    if (!strcmp(mode, "busy-release")) {
        CHECK(rt_linux_activity_acquire(root, error, sizeof error));
        int owned = rt_linux_activity_fd; CHECK(fcntl(owned, F_GETFD) & FD_CLOEXEC);
        CHECK(rt_linux_activity_acquire(root, error, sizeof error) && rt_linux_activity_fd == owned);
        probe(root, false);
        CHECK(!close(rt_linux_activity_fd)); rt_linux_activity_fd = -1;
        probe(root, true);
    } else if (!strcmp(mode, "owner-death")) {
        pid_t pid = holder(root, false); probe(root, false);
        CHECK(!kill(pid, SIGKILL)); reap(pid, SIGKILL, true); probe(root, true);
    } else if (!strcmp(mode, "close-on-exec")) {
        pid_t pid = holder(root, true); probe(root, true); reap(pid, 0, false);
    } else if (!strcmp(mode, "symlink") || !strcmp(mode, "hardlink")) {
        write_file(original, "preserve authored bytes");
        CHECK(!strcmp(mode, "symlink") ? !symlink(original, lock) : !link(original, lock));
        CHECK(!rt_linux_activity_acquire(root, error, sizeof error));
        FILE *file = fopen(original, "r"); CHECK(file); char bytes[64] = {0};
        CHECK(fread(bytes, 1, sizeof bytes - 1, file) == strlen("preserve authored bytes")); CHECK(!fclose(file));
        CHECK(!strcmp(bytes, "preserve authored bytes"));
    } else if (!strcmp(mode, "directory")) {
        CHECK(!mkdir(lock, 0700)); CHECK(!rt_linux_activity_acquire(root, error, sizeof error));
    } else if (!strcmp(mode, "long-path")) {
        char long_root[4100]; memset(long_root, 'x', sizeof long_root - 1); long_root[sizeof long_root - 1] = 0;
        CHECK(!rt_linux_activity_acquire(long_root, error, sizeof error)); CHECK(strstr(error, "too long"));
    } else { fprintf(stderr, "unknown activity case\n"); return 2; }
    CHECK(descriptors() == before);
    printf("ACTIVITY %s PASS: actual lease/guard, no game\n", mode);
    return 0;
}
