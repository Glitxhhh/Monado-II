// Authored synthetic bytes through production save journal + Quickstep pairing.
// This fixture only touches its new disposable directory argument.
#include <assert.h>
#include "../../runtime/services/fs.c"
#include "../../runtime/hle/quickstep_save.c"

Settings g_settings;
static QuickstepSnapshot fixture_snapshot;
const char *g_romfs_dir;
const char *rando_seed_dir(void) { return NULL; }
bool rando_romfs_override(const char *path, char *output, size_t size) { (void)path; (void)output; (void)size; abort(); }
void romfs_file_read_hook(const char *path) { (void)path; abort(); }
void rt_fatal_trace(void) { abort(); }
void diag_unimplemented(const char *kind, const char *format, ...) { (void)kind; (void)format; abort(); }
Service *service_new(const char *name, ServiceHandler handler, void *state) {
    Service *service = calloc(1, sizeof *service); assert(service);
    service->name = name; service->handler = handler; service->state = state; return service;
}
void ipc_return_object(IpcResp *response, Service *service) { response->objects[response->nobjects++] = service; }
void save_editor_file_read(const char *path, uint64_t address, int64_t offset, int64_t bytes) { (void)path; (void)address; (void)offset; (void)bytes; }
void flags_ext_save_read(const char *path, uint64_t address, int64_t offset, int64_t bytes) { (void)path; (void)address; (void)offset; (void)bytes; }
void flags_ext_save_written(const char *path, uint64_t address, int64_t offset, int64_t bytes) { (void)path; (void)address; (void)offset; (void)bytes; }
void quickstep_slot_snapshot(QuickstepSnapshot *snapshot) { *snapshot = fixture_snapshot; }
bool quickstep_slot_snapshot_for_save(uint64_t raw, QuickstepSnapshot *snapshot) { (void)raw; *snapshot = fixture_snapshot; return true; }
void quickstep_slot_suspend(void) {}
void quickstep_slot_adopt(uint64_t playing, uint32_t story, const QuickstepSnapshot *snapshot) { (void)playing; assert(story == snapshot->story); fixture_snapshot = *snapshot; }
void quickstep_slot_tick(Cpu *cpu, uint64_t context) { (void)cpu; (void)context; abort(); }
void guest_call(Cpu *cpu, uint64_t target) { (void)cpu; (void)target; abort(); }
bool rvfs_init(const char *path, char *reason, size_t length) { (void)path; (void)reason; (void)length; abort(); }
uint64_t rvfs_size(void) { abort(); }
int64_t rvfs_read(uint8_t *data, uint64_t size, uint64_t offset) { (void)data; (void)size; (void)offset; abort(); }
const char *rvfs_file_at(uint64_t offset) { (void)offset; abort(); }
// Native Win32 positional IO, also used by production fs.c through win_compat.
ssize_t pread(int fd, void *data, size_t bytes, off_t offset) {
    HANDLE handle = (HANDLE)_get_osfhandle(fd); OVERLAPPED operation = {0}; DWORD count = 0;
    operation.Offset = (DWORD)(uint64_t)offset; operation.OffsetHigh = (DWORD)((uint64_t)offset >> 32);
    if (!ReadFile(handle, data, (DWORD)bytes, &count, &operation)) {
        DWORD error = GetLastError(); if (error == ERROR_HANDLE_EOF) return 0; return ws_error(error);
    }
    return count;
}
ssize_t pwrite(int fd, const void *data, size_t bytes, off_t offset) {
    HANDLE handle = (HANDLE)_get_osfhandle(fd); OVERLAPPED operation = {0}; DWORD count = 0;
    operation.Offset = (DWORD)(uint64_t)offset; operation.OffsetHigh = (DWORD)((uint64_t)offset >> 32);
    return WriteFile(handle, data, (DWORD)bytes, &count, &operation) ? (ssize_t)count : ws_error(GetLastError());
}
static void capture(const void *payload) {
    memset(captures, 0, sizeof captures); captures[0].address = (uint64_t)(uintptr_t)payload;
    captures[0].age = 1; captures[0].snapshot = fixture_snapshot;
    quickstep_save_hash(payload, QUICKSTEP_SAVE_BYTES, captures[0].hash);
}
static void write_pair(HostFs *mount, const char *path, const void *payload) {
    capture(payload); QuickstepSaveWrite plan;
    assert(!quickstep_save_prepare(mount->root, path, payload, 0, QUICKSTEP_SAVE_BYTES, &plan));
    assert(plan.tracked && plan.bytes); assert(!journal_touch(mount->journal));
    assert(!quickstep_save_stage(mount->root, &plan));
    int fd = ws_open(path, O_CREAT | O_TRUNC | O_WRONLY, 0600); assert(fd >= 0);
    assert(pwrite(fd, payload, QUICKSTEP_SAVE_BYTES, 0) == QUICKSTEP_SAVE_BYTES);
    assert(!close(fd));
}
static void verify_pair(HostFs *mount, const void *expected) {
    uint8_t *payload = payload_read(mount->root, "bfsgame00.sav"); assert(payload);
    assert(!memcmp(payload, expected, QUICKSTEP_SAVE_BYTES)); free(payload);
    uint8_t metadata[QUICKSTEP_META_MAX], hash[32]; size_t bytes;
    assert(!metadata_read(mount->root, "bfsgame00.sav", metadata, &bytes));
    quickstep_save_hash(expected, QUICKSTEP_SAVE_BYTES, hash); QuickstepSnapshot restored;
    assert(!quickstep_save_decode("bfsgame00.sav", hash, QUICKSTEP_SAVE_BYTES, metadata, bytes, &restored));
    assert(!memcmp(&restored, &fixture_snapshot, sizeof restored));
}
int main(int argc, char **argv) {
    assert(argc == 2); assert(!ws_mkdir(argv[1], 0700));
    assert(!setenv("LARECOMP_SAVE_DIR", argv[1], 1)); assert(!setenv("XCDE_SAVE_BACKUPS", "0", 1));
    Service *service = open_host_fs("save_0"); HostFs *mount = service->state;
    assert(mount->journal >= 0 && !journal_poisoned(mount->journal));
    fixture_snapshot.count = 1; fixture_snapshot.entries[0].character = 1;
    fixture_snapshot.entries[0].uid = 0x30001; fixture_snapshot.entries[0].fingerprint[0] = 1;
    uint8_t *before = calloc(1, QUICKSTEP_SAVE_BYTES), *after = malloc(QUICKSTEP_SAVE_BYTES);
    assert(before && after); memset(after, 0xa5, QUICKSTEP_SAVE_BYTES);
    char path[4096], journal[4096], metadata[4096], temporary[4096];
    assert(!ws_join(path, sizeof path, mount->root, "bfsgame00.sav"));
    assert(snprintf(journal, sizeof journal, "%s.journal", mount->root) > 0);
    write_pair(mount, path, before); assert(!journal_commit(mount->journal)); verify_pair(mount, before);
    assert(!ws_exists(journal, NULL) && errno == ENOENT);
    // A locked payload makes journal acquisition fail BEFORE any mutation.
    WsGuard guard; assert(!ws_guard_open(&guard, path));
    HANDLE locked = CreateFileW(guard.path, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL); assert(locked != INVALID_HANDLE_VALUE);
    assert(journal_touch(mount->journal) < 0); assert(!g_journals[mount->journal].dirty);
    CloseHandle(locked); ws_guard_close(&guard); verify_pair(mount, before);
    // An exclusive sidecar temp collision blocks Commit, retaining rollback.
    write_pair(mount, path, after);
    char directory[PATH_CAP + 32]; assert(!metadata_directory(mount->root, false, directory));
    assert(!ws_join(temporary, sizeof temporary, directory, "bfsgame00.sav.tmp"));
    int fd = ws_open(temporary, O_CREAT | O_EXCL | O_WRONLY, 0600); assert(fd >= 0); assert(!close(fd));
    assert(journal_commit(mount->journal) < 0); assert(g_journals[mount->journal].dirty && ws_exists(journal, NULL));
    assert(!ws_unlink(temporary)); assert(!journal_commit(mount->journal)); verify_pair(mount, after);
    // Failed digest validation cannot discard rollback; restart restores pair.
    write_pair(mount, path, before); fd = ws_open(path, O_WRONLY); assert(fd >= 0);
    uint8_t corrupt = 7; assert(pwrite(fd, &corrupt, 1, 0) == 1); assert(!close(fd));
    assert(journal_commit(mount->journal) < 0 && errno == EIO); assert(ws_exists(journal, NULL));
    fs_save_journal_reset(); quickstep_save_reset();
    Service *reopened = open_host_fs("save_0"); mount = reopened->state;
    assert(!journal_poisoned(mount->journal)); verify_pair(mount, after);
    assert(!ws_exists(journal, NULL) && errno == ENOENT);
    // Reparse or locked metadata refuses IO; no native payload format changes.
    assert(!ws_join(metadata, sizeof metadata, directory, "bfsgame00.sav"));
    assert(!ws_guard_open(&guard, metadata));
    locked = CreateFileW(guard.path, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL); assert(locked != INVALID_HANDLE_VALUE);
    capture(before); QuickstepSaveWrite plan; assert(quickstep_save_prepare(mount->root, path, before, 0, QUICKSTEP_SAVE_BYTES, &plan) < 0);
    CloseHandle(locked); ws_guard_close(&guard); verify_pair(mount, after);
    // A present but invalid journal cannot be treated as an absent rollback.
    fd = ws_open(journal, O_CREAT | O_EXCL | O_WRONLY, 0600); assert(fd >= 0); assert(!close(fd));
    fs_save_journal_reset();
    int blocked = journal_mount(mount->root);
    assert(blocked >= 0 && journal_poisoned(blocked));
    assert(journal_touch(blocked) < 0 && errno == EIO);
    assert(journal_commit(blocked) < 0 && errno == EIO); verify_pair(mount, after);
    assert(!ws_unlink(journal));
    // Unsafe metadata inspection (here a hardlinked journal) likewise refuses
    // reads and mutations before touching the payload. Windows permits metadata
    // reads on sharing-locked directories, so that is not an invalid-journal test.
    WsGuard source; assert(!ws_guard_open(&source, path)); assert(!ws_guard_open(&guard, journal));
    assert(CreateHardLinkW(guard.path, source.path, NULL)); ws_guard_close(&source);
    fs_save_journal_reset(); blocked = journal_mount(mount->root);
    assert(blocked >= 0 && journal_poisoned(blocked));
    assert(journal_touch(blocked) < 0 && errno == EIO);
    // The fixture owns both names. Remove its extra link with the native API;
    // ordinary save operations deliberately reject files with multiple links.
    assert(DeleteFileW(guard.path)); ws_guard_close(&guard); verify_pair(mount, after);
    fs_save_journal_reset();
    assert(!journal_poisoned(journal_mount(mount->root))); verify_pair(mount, after);
    assert(!ws_tree(argv[1], NULL, WS_TREE_DELETE, false, 0)); free(before); free(after);
    puts("PASS production Windows save journal + Quickstep pairing, lock/temp/digest/invalid-journal failure, retry and restart recovery");
    return 0;
}
