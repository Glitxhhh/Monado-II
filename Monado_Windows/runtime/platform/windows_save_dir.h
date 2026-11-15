// [XCDE] Windows parity: UTF-8 facade over MinGW's Unicode directory API.
#pragma once
#ifdef _WIN32
#include <dirent.h>
#include "windows_save_io.h"
struct ws_dirent { char d_name[1024]; };
typedef struct { _WDIR *native; WsGuard guard; HANDLE owner; struct ws_dirent entry; } WsDir;
static WsDir *ws_opendir(const char *path) {
    WsDir *directory = calloc(1, sizeof *directory);
    if (!directory) { errno = ENOMEM; return NULL; }
    if (ws_guard_open(&directory->guard, path)) { free(directory); return NULL; }
    directory->owner = ws_file(&directory->guard, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES, OPEN_EXISTING, true);
    if (directory->owner == INVALID_HANDLE_VALUE) { ws_guard_close(&directory->guard); free(directory); return NULL; }
    directory->native = _wopendir(directory->guard.path);
    if (!directory->native) { int error = errno; CloseHandle(directory->owner); ws_guard_close(&directory->guard); free(directory); errno = error; return NULL; }
    return directory;
}
static struct ws_dirent *ws_readdir(WsDir *directory) {
    struct _wdirent *entry = _wreaddir(directory->native);
    if (!entry) return NULL;
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, entry->d_name, -1,
        directory->entry.d_name, sizeof directory->entry.d_name, NULL, NULL)) { errno = EINVAL; return NULL; }
    return &directory->entry;
}
static int ws_closedir(WsDir *directory) {
    int result = _wclosedir(directory->native), error = errno;
    if (!CloseHandle(directory->owner) && !result) { result = -1; ws_error(GetLastError()); error = errno; }
    ws_guard_close(&directory->guard); free(directory); errno = error; return result;
}
static long ws_telldir(WsDir *directory) { return _wtelldir(directory->native); }
static void ws_rewinddir(WsDir *directory) { _wrewinddir(directory->native); }
static void ws_seekdir(WsDir *directory, long position) { _wseekdir(directory->native, position); }
#endif
