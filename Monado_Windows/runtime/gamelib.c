// Loads the recompiled game code library (runtime/gamelib.h).
#include "gamelib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#include "platform/windows_library_identity.h"
#else
#include <dlfcn.h>
#endif

#include "paths.h"
#include "rt.h"

static const GameLibTables *g_tables;
static char g_error[256];

// The library's id file (<library>.id, written next to it by the build): its GAMELIB_ID, read without loading it.
static bool id_matches(const char *lib) {
    char path[1100], id[128] = {0};
    snprintf(path, sizeof path, "%s.id", lib);
#ifdef _WIN32
    // [XCDE] WINPARITY-002: an identity check must never execute a DLL, follow
    // reparse points or accept a partial/trailing/ambient development identity.
    if (strlen(GAMELIB_ID) != 64 || !wl_read_hex(path, id)) return false;
    return !strcmp(id, GAMELIB_ID);
#else
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    size_t n = fread(id, 1, sizeof id - 1, f);
    fclose(f);
    while (n && (id[n - 1] == '\n' || id[n - 1] == '\r')) id[--n] = 0;
    return !strcmp(id, GAMELIB_ID);
#endif
}

static bool exists(const char *p) {
#ifdef _WIN32
    int fd = ws_open(p, O_RDONLY);
    if (fd < 0) return false;
    _close(fd);
    return true;
#else
    FILE *f = fopen(p, "rb");
    if (f) fclose(f);
    return f != NULL;
#endif
}

// The places to look, in order (gamelib.h). Returns how many were filled.
static int candidates(const char *game_dir, char out[3][1100]) {
    int n = 0;
#ifndef _WIN32
    const char *env = getenv("LARECOMP_GAME_LIB");
    if (env && *env) snprintf(out[n++], 1100, "%s", env);
#endif
    char exe_side[1024];
#ifdef _WIN32
    // Startup has not initialized mutable profile paths yet. Locate the
    // companion directly beside this exact executable, never via the cwd/input.
    if (plat_exe_path(exe_side, sizeof exe_side)) {
        char *slash = strrchr(exe_side, '/'), *backslash = strrchr(exe_side, '\\');
        if (!slash || (backslash && backslash > slash)) slash = backslash;
        if (slash) {
            slash[1] = 0;
            if (strlen(exe_side) + strlen(GAMELIB_NAME) < sizeof exe_side) {
                strcat(exe_side, GAMELIB_NAME);
                if (exists(exe_side)) snprintf(out[n++], 1100, "%s", exe_side);
            }
        }
    }
#else
    if (rt_resource(GAMELIB_NAME, NULL, exe_side, sizeof exe_side) && exists(exe_side))
        snprintf(out[n++], 1100, "%s", exe_side);
#endif
#ifndef _WIN32
    if (game_dir) snprintf(out[n++], 1100, "%s/%s", game_dir, GAMELIB_NAME);
#else
    (void)game_dir; // Only the selected runtime's local companion is eligible.
#endif
    return n;
}

bool gamelib_present(const char *game_dir) {
    if (g_tables) return true;
    char c[3][1100];
    int n = candidates(game_dir, c);
    for (int i = 0; i < n; i++)
        if (exists(c[i]) && id_matches(c[i])) {
#ifdef _WIN32
            char sidecar[1200], expected[65];
            snprintf(sidecar, sizeof sidecar, "%s.sha256", c[i]);
            WsGuard guard;
            if (!wl_read_hex(sidecar, expected) || ws_guard_open(&guard, c[i])) continue;
            HANDLE file = CreateFileW(guard.path, GENERIC_READ | READ_CONTROL, FILE_SHARE_READ,
                NULL, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
            bool valid = file != INVALID_HANDLE_VALUE && ws_plain(file, false, true) &&
                wl_owned(file) && wl_hash_matches(file, expected);
            if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
            ws_guard_close(&guard);
            if (valid) return true;
#else
            return true;
#endif
        }
    return false;
}

static const GameLibTables *open_lib(const char *path) {
#ifdef _WIN32
    WsGuard guard;
    if (ws_guard_open(&guard, path)) { snprintf(g_error, sizeof g_error, "unsafe game code path"); return NULL; }
    // Keep checked ancestors and the exact file immutable until loader mapping.
    HANDLE file = CreateFileW(guard.path, GENERIC_READ | READ_CONTROL, FILE_SHARE_READ, NULL,
        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    char sidecar[1200], expected[65];
    snprintf(sidecar, sizeof sidecar, "%s.sha256", path);
    if (file == INVALID_HANDLE_VALUE || !ws_plain(file, false, true) || !wl_owned(file) ||
        !wl_read_hex(sidecar, expected) || !wl_hash_matches(file, expected)) {
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        ws_guard_close(&guard);
        snprintf(g_error, sizeof g_error, "game code companion or SHA256 receipt is unsafe, missing or changed; rebuild locally"); return NULL;
    }
    HMODULE h = LoadLibraryExW(guard.path, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    CloseHandle(file); ws_guard_close(&guard);
    if (!h) { snprintf(g_error, sizeof g_error, "can't load %s (error %lu)", path, GetLastError()); return NULL; }
    GameLibEntry entry = (GameLibEntry)(void *)GetProcAddress(h, GAMELIB_ENTRY);
#else
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) { snprintf(g_error, sizeof g_error, "%s", dlerror()); return NULL; }
    GameLibEntry entry = (GameLibEntry)dlsym(h, GAMELIB_ENTRY);
#endif
    if (!entry) {
        snprintf(g_error, sizeof g_error, "%s: not game code", path);
#ifdef _WIN32
        FreeLibrary(h);
#else
        dlclose(h);
#endif
        return NULL;
    }
    const GameLibTables *t = entry();
    if (!t || t->abi != GAMELIB_ABI || !t->id || strcmp(t->id, GAMELIB_ID) ||
        !t->main_count || !t->sdk_count || !t->main_addrs || !t->sdk_addrs || !t->main_fns || !t->sdk_fns) {
        snprintf(g_error, sizeof g_error, "%s was built for another version", path);
#ifdef _WIN32
        FreeLibrary(h);
#else
        dlclose(h);
#endif
        return NULL;
    }
    return t;
}

bool gamelib_load(const char *game_dir, char *why, size_t why_size) {
    static bool tried;
    if (!tried) {
        tried = true;
        snprintf(g_error, sizeof g_error, "the game code hasn't been built: set up the game files on the start screen");
        char c[3][1100];
        int n = candidates(game_dir, c);
        for (int i = 0; i < n && !g_tables; i++) {
            if (!exists(c[i])) continue;
            if (!id_matches(c[i])) {
                snprintf(g_error, sizeof g_error, "%s was built for another version", c[i]);
                continue;
            }
            g_tables = open_lib(c[i]);
            if (g_tables) LOG("[gamelib] %s (%llu + %llu functions)\n", c[i], (unsigned long long)*g_tables->main_count,
                              (unsigned long long)*g_tables->sdk_count);
        }
    }
    if (!g_tables && why) snprintf(why, why_size, "%s", g_error);
    return g_tables != NULL;
}

const GameLibTables *gamelib_tables(void) { return g_tables; }
