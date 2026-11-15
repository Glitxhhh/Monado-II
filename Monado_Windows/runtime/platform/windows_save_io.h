// [XCDE] Windows parity: bounded Unicode file operations for save transactions.
// Windows has no supported POSIX directory-fsync equivalent. Files are flushed;
// namespace replacements use MOVEFILE_WRITE_THROUGH, with rollback retained on
// any reported failure. This is native Windows persistence, not a POSIX claim.
#pragma once
#ifdef _WIN32
#include <windows.h>
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <wchar.h>

enum { WS_PATH = 4096, WS_DEPTH = 256 };
typedef struct { wchar_t path[WS_PATH]; HANDLE parents[WS_DEPTH]; unsigned count; } WsGuard;
static int ws_error(DWORD error) {
    switch (error) {
    case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: errno = ENOENT; break;
    case ERROR_ALREADY_EXISTS: case ERROR_FILE_EXISTS: errno = EEXIST; break;
    case ERROR_ACCESS_DENIED: case ERROR_SHARING_VIOLATION: case ERROR_LOCK_VIOLATION: errno = EACCES; break;
    case ERROR_DISK_FULL: case ERROR_HANDLE_DISK_FULL: errno = ENOSPC; break;
    case ERROR_FILENAME_EXCED_RANGE: errno = ENAMETOOLONG; break;
    case ERROR_NOT_ENOUGH_MEMORY: case ERROR_OUTOFMEMORY: errno = ENOMEM; break;
    default: errno = EIO; break;
    }
    return -1;
}
static void ws_guard_close(WsGuard *guard) {
    while (guard->count) CloseHandle(guard->parents[--guard->count]);
}
static bool ws_plain(HANDLE file, bool directory, bool single_link) {
    BY_HANDLE_FILE_INFORMATION info;
    if (!GetFileInformationByHandle(file, &info)) { ws_error(GetLastError()); return false; }
    if ((info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
        !!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != directory ||
        (single_link && info.nNumberOfLinks != 1)) { errno = EINVAL; return false; }
    return true;
}
// Holding every parent without FILE_SHARE_DELETE prevents an ancestor being
// renamed or exchanged for a junction while the checked operation is running.
static int ws_guard_open(WsGuard *guard, const char *path) {
    memset(guard, 0, sizeof *guard);
    wchar_t input[WS_PATH];
    if (!path || !*path || !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, input, WS_PATH))
        return errno = EINVAL, -1;
    if (!wcsncmp(input, L"\\\\?\\", 4) || !wcsncmp(input, L"\\\\.\\", 4)) return errno = EINVAL, -1;
    DWORD length = GetFullPathNameW(input, WS_PATH, guard->path, NULL);
    if (!length || length >= WS_PATH) return ws_error(length ? ERROR_FILENAME_EXCED_RANGE : GetLastError());
    for (wchar_t *p = guard->path; *p; ++p) if (*p == L'/') *p = L'\\';
    size_t root = 3;
    if (guard->path[0] == L'\\' && guard->path[1] == L'\\') {
        wchar_t *server = wcschr(guard->path + 2, L'\\');
        wchar_t *share = server ? wcschr(server + 1, L'\\') : NULL;
        if (!share) return errno = EINVAL, -1;
        root = (size_t)(share - guard->path) + 1;
    } else if (guard->path[1] != L':' || guard->path[2] != L'\\') return errno = EINVAL, -1;
    if (wcschr(guard->path + root, L':')) return errno = EINVAL, -1;
    for (size_t i = root; i < length; ++i) if (guard->path[i] == L'\\') {
        if (guard->count == WS_DEPTH) { ws_guard_close(guard); return errno = ENAMETOOLONG, -1; }
        guard->path[i] = 0;
        HANDLE parent = CreateFileW(guard->path, FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
        guard->path[i] = L'\\';
        if (parent == INVALID_HANDLE_VALUE) { DWORD error = GetLastError(); ws_guard_close(guard); return ws_error(error); }
        if (!ws_plain(parent, true, false)) { CloseHandle(parent); ws_guard_close(guard); return -1; }
        guard->parents[guard->count++] = parent;
    }
    return 0;
}
static HANDLE ws_file(const WsGuard *guard, DWORD access, DWORD disposition, bool directory) {
    HANDLE file = CreateFileW(guard->path, access, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
        disposition, FILE_FLAG_OPEN_REPARSE_POINT | (directory ? FILE_FLAG_BACKUP_SEMANTICS : 0), NULL);
    if (file == INVALID_HANDLE_VALUE) { ws_error(GetLastError()); return INVALID_HANDLE_VALUE; }
    if (!ws_plain(file, directory, !directory)) { CloseHandle(file); return INVALID_HANDLE_VALUE; }
    return file;
}
static int ws_open(const char *path, int flags, ...) {
    WsGuard guard;
    if (ws_guard_open(&guard, path)) return -1;
    DWORD access = (flags & O_RDWR) ? GENERIC_READ | GENERIC_WRITE : (flags & O_WRONLY) ? GENERIC_WRITE : GENERIC_READ;
    DWORD disposition = (flags & O_CREAT) ? ((flags & O_EXCL) ? CREATE_NEW : OPEN_ALWAYS) : OPEN_EXISTING;
    HANDLE file = ws_file(&guard, access, disposition, false);
    ws_guard_close(&guard);
    if (file == INVALID_HANDLE_VALUE) return -1;
    int descriptor = _open_osfhandle((intptr_t)file, flags | O_BINARY);
    if (descriptor < 0) { CloseHandle(file); return -1; }
    if ((flags & O_TRUNC) && _chsize_s(descriptor, 0)) { int error = errno; _close(descriptor); errno = error; return -1; }
    return descriptor;
}
static int ws_mkdir(const char *path, int mode) {
    (void)mode; WsGuard guard;
    if (ws_guard_open(&guard, path)) return -1;
    int result = CreateDirectoryW(guard.path, NULL) ? 0 : ws_error(GetLastError());
    ws_guard_close(&guard); return result;
}
static int ws_remove(const char *path, bool directory) {
    WsGuard guard; if (ws_guard_open(&guard, path)) return -1;
    HANDLE file = ws_file(&guard, DELETE, OPEN_EXISTING, directory);
    int result = -1;
    if (file != INVALID_HANDLE_VALUE) {
        FILE_DISPOSITION_INFO disposition = { TRUE };
        result = SetFileInformationByHandle(file, FileDispositionInfo, &disposition, sizeof disposition) ? 0 : ws_error(GetLastError());
        if (!CloseHandle(file) && !result) result = ws_error(GetLastError());
    }
    ws_guard_close(&guard); return result;
}
static int ws_unlink(const char *path) { return ws_remove(path, false); }
static int ws_rmdir(const char *path) { return ws_remove(path, true); }
static int ws_move(const char *from, const char *to) {
    WsGuard source, destination;
    if (ws_guard_open(&source, from)) return -1;
    if (ws_guard_open(&destination, to)) { ws_guard_close(&source); return -1; }
    DWORD attributes = GetFileAttributesW(source.path);
    int result = -1;
    if (attributes == INVALID_FILE_ATTRIBUTES) ws_error(GetLastError());
    else if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) errno = EINVAL;
    else result = MoveFileExW(source.path, destination.path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) ? 0 : ws_error(GetLastError());
    ws_guard_close(&destination); ws_guard_close(&source); return result;
}
static bool ws_exists(const char *path, bool *directory) {
    WsGuard guard; if (ws_guard_open(&guard, path)) return false;
    DWORD attributes = GetFileAttributesW(guard.path);
    if (attributes == INVALID_FILE_ATTRIBUTES) { ws_error(GetLastError()); ws_guard_close(&guard); return false; }
    if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) { errno = EINVAL; ws_guard_close(&guard); return false; }
    if (directory) *directory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    ws_guard_close(&guard); return true;
}
static int ws_join(char *output, size_t capacity, const char *root, const char *leaf) {
    if (!leaf || !*leaf || strchr(leaf, '/') || strchr(leaf, '\\') || strchr(leaf, ':') || !strcmp(leaf, ".") || !strcmp(leaf, ".."))
        return errno = EINVAL, -1;
    int length = snprintf(output, capacity, "%s/%s", root, leaf);
    return length < 0 || (size_t)length >= capacity ? (errno = ENAMETOOLONG, -1) : 0;
}
static int ws_directory(const char *path, bool create, HANDLE *result) {
    if (create && ws_mkdir(path, 0700) && errno != EEXIST) return -1;
    WsGuard guard; if (ws_guard_open(&guard, path)) return -1;
    *result = ws_file(&guard, FILE_READ_ATTRIBUTES, OPEN_EXISTING, true);
    ws_guard_close(&guard); return *result == INVALID_HANDLE_VALUE ? -1 : 0;
}
static int ws_stat(const char *path, struct stat *info) {
    WsGuard guard; if (ws_guard_open(&guard, path)) return -1;
    DWORD attributes = GetFileAttributesW(guard.path);
    if (attributes == INVALID_FILE_ATTRIBUTES) { DWORD error = GetLastError(); ws_guard_close(&guard); return ws_error(error); }
    bool directory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    HANDLE file = ws_file(&guard, FILE_READ_ATTRIBUTES, OPEN_EXISTING, directory);
    ws_guard_close(&guard);
    if (file == INVALID_HANDLE_VALUE) return -1;
    BY_HANDLE_FILE_INFORMATION native;
    if (!GetFileInformationByHandle(file, &native)) { DWORD error = GetLastError(); CloseHandle(file); return ws_error(error); }
    memset(info, 0, sizeof *info);
    info->st_mode = directory ? S_IFDIR : S_IFREG;
    info->st_nlink = native.nNumberOfLinks;
    info->st_size = ((uint64_t)native.nFileSizeHigh << 32) | native.nFileSizeLow;
    if (!CloseHandle(file)) return ws_error(GetLastError());
    return 0;
}
static int ws_copy_file(const char *from, const char *to) {
    int input = ws_open(from, O_RDONLY);
    if (input < 0) return -1;
    int output = ws_open(to, O_CREAT | O_EXCL | O_WRONLY, 0600);
    int error = output < 0 ? errno : 0;
    char data[65536];
    while (!error) {
        int count = _read(input, data, sizeof data);
        if (count < 0) { error = errno; break; }
        if (!count) break;
        for (int at = 0; at < count;) {
            int put = _write(output, data + at, count - at);
            if (put <= 0) { error = put < 0 ? errno : EIO; break; }
            at += put;
        }
    }
    if (output >= 0 && !error && _commit(output)) error = errno;
    if (output >= 0 && _close(output) && !error) error = errno;
    if (_close(input) && !error) error = errno;
    return error ? (errno = error, -1) : 0;
}
enum { WS_TREE_COPY, WS_TREE_FLUSH, WS_TREE_DELETE };
static int ws_tree(const char *path, const char *destination, int operation, bool keep_root, unsigned depth) {
    if (depth > 64) return errno = EINVAL, -1;
    WsGuard guard;
    if (ws_guard_open(&guard, path)) return -1;
    HANDLE directory = ws_file(&guard, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES, OPEN_EXISTING, true);
    if (directory == INVALID_HANDLE_VALUE) { ws_guard_close(&guard); return -1; }
    int error = 0;
    HANDLE target = INVALID_HANDLE_VALUE;
    if (operation == WS_TREE_COPY && ws_directory(destination, true, &target)) error = errno;
    wchar_t pattern[WS_PATH];
    int n = swprintf(pattern, WS_PATH, L"%ls\\*", guard.path);
    if (n < 0 || n >= WS_PATH) error = ENAMETOOLONG;
    WIN32_FIND_DATAW entry;
    HANDLE search = error ? INVALID_HANDLE_VALUE : FindFirstFileW(pattern, &entry);
    if (!error && search == INVALID_HANDLE_VALUE) {
        DWORD status = GetLastError();
        if (status != ERROR_FILE_NOT_FOUND) { ws_error(status); error = errno; }
    }
    if (search != INVALID_HANDLE_VALUE) for (;;) {
        if (wcscmp(entry.cFileName, L".") && wcscmp(entry.cFileName, L"..")) {
            char leaf[1024], child[WS_PATH], next[WS_PATH];
            if ((entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
                !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, entry.cFileName, -1, leaf, sizeof leaf, NULL, NULL)) error = EINVAL;
            else if (ws_join(child, sizeof child, path, leaf) ||
                (operation == WS_TREE_COPY && ws_join(next, sizeof next, destination, leaf))) error = errno;
            else if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (ws_tree(child, operation == WS_TREE_COPY ? next : NULL, operation, false, depth + 1)) error = errno;
            } else if (operation == WS_TREE_COPY) {
                if (ws_copy_file(child, next)) error = errno;
            } else if (operation == WS_TREE_DELETE) {
                if (ws_unlink(child)) error = errno;
            } else {
                int file = ws_open(child, O_RDWR);
                if (file < 0) error = errno;
                else {
                    if (_commit(file)) error = errno;
                    if (_close(file) && !error) error = errno;
                }
            }
        }
        if (error) break;
        if (!FindNextFileW(search, &entry)) {
            DWORD status = GetLastError();
            if (status != ERROR_NO_MORE_FILES) { ws_error(status); error = errno; }
            break;
        }
    }
    if (search != INVALID_HANDLE_VALUE && !FindClose(search) && !error) { ws_error(GetLastError()); error = errno; }
    if (target != INVALID_HANDLE_VALUE && !CloseHandle(target) && !error) { ws_error(GetLastError()); error = errno; }
    if (!CloseHandle(directory) && !error) { ws_error(GetLastError()); error = errno; }
    ws_guard_close(&guard);
    if (!error && operation == WS_TREE_DELETE && !keep_root && ws_rmdir(path)) error = errno;
    return error ? (errno = error, -1) : 0;
}
#endif
