// [XCDE] LINUX-018: one game/precompiler writer per private runtime state.
// Acquire after path confinement, before logs, caches, workers or guest startup.
// The close-on-exec descriptor lives until this process exits; external helper
// executables cannot accidentally keep the lease alive.
#pragma once
#include <stdbool.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

static int rt_linux_activity_fd = -1;
static bool rt_linux_activity_acquire(const char *root, char *error, size_t size) {
    if (rt_linux_activity_fd >= 0) return true;
    char path[4096];
    int n = snprintf(path, sizeof path, "%s/.runtime.lock", root);
    if (n < 0 || n >= (int)sizeof path) { snprintf(error, size, "runtime lock path is too long"); return false; }
    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    struct stat st;
    int e = 0;
    if (fd < 0) e = errno;
    else if (fstat(fd, &st)) e = errno;
    else if (!S_ISREG(st.st_mode) || st.st_uid != getuid() || st.st_nlink != 1) e = EINVAL;
    else if (flock(fd, LOCK_EX | LOCK_NB)) e = errno;
    if (e) {
        if (fd >= 0) close(fd);
        if (e == EWOULDBLOCK || e == EAGAIN)
            snprintf(error, size, "another game or shader precompiler is using this state directory");
        else snprintf(error, size, "cannot acquire private runtime lock: %s", strerror(e));
        return false;
    }
    rt_linux_activity_fd = fd;
    if (error && size) error[0] = 0;
    return true;
}
