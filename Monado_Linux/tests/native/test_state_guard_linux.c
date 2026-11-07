// [XCDE] LINUX-004: real filesystem enforcement against synthetic input only.
#define _GNU_SOURCE
#include "../../runtime/platform/linux_state_guard.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s (errno %d)\n", __LINE__, #x, errno); exit(1); } } while (0)
static char input[PATH_MAX];
static void denied_open(const char *path, int flags) {
    errno = 0;
    int fd = open(path, flags, 0600);
    if (fd >= 0) close(fd);
    CHECK(fd < 0 && (errno == EACCES || errno == EPERM));
}
static void *worker(void *unused) { (void)unused; denied_open(input, O_WRONLY); return NULL; }

int main(int argc, char **argv) {
    CHECK(argc == 4);
    char state[PATH_MAX], path[PATH_MAX], error[512];
    CHECK(realpath(argv[2], state) != NULL);
    CHECK(realpath(argv[3], input) != NULL);
    if (!strcmp(argv[1], "exec-child")) {
        denied_open(input, O_WRONLY);
        puts("exec child remains confined");
        return 0;
    }
    if (!strcmp(argv[1], "baseline")) {
        int fd = open(input, O_WRONLY);
        CHECK(fd >= 0); CHECK(close(fd) == 0);
        puts("negative control: unguarded input writable");
        return 0;
    }
    if (!strcmp(argv[1], "inherited")) {
        int fd = open(input, O_RDWR);
        CHECK(fd >= 0);
        CHECK(!rt_linux_state_guard(state, error, sizeof error));
        CHECK(strstr(error, "writable inherited fd") != NULL);
        CHECK(close(fd) == 0);
    }
    CHECK(rt_linux_state_guard(state, error, sizeof error));
    CHECK(chdir(state) == 0);
    // State can create, write, truncate and commit/rename across its subdirs.
    CHECK(mkdir("save", 0700) == 0);
    CHECK(mkdir("journal", 0700) == 0);
    int fd = open("journal/pending", O_CREAT | O_WRONLY | O_EXCL, 0600);
    CHECK(fd >= 0); CHECK(write(fd, "saved", 5) == 5); CHECK(fsync(fd) == 0); CHECK(close(fd) == 0);
    CHECK(rename("journal/pending", "save/committed") == 0);
    fd = open("save/committed", O_RDWR); CHECK(fd >= 0); CHECK(ftruncate(fd, 3) == 0); CHECK(close(fd) == 0);
    // Input can be read, but not written, truncated, deleted, moved or linked in.
    fd = open(input, O_RDONLY); CHECK(fd >= 0);
    char bytes[16] = {0}; CHECK(read(fd, bytes, sizeof bytes) == 8); CHECK(close(fd) == 0);
    CHECK(!strcmp(bytes, "sentinel"));
    denied_open(input, O_WRONLY);
    denied_open(input, O_RDWR | O_TRUNC);
    CHECK(unlink(input) == -1 && errno == EACCES);
    CHECK(rename(input, "stolen") == -1 && (errno == EACCES || errno == EXDEV));
    CHECK(link(input, "linked") == -1 && (errno == EACCES || errno == EXDEV));
    CHECK(symlink(input, "escape") == 0);
    denied_open("escape", O_WRONLY);
    CHECK(snprintf(path, sizeof path, "%s-new", input) < (int)sizeof path);
    denied_open(path, O_CREAT | O_WRONLY);
    CHECK(snprintf(path, sizeof path, "/proc/self/root%s", input) < (int)sizeof path);
    denied_open(path, O_WRONLY);
    // Existing input read descriptors cannot acquire writable rights via proc.
    fd = open(input, O_RDONLY); CHECK(fd >= 0);
    snprintf(path, sizeof path, "/proc/self/fd/%d", fd);
    denied_open(path, O_WRONLY); CHECK(close(fd) == 0);
    fd = open("/dev/null", O_WRONLY); CHECK(fd >= 0); CHECK(write(fd, "x", 1) == 1); CHECK(close(fd) == 0);
    pthread_t thread; CHECK(pthread_create(&thread, NULL, worker, NULL) == 0); CHECK(pthread_join(thread, NULL) == 0);
    pid_t child = fork(); CHECK(child >= 0);
    if (!child) { execl("/proc/self/exe", argv[0], "exec-child", state, input, (char *)NULL); _exit(2); }
    int status; CHECK(waitpid(child, &status, 0) == child); CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    puts("state guard: contents/create/truncate/remove/rename/link, threads and exec OK");
    return 0;
}
