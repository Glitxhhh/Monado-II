// [XCDE] LINUX-018: actual shaderc/SPIR-V cache, authored GLSL and I/O faults.
// No guest data, Vulkan instance, UI or human state. Source inclusion exposes
// cache internals to targeted corruption probes without production test hooks.
#define _GNU_SOURCE
#define pread cache_test_pread
#define pwrite cache_test_pwrite
#define flock cache_test_flock
#define fdatasync cache_test_fdatasync
#define fsync cache_test_fsync
#include "../../runtime/nvn/vulkan/shaders.c"
#undef pread
#undef pwrite
#undef flock
#undef fdatasync
#undef fsync
#include "../../runtime/platform/linux_state_guard.h"
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>

const char smaa_hlsl_src[] = ""; // unrelated builtin branch is never requested
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %d: %s errno=%d\n", __LINE__, #x, errno); exit(1); } } while (0)
static int short_reads, short_writes, fail_writes, fail_locks, fail_sync;
static int read_eintr, write_eintr, lock_eintr, sync_eintr;

ssize_t cache_test_pread(int fd, void *buffer, size_t count, off_t offset) {
    if (read_eintr) { --read_eintr; errno = EINTR; return -1; }
    if (short_reads && fd == idx_fd && count > 8) count = 8;
    return syscall(SYS_pread64, fd, buffer, count, offset);
}
ssize_t cache_test_pwrite(int fd, const void *buffer, size_t count, off_t offset) {
    if (write_eintr) { --write_eintr; errno = EINTR; return -1; }
    if (fail_writes) { errno = EIO; return -1; }
    if (short_writes && count > 8) count = 8;
    return syscall(SYS_pwrite64, fd, buffer, count, offset);
}
int cache_test_flock(int fd, int operation) {
    if (lock_eintr) { --lock_eintr; errno = EINTR; return -1; }
    if (fail_locks && (operation & LOCK_EX)) { errno = EWOULDBLOCK; return -1; }
    return syscall(SYS_flock, fd, operation);
}
int cache_test_fdatasync(int fd) {
    if (sync_eintr) { --sync_eintr; errno = EINTR; return -1; }
    if (fail_sync) { errno = EIO; return -1; }
    return syscall(SYS_fdatasync, fd);
}
int cache_test_fsync(int fd) {
    if (sync_eintr) { --sync_eintr; errno = EINTR; return -1; }
    if (fail_sync) { errno = EIO; return -1; }
    return syscall(SYS_fsync, fd);
}

static const char fragment[] = "#version 450\nlayout(location=0) out vec4 color; void main(){color=vec4(0.2,0.4,0.7,1.0);}\n";
static uint32_t *compile(size_t *words) {
    uint32_t *result = vk_glsl_to_spirv(fragment, STAGE_FRAGMENT, "authored-cache.frag", words);
    CHECK(result && *words > 5 && result[0] == 0x07230203u);
    return result;
}
static void setup(const char *state) {
    char canonical[PATH_MAX], error[256];
    CHECK(realpath(state, canonical) && !strcmp(canonical, state));
    CHECK(chdir(state) == 0);
    CHECK(!mkdir("cache", 0700) || errno == EEXIST);
    CHECK(!mkdir("cache/spirv", 0700) || errno == EEXIST);
    CHECK(rt_linux_state_guard(state, error, sizeof error));
}
static uint8_t *contents(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb"); CHECK(f);
    CHECK(fseek(f, 0, SEEK_END) == 0); long count = ftell(f); CHECK(count >= 0);
    CHECK(fseek(f, 0, SEEK_SET) == 0);
    uint8_t *bytes = malloc((size_t)count + 1); CHECK(bytes);
    CHECK(fread(bytes, 1, (size_t)count, f) == (size_t)count && fclose(f) == 0);
    *size = (size_t)count; return bytes;
}
static void check_errors(bool expected) {
#ifndef CACHE_BEFORE_PROBE
    CHECK((vk_spirv_cache_errors() != 0) == expected);
#else
    (void)expected;
#endif
}

