// [XCDE] LINUX-008: synthetic save service failure propagation, no game data.
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int injected_fsync(int fd);
static DIR *injected_opendir(const char *path);
#define fsync injected_fsync
#define opendir injected_opendir
#include "../../runtime/services/fs.c"
#undef fsync
#undef opendir

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
static bool fail_file_sync, fail_directory_sync, fail_open_directory;
static unsigned injected_errors;

static int injected_fsync(int fd) {
    struct stat st;
    CHECK(fstat(fd, &st) == 0);
    if ((S_ISREG(st.st_mode) && fail_file_sync) || (S_ISDIR(st.st_mode) && fail_directory_sync)) {
        injected_errors++;
        errno = EIO;
        return -1;
    }
    return fsync(fd);
}
static DIR *injected_opendir(const char *path) {
    if (fail_open_directory && !strcmp(path, "save")) {
        injected_errors++;
        errno = EACCES;
        return NULL;
    }
    return opendir(path);
}

// Unrelated service callbacks are not exercised by Flush/Commit requests.
const char *rando_seed_dir(void) { return NULL; }
void rt_fatal_trace(void) { abort(); }
void diag_unimplemented(const char *kind, const char *format, ...) { (void)kind; (void)format; abort(); }
Service *service_new(const char *name, ServiceHandler handler, void *state) {
    (void)name; (void)handler; (void)state; abort();
}
void ipc_return_object(IpcResp *response, Service *service) { (void)response; (void)service; abort(); }
void save_editor_file_read(const char *p, uint64_t a, int64_t o, int64_t n) { (void)p; (void)a; (void)o; (void)n; abort(); }
void flags_ext_save_read(const char *p, uint64_t a, int64_t o, int64_t n) { (void)p; (void)a; (void)o; (void)n; abort(); }
void flags_ext_save_written(const char *p, uint64_t a, int64_t o, int64_t n) { (void)p; (void)a; (void)o; (void)n; abort(); }

static void put(const char *path, const char *text) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    CHECK(fd >= 0);
    CHECK(write(fd, text, strlen(text)) == (ssize_t)strlen(text));
    CHECK(close(fd) == 0);
}
static void check_text(const char *path, const char *expected) {
    char bytes[64] = {0};
    int fd = open(path, O_RDONLY);
    CHECK(fd >= 0);
    CHECK(read(fd, bytes, sizeof bytes) == (ssize_t)strlen(expected));
    CHECK(!strcmp(bytes, expected));
    CHECK(close(fd) == 0);
}
static uint32_t commit(int journal) {
    HostFs fs = {.save = true, .journal = journal};
    strcpy(fs.root, "save");
    Service service = {.state = &fs};
    IpcReq request = {.cmd = 10};
    IpcResp response = {0};
    filesystem(&service, &request, &response);
    return response.result;
}

int main(int argc, char **argv) {
    CHECK(argc == 2);
    if (!strncmp(argv[1], "flush-", 6)) {
        put("file.bin", "synthetic");
        HostFile host = {.fd = open("file.bin", O_RDWR), .journal = -1};
        CHECK(host.fd >= 0);
        Service service = {.state = &host};
        IpcReq request = {.cmd = 2};
        IpcResp response = {0};
        fail_file_sync = !strcmp(argv[1], "flush-error");
        file(&service, &request, &response);
        fprintf(stderr, "flush result=%u injected_errors=%u\n", response.result, injected_errors);
        CHECK((response.result != 0) == fail_file_sync);
        CHECK(close(host.fd) == 0);
    } else {
        CHECK(setenv("XCDE_SAVE_BACKUPS", "0", 1) == 0);
        CHECK(mkdir("save", 0700) == 0);
        put("save/slot.bin", "last committed");
        int journal = journal_mount("save");
        CHECK(journal >= 0);
        journal_touch(journal);
        CHECK(g_journals[journal].dirty);
        check_text("save.journal/slot.bin", "last committed");
        put("save/slot.bin", "new pending");
        fail_file_sync = !strcmp(argv[1], "commit-file-error");
        fail_directory_sync = !strcmp(argv[1], "commit-directory-error");
        fail_open_directory = !strcmp(argv[1], "commit-open-error");
        bool failure_expected = fail_file_sync || fail_directory_sync || fail_open_directory;
        uint32_t result = commit(journal);
        fprintf(stderr, "commit result=%u dirty=%d journal_present=%d injected_errors=%u\n",
                result, g_journals[journal].dirty, access("save.journal", F_OK) == 0, injected_errors);
        CHECK((result != 0) == failure_expected);
        if (failure_expected) {
            CHECK(injected_errors > 0);
            CHECK(g_journals[journal].dirty);
            check_text("save.journal/slot.bin", "last committed");
            check_text("save/slot.bin", "new pending");
            fail_file_sync = fail_directory_sync = fail_open_directory = false;
            CHECK(commit(journal) == 0); // A successful retry can remove the rollback copy.
        }
        CHECK(!g_journals[journal].dirty);
        CHECK(access("save.journal", F_OK) != 0 && errno == ENOENT);
        check_text("save/slot.bin", "new pending");
    }
    puts("Synthetic save Flush/Commit checks passed");
    return 0;
}
