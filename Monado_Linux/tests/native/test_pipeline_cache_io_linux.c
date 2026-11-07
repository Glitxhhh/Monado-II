// [XCDE] LINUX-018: actual atomic pipeline-cache helper, authored bytes/mock VK.
#define _GNU_SOURCE
#include <vulkan/vulkan.h>
#include <stdbool.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include "../../runtime/platform/linux_state_guard.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %s:%d: %s (errno %d)\n", __FILE__, __LINE__, #x, errno); exit(1); } } while (0)
static const char payload[] = "authored complete pipeline cache bytes";
static const char old[] = "original authored pipeline cache";
static const char *mode, *destination;
static char temporary_path[4096];
static unsigned queries, fetches, allocations, releases, write_calls, unlink_calls, rename_calls;
static int temporary_fd = -1;
static bool renamed, old_removed;
static void *allocation;

static bool is(const char *value) { return !strcmp(mode, value); }
static int fd_count(void) {
    DIR *dir = opendir("/proc/self/fd"); CHECK(dir); int count = 0;
    while (readdir(dir)) count++;
    CHECK(!closedir(dir)); return count;
}
static bool contents(const char *path, const char *expected, size_t size) {
    FILE *file = fopen(path, "rb"); if (!file) return false;
    char bytes[256]; size_t count = fread(bytes, 1, sizeof bytes, file); CHECK(!fclose(file));
    return count == size && !memcmp(bytes, expected, size);
}
static VkResult mock_data(VkDevice dev, VkPipelineCache cache, size_t *size, void *data) {
    CHECK(dev && cache);
    if (!data) {
        queries++;
        if (is("query-error")) return VK_ERROR_DEVICE_LOST;
        *size = is("query-zero") ? 0 : sizeof payload;
        return VK_SUCCESS;
    }
    fetches++;
    if (is("fetch-error")) return VK_ERROR_OUT_OF_HOST_MEMORY;
    if (is("fetch-zero")) { *size = 0; return VK_SUCCESS; }
    if (is("fetch-overflow")) { (*size)++; return VK_SUCCESS; }
    if (is("incomplete-limit") || (is("incomplete-once") && fetches == 1)) {
        *size += 4; return VK_INCOMPLETE;
    }
    CHECK(*size >= sizeof payload); memcpy(data, payload, sizeof payload); *size = sizeof payload;
    return VK_SUCCESS;
}
static void *mock_malloc(size_t size) {
    allocations++;
    if (is("oom")) return NULL;
    CHECK(!allocation); allocation = malloc(size); CHECK(allocation); return allocation;
}
static void mock_free(void *pointer) {
    if (pointer) { CHECK(pointer == allocation); releases++; free(pointer); allocation = NULL; }
}
static int mock_mkstemp(char *path) {
    CHECK(contents(destination, old, sizeof old));
    if (is("temp-open")) { errno = EACCES; return -1; }
    int fd = mkstemp(path); CHECK(fd >= 0); CHECK(strlen(path) < sizeof temporary_path);
    strcpy(temporary_path, path); temporary_fd = fd; return fd;
}
static int mock_fcntl(int fd, int operation, int value) {
    CHECK(operation == F_SETFD && value == FD_CLOEXEC);
    if (is("cloexec-error")) { errno = EIO; return -1; }
    return fcntl(fd, operation, value);
}
static ssize_t mock_write(int fd, const void *bytes, size_t size) {
    CHECK(fcntl(fd, F_GETFD) & FD_CLOEXEC); write_calls++;
    if (is("write-eintr") && write_calls == 1) { errno = EINTR; return -1; }
    if (is("write-error")) { errno = ENOSPC; return -1; }
    if (is("write-zero") && write_calls > 1) return 0;
    if (is("write-short") || is("write-zero")) { if (size > 3) size = 3; }
    return write(fd, bytes, size);
}
static int mock_fsync(int fd) {
    if ((!renamed && is("file-fsync")) || (renamed && is("directory-fsync"))) { errno = EIO; return -1; }
    return fsync(fd);
}
static int mock_close(int fd) {
    CHECK(!close(fd));
    if ((!renamed && is("file-close")) || (renamed && is("directory-close"))) { errno = EIO; return -1; }
    return 0;
}
static int mock_rename(const char *from, const char *to) {
    rename_calls++; CHECK(!strcmp(to, destination));
    CHECK(contents(destination, old, sizeof old)); CHECK(contents(from, payload, sizeof payload));
    if (is("rename-error")) { errno = EXDEV; return -1; }
    CHECK(!rename(from, to)); renamed = true; return 0;
}
static int mock_unlink(const char *path) {
    unlink_calls++; if (!strcmp(path, destination)) old_removed = true;
    CHECK(!strcmp(path, temporary_path)); return unlink(path);
}
static int mock_open(const char *path, int flags) {
    CHECK(renamed); CHECK(flags == (O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (is("directory-open")) { errno = EACCES; return -1; }
    return open(path, flags);
}

// Only the actual header's system/Vulkan boundaries are substituted. Its
// snapshot retries, atomic replacement and cleanup logic execute unchanged.
#define vkGetPipelineCacheData mock_data
#define malloc mock_malloc
#define free mock_free
#define mkstemp mock_mkstemp
#define fcntl mock_fcntl
#define write mock_write
#define fsync mock_fsync
#define close mock_close
#define rename mock_rename
#define unlink mock_unlink
#define open mock_open
#include "../../runtime/nvn/vulkan/pipeline_cache_io.h"
#undef vkGetPipelineCacheData
#undef malloc
#undef free
#undef mkstemp
#undef fcntl
#undef write
#undef fsync
#undef close
#undef rename
#undef unlink
#undef open

int main(int argc, char **argv) {
    CHECK(argc == 3); mode = argv[1]; const char *state = argv[2]; char error[512];
    CHECK(state[0] == '/'); CHECK(rt_linux_state_guard(state, error, sizeof error)); CHECK(!chdir(state));
    char absolute[4096];
    destination = "pipeline-cache.bin";
    if (is("relative-parent") || is("absolute-parent")) {
        CHECK(!mkdir("cache", 0700));
        if (is("relative-parent")) destination = "cache/pipeline-cache.bin";
        else {
            CHECK(snprintf(absolute, sizeof absolute, "%s/cache/pipeline-cache.bin", state) < (int)sizeof absolute);
            destination = absolute;
        }
    }
    FILE *file = fopen(destination, "wbx"); CHECK(file);
    CHECK(fwrite(old, 1, sizeof old, file) == sizeof old); CHECK(!fclose(file));
    int before = fd_count();
    const char *path = destination; char long_path[4100];
    if (is("long-path")) { memset(long_path, 'x', sizeof long_path - 1); long_path[sizeof long_path - 1] = 0; path = long_path; }
    VkDevice device = is("null-device") ? VK_NULL_HANDLE : (VkDevice)(uintptr_t)1;
    VkPipelineCache cache = is("null-cache") ? VK_NULL_HANDLE : (VkPipelineCache)(uintptr_t)1;
    if (is("null-path")) path = NULL;
    bool okay = vk_pipeline_cache_write_file(device, cache, path);
    bool success = is("success") || is("relative-parent") || is("absolute-parent") ||
                   is("write-short") || is("write-eintr") || is("incomplete-once");
    bool installed = success || is("directory-open") || is("directory-fsync") || is("directory-close");
    CHECK(okay == success);
    CHECK(contents(destination, installed ? payload : old, installed ? sizeof payload : sizeof old));
    CHECK(!old_removed); CHECK(!allocation); CHECK(fd_count() == before);
    CHECK(releases == allocations - (is("oom") ? 1u : 0u));
    if (temporary_path[0]) CHECK(access(temporary_path, F_OK) != 0 && errno == ENOENT);
    if (is("incomplete-limit")) CHECK(queries == 4 && fetches == 4 && allocations == 4);
    if (is("incomplete-once")) CHECK(queries == 2 && fetches == 2 && allocations == 2);
    if (is("write-short")) CHECK(write_calls > 1);
    if (is("write-eintr")) CHECK(write_calls >= 2);
    if (success) CHECK(rename_calls == 1 && unlink_calls == 0 && temporary_fd >= 0);
    printf("PIPELINE_CACHE %s PASS: return=%d installed=%d queries=%u fetches=%u writes=%u\n",
           mode, okay, installed, queries, fetches, write_calls);
    return 0;
}
