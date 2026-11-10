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
static bool expanded_case;
static unsigned expanded_reads, expanded_writes;

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
    CHECK(expanded_case && !strcmp(name, "IFile"));
    Service *result = calloc(1, sizeof *result);
    CHECK(result != NULL);
    result->name = name; result->handler = handler; result->state = state;
    return result;
}
void ipc_return_object(IpcResp *response, Service *service) {
    CHECK(expanded_case && response->nobjects == 0);
    response->objects[response->nobjects++] = service;
}
void save_editor_file_read(const char *p, uint64_t a, int64_t o, int64_t n) {
    CHECK(expanded_case && !strncmp(p, "save/bfs", 8) && a && o == 0 && n > 0);
    ++expanded_reads;
}
void flags_ext_save_read(const char *p, uint64_t a, int64_t o, int64_t n) {
    CHECK(expanded_case && !strncmp(p, "save/bfs", 8) && a && o == 0 && n > 0);
}
void flags_ext_save_written(const char *p, uint64_t a, int64_t o, int64_t n) {
    CHECK(expanded_case && !strncmp(p, "save/bfs", 8) && a && o == 0 && n > 0);
    ++expanded_writes;
}
// The optional pair is covered by test_quickstep_save.c. These existing eleven
// cases retain their original payload-only contract and real journal handlers.
void quickstep_save_observe(const char *r, const char *p, const void *a, int64_t o, int64_t n) {
    (void)r; (void)p; (void)a; (void)o; (void)n;
}
int quickstep_save_prepare(const char *r, const char *p, const void *a, int64_t o, int64_t n, QuickstepSaveWrite *w) {
    (void)r; (void)p; (void)a; (void)o; (void)n; memset(w, 0, sizeof *w); return 0;
}
int quickstep_save_stage(const char *r, const QuickstepSaveWrite *w) { (void)r; (void)w; return 0; }
int quickstep_save_changed(const char *r, const char *p, const char *d) { (void)r; (void)p; (void)d; return 0; }
int quickstep_save_flush(const char *r) { (void)r; return 0; }
void quickstep_save_committed(const char *r) { (void)r; }

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

