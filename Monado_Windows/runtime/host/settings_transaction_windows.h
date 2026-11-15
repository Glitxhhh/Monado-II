// [XCDE] WINPARITY-003: preserve custom INI lines and commit a complete file.
// Native file flush + write-through rename; no POSIX directory-fsync claim.
#pragma once
#include "../platform/windows_save_io.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <wctype.h>
#ifndef _CRT_RAND_S
#ifdef __cplusplus
extern "C" int __cdecl rand_s(unsigned int *);
#else
int __cdecl rand_s(unsigned int *);
#endif
#endif

typedef struct { char key[128]; } MonadoIniKey;
static bool monado_ini_key(const char *line, size_t length, char key[128]) {
    size_t start = 0, end = 0;
    while (start < length && (line[start] == ' ' || line[start] == '\t')) ++start;
    if (length - start >= 3 && !memcmp(line + start, "\xef\xbb\xbf", 3)) start += 3;
    if (start == length || line[start] == '#' || line[start] == ';' || line[start] == '[') return false;
    end = start;
    while (end < length && line[end] != '=' && line[end] != '\n') ++end;
    if (end == length || line[end] != '=') return false;
    while (end > start && (line[end - 1] == ' ' || line[end - 1] == '\t')) --end;
    if (end == start || end - start >= 128) return false;
    memcpy(key, line + start, end - start); key[end - start] = 0; return true;
}
// obsolete_binding identifies a known binding reset to its default and therefore
// intentionally absent from the serializer. Unknown keys, including bindings,
// remain byte-for-byte intact.
static bool monado_ini_write(const char *path, void (*emit)(FILE *), bool (*obsolete_binding)(const char *)) {
    WsGuard guard;
    if (ws_guard_open(&guard, path)) return false;
    uint64_t hash = 1469598103934665603ull;
    for (const wchar_t *p = guard.path; *p; ++p) { hash ^= (uint16_t)towlower(*p); hash *= 1099511628211ull; }
    wchar_t name[80]; swprintf(name, 80, L"Local\\MonadoIIIni%016llx", (unsigned long long)hash);
    HANDLE lease = CreateMutexW(NULL, FALSE, name);
    DWORD waited = lease ? WaitForSingleObject(lease, 3000) : WAIT_FAILED;
    if (waited != WAIT_OBJECT_0 && waited != WAIT_ABANDONED) {
        if (lease) CloseHandle(lease); ws_guard_close(&guard); return false;
    }
    bool ok = false, temporary_owned = false;
    FILE *file = NULL; char *old = NULL; MonadoIniKey *keys = NULL;
    char temporary[8192] = {0}; int fd = -1, old_fd = -1;
    size_t old_size = 0, count = 0;
    old_fd = ws_open(path, O_RDONLY);
    if (old_fd < 0 && errno != ENOENT) goto done;
    if (old_fd >= 0) {
        struct _stat64 info;
        if (_fstat64(old_fd, &info) || info.st_size < 0 || info.st_size > 1048576) goto done;
        old_size = (size_t)info.st_size;
        old = (char *)malloc(old_size + 1);
        if (!old || _read(old_fd, old, (unsigned)old_size) != (int)old_size) goto done;
        if (memchr(old, 0, old_size)) goto done;
        old[old_size] = 0; _close(old_fd); old_fd = -1;
    }
    for (unsigned attempt = 0; attempt < 32; ++attempt) {
        unsigned random;
        if (rand_s(&random)) goto done;
        int length = snprintf(temporary, sizeof temporary, "%s.tmp-%lu-%08x", path, GetCurrentProcessId(), random);
        if (length < 0 || (size_t)length >= sizeof temporary) goto done;
        fd = ws_open(temporary, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd >= 0 || errno != EEXIST) break;
    }
    if (fd < 0) goto done;
    temporary_owned = true;
    file = _fdopen(fd, "w+b");
    if (!file) goto done;
    fd = -1;
    emit(file);
    if (ferror(file) || fflush(file) || fseek(file, 0, SEEK_SET)) goto done;
    keys = (MonadoIniKey *)calloc(512, sizeof *keys);
    if (!keys) goto done;
    char line[4096], key[128];
    while (fgets(line, sizeof line, file)) if (monado_ini_key(line, strlen(line), key)) {
        if (count == 512) goto done;
        strcpy(keys[count++].key, key);
    }
    if (ferror(file) || fseek(file, 0, SEEK_END)) goto done;
    for (size_t start = 0; start < old_size;) {
        size_t end = start;
        while (end < old_size && old[end] != '\n') ++end;
        if (end < old_size) ++end;
        bool known = false;
        if (monado_ini_key(old + start, end - start, key)) {
            for (size_t i = 0; i < count; ++i) if (!strcmp(keys[i].key, key)) { known = true; break; }
            if (!known && obsolete_binding) known = obsolete_binding(key);
        }
        if (!known && fwrite(old + start, 1, end - start, file) != end - start) goto done;
        start = end;
    }
    if (ferror(file) || fflush(file) || _commit(_fileno(file))) goto done;
    if (fclose(file)) { file = NULL; goto done; }
    file = NULL;
    if (ws_move(temporary, path)) goto done;
    temporary[0] = 0; ok = true;
done:
    if (old_fd >= 0) _close(old_fd);
    if (file) fclose(file);
    if (fd >= 0) _close(fd);
    if (temporary_owned && *temporary) ws_unlink(temporary);
    free(keys); free(old);
    ReleaseMutex(lease); CloseHandle(lease); ws_guard_close(&guard);
    return ok;
}