int main(int argc, char **argv) {
    CHECK(argc >= 3); const char *mode = argv[1]; setup(argv[2]);
    size_t words;
    if (!strcmp(mode, "cold-warm")) {
        uint32_t *first = compile(&words); size_t second_words;
        uint32_t *second = compile(&second_words);
        CHECK(second_words == words && !memcmp(first, second, words * 4));
        CHECK(vk_spirv_misses == 1 && vk_spirv_hits == 1);
        free(first); free(second); check_errors(false);
    } else if (!strcmp(mode, "reopen-hit")) {
        uint32_t *result = compile(&words); free(result);
        CHECK(vk_spirv_hits == 1 && vk_spirv_misses == 0); check_errors(false);
    } else if (!strcmp(mode, "corrupt-repair")) {
        uint32_t *first = compile(&words); free(first);
        const PackEntry *entry = pack_find(src_hash(fragment, STAGE_FRAGMENT)); CHECK(entry);
        uint32_t bad = 0; CHECK(syscall(SYS_pwrite64, pack_fd, &bad, 4, entry->off) == 4);
        CHECK(!vk_spirv_cached(fragment, STAGE_FRAGMENT));
        uint32_t *second = compile(&words); free(second);
        uint32_t *third = compile(&words); free(third);
        CHECK(vk_spirv_misses == 2 && vk_spirv_hits == 1);
        check_errors(false);
    } else if (!strcmp(mode, "structure-repair")) {
        uint32_t *result = compile(&words); free(result);
        const PackEntry *entry = pack_find(src_hash(fragment, STAGE_FRAGMENT)); CHECK(entry);
        uint32_t bad = 0; CHECK(syscall(SYS_pwrite64, pack_fd, &bad, 4, entry->off + 5 * 4) == 4);
        CHECK(!vk_spirv_cached(fragment, STAGE_FRAGMENT));
        result = compile(&words); free(result); result = compile(&words); free(result);
        CHECK(vk_spirv_misses == 2 && vk_spirv_hits == 1); check_errors(false);
    } else if (!strcmp(mode, "short-read")) {
        uint32_t *first = compile(&words); free(first);
        idx_read = 0; short_reads = 1;
        pack_refresh();
        short_reads = 0;
        uint32_t *second = compile(&words); free(second);
        CHECK(vk_spirv_misses == 1 && vk_spirv_hits == 1);
        check_errors(false);
    } else if (!strcmp(mode, "short-write")) {
        short_writes = 1;
        const uint32_t data[] = {0x07230203, 0x00010600, 0, 1, 0};
        CHECK(pack_put(0xabc123, data, 5));
        short_writes = 0;
        uint32_t *read = pack_get(0xabc123, &words);
        CHECK(read && words == 5 && !memcmp(read, data, sizeof data)); free(read);
        check_errors(false);
    } else if (!strcmp(mode, "lock-failure")) {
        fail_locks = 1;
        const uint32_t data[] = {0x07230203, 0x00010600, 0, 1, 0};
        CHECK(!pack_put(0xabc123, data, 5));
        CHECK(fd_size(pack_fd) == 0 && fd_size(idx_fd) == 0);
        fail_locks = 0; check_errors(true);
    } else if (!strcmp(mode, "write-failure-render")) {
        // Even storage failure must not drop a successfully compiled effect.
        CHECK(rmdir("cache/spirv") == 0);
        FILE *block = fopen("cache/spirv", "wb"); CHECK(block && fclose(block) == 0);
        fail_writes = 1;
        uint32_t *result = compile(&words); free(result);
        CHECK(vk_spirv_misses == 1); check_errors(true);
    } else if (!strcmp(mode, "flush-failure")) {
#ifndef CACHE_BEFORE_PROBE
        uint32_t *result = compile(&words); free(result);
        CHECK(vk_spirv_cache_flush()); check_errors(false);
        fail_sync = 1; CHECK(!vk_spirv_cache_flush()); fail_sync = 0;
        check_errors(true);
#else
        CHECK(!"new flush API is not in the baseline");
#endif
    } else if (!strcmp(mode, "eintr")) {
#ifndef CACHE_BEFORE_PROBE
        write_eintr = lock_eintr = 1;
        uint32_t *result = compile(&words); free(result);
        read_eintr = 1; idx_read = 0; pack_refresh();
        result = compile(&words); free(result);
        sync_eintr = 1; CHECK(vk_spirv_cache_flush());
        CHECK(vk_spirv_hits == 1 && vk_spirv_misses == 1); check_errors(false);
#else
        CHECK(!"new sync API is not in baseline");
#endif
    } else if (!strcmp(mode, "loose-buffered-failure")) {
        CHECK(setenv("XCDE_SPIRV_LOOSE", "1", 1) == 0);
        uint32_t *original = compile(&words);
        uint64_t h = src_hash(fragment, STAGE_FRAGMENT);
        char path[80]; snprintf(path, sizeof path, "cache/spirv/%016llx.spv", (unsigned long long)h);
        size_t before_size; uint8_t *before = contents(path, &before_size);
        struct rlimit old; CHECK(getrlimit(RLIMIT_FSIZE, &old) == 0);
        struct rlimit limit = {128, old.rlim_max};
        CHECK(signal(SIGXFSZ, SIG_IGN) != SIG_ERR && setrlimit(RLIMIT_FSIZE, &limit) == 0);
        cache_store(h, original, words);
        CHECK(setrlimit(RLIMIT_FSIZE, &old) == 0);
        size_t after_size; uint8_t *after = contents(path, &after_size);
        CHECK(before_size == after_size && !memcmp(before, after, before_size));
        free(before); free(after); free(original); check_errors(true);
    } else if (!strcmp(mode, "concurrency")) {
        pid_t children[3];
        for (unsigned i = 0; i < 3; ++i) {
            children[i] = fork(); CHECK(children[i] >= 0);
            if (!children[i]) { execl(argv[0], argv[0], "writer", argv[2], (char *)NULL); _exit(127); }
        }
        for (unsigned i = 0; i < 3; ++i) {
            int status; pid_t done;
            do { done = waitpid(children[i], &status, 0); } while (done < 0 && errno == EINTR);
            CHECK(done == children[i] && WIFEXITED(status) && WEXITSTATUS(status) == 0);
        }
        CHECK(pack_open() && pack_n == 12);
        for (size_t i = 0; i < pack_cap; ++i) if (pack_tab[i].hash) {
            uint32_t *result = pack_get(pack_tab[i].hash, &words); CHECK(result && words > 5); free(result);
        }
        CHECK(fd_size(idx_fd) == 12 * (int64_t)sizeof(PackEntry)); check_errors(false);
    } else if (!strcmp(mode, "writer")) {
        for (unsigned i = 0; i < 12; ++i) {
            char source[256]; snprintf(source, sizeof source, "#version 450\nlayout(location=0) out vec4 c; void main(){c=vec4(%u.0/16.0,0.4,0.7,1.0);}", i);
            uint32_t *result = vk_glsl_to_spirv(source, STAGE_FRAGMENT, "authored-concurrent.frag", &words);
            CHECK(result && words > 5); free(result);
        }
        check_errors(false);
    } else if (!strcmp(mode, "verify-writers")) {
        CHECK(pack_open() && pack_n == 12);
        for (size_t i = 0; i < pack_cap; ++i) if (pack_tab[i].hash) {
            uint32_t *result = pack_get(pack_tab[i].hash, &words); CHECK(result && words > 5); free(result);
        }
        CHECK(fd_size(idx_fd) == 12 * (int64_t)sizeof(PackEntry)); check_errors(false);
    } else { CHECK(!"unknown case"); }
#ifndef CACHE_BEFORE_PROBE
    if (!fail_writes && strcmp(mode, "flush-failure")) CHECK(vk_spirv_cache_flush());
#endif
    printf("PASS %s shaderc_hits=%llu misses=%llu\n", mode, (unsigned long long)vk_spirv_hits, (unsigned long long)vk_spirv_misses);
    return 0;
}