// [XCDE] LINUX-035: actual file/journal handlers, authored small payloads only.
static IpcResp save_request(HostFs *host, unsigned cmd, const char *name, int64_t value) {
    int64_t data[2] = {3, value}; // read/write mode, CreateFile's size at +8
    IpcReq req = {.cmd = cmd, .data = (const uint8_t *)data, .data_size = sizeof data, .nx = 1};
    req.x[0] = (IpcBuf){(uint64_t)(uintptr_t)name, strlen(name) + 1};
    Service fs = {.state = host}; IpcResp resp = {0};
    filesystem(&fs, &req, &resp);
    return resp;
}
static unsigned populated_files(void) {
    DIR *dir = opendir("save"); CHECK(dir != NULL);
    unsigned count = 0; struct dirent *entry;
    while ((entry = readdir(dir))) if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..")) ++count;
    CHECK(closedir(dir) == 0); return count;
}
static void original_slots(bool create) {
    const char *prefixes[] = {"bfsgame", "bfsmeria"};
    const char *slots[] = {"00a", "00", "01", "02"};
    const char *extensions[] = {"sav", "tmb"};
    for (unsigned story = 0; story < 2; ++story)
        for (unsigned slot = 0; slot < 4; ++slot)
            for (unsigned ext = 0; ext < 2; ++ext) {
                char path[64], contents[64];
                snprintf(path, sizeof path, "save/%s%s.%s", prefixes[story], slots[slot], extensions[ext]);
                snprintf(contents, sizeof contents, "original story %u slot %u kind %u", story, slot, ext);
                if (create) put(path, contents); else check_text(path, contents);
            }
    if (create) put("save/bfssystem.sav", "original system metadata");
    else check_text("save/bfssystem.sav", "original system metadata");
}
static void write_and_reload(HostFs *fs, const char *name, const char *contents) {
    IpcResp made = save_request(fs, 0, name, (int64_t)strlen(contents)); CHECK(made.result == 0);
    // A repeated create must not replace any existing payload.
    CHECK(save_request(fs, 0, name, 1).result == FS_PATH_EXISTS);
    IpcResp opened = save_request(fs, 8, name, 0); CHECK(!opened.result && opened.nobjects == 1);
    Service *handle = opened.objects[0]; CHECK(handle && handle->on_close);
    int64_t data[3] = {0, 0, (int64_t)strlen(contents)};
    IpcReq req = {.cmd = 1, .data = (const uint8_t *)data, .data_size = sizeof data, .na = 1};
    req.a[0] = (IpcBuf){(uint64_t)(uintptr_t)contents, strlen(contents)};
    IpcResp response = {0}; handle->handler(handle, &req, &response); CHECK(!response.result);
    req.cmd = 2; response = (IpcResp){0}; handle->handler(handle, &req, &response); CHECK(!response.result);
    char output[64] = {0}; req.cmd = 0; req.na = 0; req.nb = 1;
    req.b[0] = (IpcBuf){(uint64_t)(uintptr_t)output, sizeof output};
    response = (IpcResp){0}; handle->handler(handle, &req, &response);
    CHECK(!response.result && !strcmp(output, contents));
    handle->on_close(handle); free(handle->state); free(handle);
}
static void expanded_slots(const char *scenario) {
    expanded_case = true;
    CHECK(setenv("XCDE_SAVE_BACKUPS", "0", 1) == 0 && mkdir("save", 0700) == 0);
    original_slots(true); CHECK(populated_files() == 17);
    int journal = journal_mount("save"); CHECK(journal >= 0);
    HostFs fs = {.save = true, .journal = journal}; strcpy(fs.root, "save");
    // Looking up every extra slot in both stories must neither create files nor dirty a transaction.
    for (unsigned story = 0; story < 2; ++story) for (unsigned slot = 3; slot < 99; ++slot)
        for (unsigned ext = 0; ext < 2; ++ext) {
            char name[64]; snprintf(name, sizeof name, "%s%02u.%s", story ? "bfsmeria" : "bfsgame", slot, ext ? "tmb" : "sav");
            CHECK(save_request(&fs, 7, name, 0).result == FS_PATH_NOT_FOUND);
        }
    CHECK(populated_files() == 17 && !g_journals[journal].dirty && access("save.journal", F_OK) != 0);
    const char *names[] = {"bfsgame98.sav", "bfsgame98.tmb", "bfsmeria98.sav", "bfsmeria98.tmb"};
    const char *values[] = {"extended main payload", "extended main picture", "extended FC payload", "extended FC picture"};
    for (unsigned i = 0; i < 4; ++i) write_and_reload(&fs, names[i], values[i]);
    CHECK(populated_files() == 21 && expanded_writes == 4 && expanded_reads == 4);
    original_slots(false); CHECK(g_journals[journal].dirty);
    fail_file_sync = strcmp(scenario, "extended-slots") != 0;
    CHECK((commit(journal) != 0) == fail_file_sync);
    if (fail_file_sync) {
        CHECK(injected_errors && g_journals[journal].dirty);
        CHECK(access("save.journal/bfsgame98.sav", F_OK) != 0);
        check_text("save.journal/bfssystem.sav", "original system metadata");
        fail_file_sync = false;
        if (!strcmp(scenario, "extended-slots-recovery")) {
            // Fresh-session journal table over this synthetic fixture only; no original save is involved.
            memset(g_journals, 0, sizeof g_journals); g_njournals = 0;
            CHECK(journal_mount("save") >= 0);
            original_slots(false); CHECK(populated_files() == 17);
            for (unsigned i = 0; i < 4; ++i) {
                char path[80]; snprintf(path, sizeof path, "save/%s", names[i]);
                CHECK(access(path, F_OK) != 0 && errno == ENOENT);
            }
        } else CHECK(commit(journal) == 0);
    }
    CHECK(access("save.journal", F_OK) != 0);
    original_slots(false);
    if (strcmp(scenario, "extended-slots-recovery"))
        for (unsigned i = 0; i < 4; ++i) { char path[80]; snprintf(path, sizeof path, "save/%s", names[i]); check_text(path, values[i]); }
    puts("Expanded save filenames: lazy No data, independent main/FC high slots, no overwrite, original/autosave preservation and transaction checks passed");
}

int main(int argc, char **argv) {
    CHECK(argc == 2);
    if (!strncmp(argv[1], "extended-slots", 14)) { expanded_slots(argv[1]); return 0; }
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
