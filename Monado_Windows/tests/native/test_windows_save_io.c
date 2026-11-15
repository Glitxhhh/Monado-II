// Authored disposable Windows save IO checks. No native game or save data.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../runtime/platform/windows_save_io.h"
#include "../../runtime/platform/windows_save_dir.h"
#include <winioctl.h>

static void put(const char *path, const char *text) {
    int file = ws_open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (file < 0) { fprintf(stderr, "open failed errno=%d Windows=%lu path=%s\n", errno, GetLastError(), path); abort(); }
    assert(_write(file, text, strlen(text)) == (int)strlen(text));
    assert(!_commit(file)); assert(!_close(file));
}
static void contents(const char *path, const char *expected) {
    char data[64] = {0}; int file = ws_open(path, O_RDONLY);
    assert(file >= 0); assert(_read(file, data, sizeof data - 1) == (int)strlen(expected));
    assert(!strcmp(data, expected)); assert(!_close(file));
}
static void path(char out[4096], const char *root, const char *leaf) {
    assert(!ws_join(out, 4096, root, leaf));
}
static bool junction(const char *link, const char *target) {
    assert(!ws_mkdir(link, 0700));
    WsGuard guard, dest; assert(!ws_guard_open(&guard, link)); assert(!ws_guard_open(&dest, target));
    HANDLE handle = CreateFileW(guard.path, GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, NULL);
    assert(handle != INVALID_HANDLE_VALUE);
    struct { DWORD tag; WORD length, reserved, sub_offset, sub_length, print_offset, print_length; wchar_t data[8192]; } reparse = {0};
    reparse.tag = IO_REPARSE_TAG_MOUNT_POINT;
    assert(swprintf(reparse.data, 8192, L"\\??\\%ls", dest.path) > 0);
    size_t substitution = wcslen(reparse.data), display = wcslen(dest.path);
    reparse.sub_length = (WORD)(substitution * sizeof(wchar_t));
    reparse.print_offset = (WORD)((substitution + 1) * sizeof(wchar_t));
    reparse.print_length = (WORD)(display * sizeof(wchar_t));
    memcpy(reparse.data + substitution + 1, dest.path, (display + 1) * sizeof(wchar_t));
    reparse.length = (WORD)(8 + (substitution + display + 2) * sizeof(wchar_t));
    DWORD count; bool result = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, &reparse,
        reparse.length + 8, NULL, 0, &count, NULL) != 0;
    DWORD error = GetLastError(); CloseHandle(handle); ws_guard_close(&dest); ws_guard_close(&guard);
    if (!result) { fprintf(stderr, "junction creation error=%lu\n", error); assert(!ws_rmdir(link)); }
    return result;
}
int main(int argc, char **argv) {
    assert(argc == 2); const char *root = argv[1]; assert(!ws_mkdir(root, 0700));
    char original[4096], temporary[4096], copy[4096], unicode[4096], link[4096], hardlink[4096];
    path(original, root, "ordinary.sav"); path(temporary, root, "exclusive.tmp");
    path(unicode, root, "\u6e2c\u8a66 with spaces.sav");
    put(original, "old"); put(unicode, "Unicode"); put(temporary, "new");
    assert(ws_open(temporary, O_CREAT | O_EXCL | O_WRONLY, 0600) < 0 && errno == EEXIST);
    assert(!ws_move(temporary, original)); contents(original, "new"); contents(unicode, "Unicode");
    WsGuard guard; assert(!ws_guard_open(&guard, original));
    HANDLE locked = CreateFileW(guard.path, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
    assert(locked != INVALID_HANDLE_VALUE); assert(ws_open(original, O_RDONLY) < 0 && errno == EACCES);
    CloseHandle(locked); ws_guard_close(&guard);
    path(hardlink, root, "hardlink.sav"); WsGuard one, two;
    assert(!ws_guard_open(&one, original)); assert(!ws_guard_open(&two, hardlink));
    assert(CreateHardLinkW(two.path, one.path, NULL));
    assert(ws_open(original, O_RDONLY) < 0 && errno == EINVAL);
    assert(DeleteFileW(two.path)); ws_guard_close(&two); ws_guard_close(&one);
    WsDir *directory = ws_opendir(root); assert(directory);
    bool found = false; struct ws_dirent *entry;
    while ((entry = ws_readdir(directory))) if (!strcmp(entry->d_name, "\u6e2c\u8a66 with spaces.sav")) found = true;
    assert(found); assert(!ws_closedir(directory));
    // Exercise native recursive copying/flush/cleanup with Unicode child names.
    assert(snprintf(copy, sizeof copy, "%s.copy", root) > 0);
    assert(!ws_tree(root, copy, WS_TREE_COPY, false, 0));
    path(temporary, copy, "\u6e2c\u8a66 with spaces.sav"); contents(temporary, "Unicode");
    assert(!ws_tree(copy, NULL, WS_TREE_FLUSH, true, 0));
    path(link, root, "junction");
    if (junction(link, copy)) {
        assert(!ws_guard_open(&one, link));
        HANDLE reparse = ws_file(&one, FILE_READ_ATTRIBUTES, OPEN_EXISTING, true);
        assert(reparse == INVALID_HANDLE_VALUE && errno == EINVAL); ws_guard_close(&one);
        path(temporary, link, "\u6e2c\u8a66 with spaces.sav");
        assert(ws_open(temporary, O_RDONLY) < 0 && errno == EINVAL);
        assert(ws_tree(root, NULL, WS_TREE_FLUSH, true, 0) < 0 && errno == EINVAL);
        assert(!ws_guard_open(&one, link)); assert(RemoveDirectoryW(one.path)); ws_guard_close(&one);
    } else puts("SKIP junction creation unavailable on this filesystem/token");
    assert(!ws_tree(copy, NULL, WS_TREE_DELETE, false, 0));
    assert(!ws_tree(root, NULL, WS_TREE_DELETE, false, 0));
    puts("PASS Windows Unicode, exclusive creation, replacement, sharing, hardlink, directory and reparse IO");
    return 0;
}
