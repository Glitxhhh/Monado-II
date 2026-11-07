// Data root and shipped resources (runtime/paths.h).
#define _GNU_SOURCE
#include <SDL3/SDL.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef _WIN32
#include <windows.h>
#endif

#ifndef __linux__
#include "gamelib.h"  // legacy setup owns/removes its generated game library
#endif
#include "paths.h"
#include "platform.h"

static char g_root[1024], g_start[1024], g_exe_dir[1024];

#ifdef _WIN32
#define SEP "\\"
static bool is_link(const char *p) {  // symlink or junction
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT);
}
static bool resolve(const char *p, char *out, size_t n) { return _fullpath(out, p, n) != NULL; }
#define path_eq(a, b) (_stricmp(a, b) == 0)
#else
#define SEP "/"
static bool is_link(const char *p) {
    struct stat st;
    return lstat(p, &st) == 0 && S_ISLNK(st.st_mode);
}
static bool resolve(const char *p, char *out, size_t n) {
    char buf[PATH_MAX];
    if (!realpath(p, buf)) return false;
    // [XCDE] LINUX-004: a truncated canonical path is never a valid identity.
    if (strlen(buf) >= n) { errno = ENAMETOOLONG; return false; }
    memcpy(out, buf, strlen(buf) + 1);
    return true;
}
#define path_eq(a, b) (strcmp(a, b) == 0)
#endif

static bool is_dir(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0;
}

#ifndef __linux__
static void mkdirs(const char *path) {
    char p[1024];
    snprintf(p, sizeof p, "%s", path);
    for (char *c = p + 1; *c; c++)
        if (*c == '/' || *c == '\\') { char s = *c; *c = 0; mkdir(p, 0755); *c = s; }
    mkdir(p, 0755);
}

static bool writable(const char *dir) {
    char t[1100];
    snprintf(t, sizeof t, "%s" SEP ".write_test", dir);
    FILE *f = fopen(t, "wb");
    if (!f) return false;
    fclose(f);
    remove(t);
    return true;
}
#endif

static void fail(const char *msg) {
    fprintf(stderr, "%s\n", msg);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "LAS Recomp", msg, NULL);
    exit(1);
}

static bool g_dev;  // the data root is the starting folder because it has extracted/ (a source checkout)
bool rt_dev_mode(void) { return g_dev; }

#ifdef __linux__
// [XCDE] LINUX-004: validation and ordinary output defaults. Enforcement of
// writes (including absolute diagnostic paths) is a separate startup guard.
static char g_input[1024];
static bool g_linux_ready;
const char *rt_input_root(void) { return g_input; }

static bool path_error(char *out, size_t n, const char *fmt, ...) {
    if (out && n) {
        va_list ap; va_start(ap, fmt); vsnprintf(out, n, fmt, ap); va_end(ap);
    }
    return false;
}
static bool beneath(const char *path, const char *root) {
    size_t n = strlen(root);
    return !strncmp(path, root, n) && (!path[n] || path[n] == '/');
}
static bool append_path(char *out, size_t n, const char *a, const char *b) {
    int size = snprintf(out, n, "%s/%s", a, b);
    if (size < 0 || (size_t)size >= n) { errno = ENAMETOOLONG; return false; }
    return true;
}

// State is allowed to be new, but every existing component must be an ordinary
// directory. Reject parent traversal rather than giving lexical '..' a different
// meaning from a filesystem path containing a symlink.
static bool state_absolute(const char *path, const char *base, char *out, size_t n,
                           char *error, size_t en) {
    char raw[PATH_MAX];
    if (!path || !*path) return path_error(error, en, "an explicit nonempty state root is required");
    if (*path == '/') {
        if (strlen(path) >= sizeof raw) return path_error(error, en, "state path is too long");
        strcpy(raw, path);
    } else if (!append_path(raw, sizeof raw, base, path)) return path_error(error, en, "state path is too long");
    if (n < 2) return path_error(error, en, "state path buffer is too small");
    strcpy(out, "/");
    char *save = NULL;
    for (char *part = strtok_r(raw, "/", &save); part; part = strtok_r(NULL, "/", &save)) {
        if (!strcmp(part, ".")) continue;
        if (!strcmp(part, "..")) return path_error(error, en, "state path may not contain parent traversal (..)");
        size_t used = strlen(out), length = strlen(part);
        bool slash = used > 1;
        if (used + slash + length >= n) return path_error(error, en, "state path is too long");
        if (slash) out[used++] = '/';
        memcpy(out + used, part, length + 1);
        struct stat st;
        if (!lstat(out, &st)) {
            if (S_ISLNK(st.st_mode)) return path_error(error, en, "state path contains a symlink: %s", out);
            if (!S_ISDIR(st.st_mode)) return path_error(error, en, "state path is not a directory: %s", out);
        } else if (errno != ENOENT) return path_error(error, en, "cannot inspect state directory: %s", strerror(errno));
    }
    if (!strcmp(out, "/")) return path_error(error, en, "the filesystem root cannot be a state directory");
    return true;
}

static bool inspect_state(const char *path, const char *root, unsigned depth, char *error, size_t en) {
    if (depth > 128) return path_error(error, en, "state directory nesting is too deep");
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return path_error(error, en, "cannot inspect state directory %s: %s", path, strerror(errno));
    DIR *dir = fdopendir(fd);
    if (!dir) { close(fd); return path_error(error, en, "cannot inspect state directory: %s", strerror(errno)); }
    bool ok = true;
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(dir);
        if (!entry) {
            if (errno) ok = path_error(error, en, "cannot enumerate state directory: %s", strerror(errno));
            break;
        }
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        char child[PATH_MAX]; struct stat st;
        if (!append_path(child, sizeof child, path, entry->d_name) ||
            fstatat(fd, entry->d_name, &st, AT_SYMLINK_NOFOLLOW)) {
            ok = path_error(error, en, "cannot inspect state entry: %s", strerror(errno)); break;
        }
        if (S_ISLNK(st.st_mode)) {
            char target[PATH_MAX];
            if (!resolve(child, target, sizeof target) || !beneath(target, root)) {
                ok = path_error(error, en, "state symlink escapes its root or is dangling: %s", child); break;
            }
        } else if (S_ISDIR(st.st_mode)) {
            if (!(ok = inspect_state(child, root, depth + 1, error, en))) break;
        } else if (!S_ISREG(st.st_mode) || st.st_nlink != 1) {
            ok = path_error(error, en, "state contains a special or multiply-linked file: %s", child); break;
        }
    }
    closedir(dir);
    return ok;
}

static bool create_state_dir(const char *path, char *error, size_t en) {
    char parts[PATH_MAX];
    if (strlen(path) >= sizeof parts) return path_error(error, en, "state directory path is too long");
    strcpy(parts, path);
    int fd = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return path_error(error, en, "cannot open filesystem root: %s", strerror(errno));
    char *save = NULL;
    for (char *part = strtok_r(parts, "/", &save); part; part = strtok_r(NULL, "/", &save)) {
        if (mkdirat(fd, part, 0700) && errno != EEXIST) {
            int failure = errno; close(fd); return path_error(error, en, "cannot create state directory: %s", strerror(failure));
        }
        int next = openat(fd, part, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        int failure = errno; close(fd);
        if (next < 0) return path_error(error, en, "unsafe state directory component: %s", strerror(failure));
        fd = next;
    }
    close(fd);
    return true;
}

bool rt_paths_init_linux(const char *input_root, const char *state_root, char *error, size_t en) {
    char start[sizeof g_start], input[sizeof g_input], state[sizeof g_root], save_dir[sizeof g_root];
    if (error && en) *error = 0;
    if (g_linux_ready) return path_error(error, en, "Linux paths are already initialized");
    if (!getcwd(start, sizeof start)) return path_error(error, en, "starting directory is unavailable or too long");
    if (!input_root || !*input_root || !resolve(input_root, input, sizeof input) || !is_dir(input))
        return path_error(error, en, "input must be an existing extracted directory with a bounded canonical path");
    for (const char **child = (const char *[]){"exefs", "romfs", NULL}; *child; ++child) {
        char path[PATH_MAX], canonical[PATH_MAX];
        if (!append_path(path, sizeof path, input, *child) || !resolve(path, canonical, sizeof canonical) ||
            !is_dir(canonical) || !beneath(canonical, input))
            return path_error(error, en, "input requires exefs/ and romfs/ directories inside its root");
    }
    if (!state_absolute(state_root, start, state, sizeof state, error, en)) return false;
    if (beneath(state, input) || beneath(input, state))
        return path_error(error, en, "input and state roots must be disjoint (neither may contain the other)");
    struct stat in_stat, state_stat;
    if (!stat(input, &in_stat) && !stat(state, &state_stat) && in_stat.st_dev == state_stat.st_dev && in_stat.st_ino == state_stat.st_ino)
        return path_error(error, en, "input and state refer to the same directory");
    const char *save = getenv("LARECOMP_SAVE_DIR");
    if (!state_absolute(save && *save ? save : "save", state, save_dir, sizeof save_dir, error, en)) return false;
    if (!beneath(save_dir, state) || !strcmp(save_dir, state))
        return path_error(error, en, "LARECOMP_SAVE_DIR must be a child of the isolated state root");
    if (is_dir(state) && !inspect_state(state, state, 0, error, en)) return false;
    const char *children[] = {"save", "cache", "cache/nvidia", "cache/mesa", "config", "data", "tmp", "logs", "crash", "build"};
    if (!create_state_dir(state, error, en)) return false;
    for (size_t i = 0; i < sizeof children / sizeof *children; ++i) {
        char path[PATH_MAX];
        if (!append_path(path, sizeof path, state, children[i]) || !create_state_dir(path, error, en)) return false;
    }
    if (!create_state_dir(save_dir, error, en)) return false;
    // Never truncate a user's existing fixed .write_test file as the inherited
    // writable() helper did. mkstemp creates a new, exclusive probe instead.
    char probe[PATH_MAX];
    if (!append_path(probe, sizeof probe, state, ".monado-write-XXXXXX")) return path_error(error, en, "state root is too long");
    int probe_fd = mkstemp(probe);
    if (probe_fd < 0) return path_error(error, en, "state root is not writable: %s", strerror(errno));
    close(probe_fd);
    if (unlink(probe)) return path_error(error, en, "cannot remove state write probe: %s", strerror(errno));
    const char *names[] = {"TMPDIR", "XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME", "__GL_SHADER_DISK_CACHE_PATH", "MESA_SHADER_CACHE_DIR"};
    const char *values[] = {"tmp", "config", "data", "cache", "cache/nvidia", "cache/mesa"};
    for (size_t i = 0; i < sizeof names / sizeof *names; ++i) {
        char value[PATH_MAX];
        if (!append_path(value, sizeof value, state, values[i]) || setenv(names[i], value, 1))
            return path_error(error, en, "cannot set isolated output environment: %s", strerror(errno));
    }
    if (setenv("MONADO_INPUT_ROOT", input, 1) || setenv("MONADO_STATE_ROOT", state, 1) ||
        setenv("LARECOMP_DATA_ROOT", state, 1) || setenv("LARECOMP_SAVE_DIR", save_dir, 1))
        return path_error(error, en, "cannot set isolated runtime roots: %s", strerror(errno));
    if (chdir(state)) return path_error(error, en, "cannot enter isolated state root: %s", strerror(errno));
    strcpy(g_input, input); strcpy(g_root, state); strcpy(g_start, start);
    if (plat_exe_path(g_exe_dir, sizeof g_exe_dir)) {
        char *slash = strrchr(g_exe_dir, '/');
        if (slash) *slash = 0;
    }
    g_linux_ready = true;
    fprintf(stderr, "[paths] input (read-only): %s\n[paths] state: %s\n", g_input, g_root);
    return true;
}
#endif

void rt_paths_init(void) {
#ifdef __linux__
    // Startup must supply the selected input explicitly; do not import another
    // LASRecomp installation or infer input from the process working directory.
    if (!g_linux_ready) fail("Monado Linux requires explicit input/state roots before platform initialization");
#else
    if (!getcwd(g_start, sizeof g_start)) g_start[0] = 0;
    if (plat_exe_path(g_exe_dir, sizeof g_exe_dir)) {
        char *s = strrchr(g_exe_dir, '/'), *b = strrchr(g_exe_dir, '\\');
        if (b > s) s = b;
        if (s) *s = 0;
    }
    char root[1024];
    const char *env = getenv("LARECOMP_DATA_ROOT");
    if (env && *env) snprintf(root, sizeof root, "%s", env);
    else if (is_dir("extracted")) snprintf(root, sizeof root, "%s", g_start), g_dev = true;  // dev / portable: as before
    else {
#ifdef _WIN32
        snprintf(root, sizeof root, "%s", g_exe_dir);
#elif defined(__linux__)
        // (the AppImage is read-only): $XDG_DATA_HOME/lasrecomp, ~/.local/share/lasrecomp by default
        const char *xdg = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
        if (xdg && *xdg == '/') snprintf(root, sizeof root, "%s/lasrecomp", xdg);
        else snprintf(root, sizeof root, "%s/.local/share/lasrecomp", home && *home ? home : "/tmp");
#else
        const char *home = getenv("HOME");
        snprintf(root, sizeof root, "%s/Library/Application Support/" RT_BUNDLE_ID, home && *home ? home : "/tmp");
#endif
    }
    mkdirs(root);
    char msg[1400];
    if (!is_dir(root) || !writable(root) || chdir(root) != 0) {
#ifdef _WIN32
        snprintf(msg, sizeof msg, "LAS Recomp can't write to its folder:\n%s\n\nIt keeps game files, saves and seeds "
                 "next to the program. Move the \"LAS Recomp\" folder somewhere you can write to (e.g. Documents or "
                 "Desktop, not Program Files) and start it again.", root);
#else
        snprintf(msg, sizeof msg, "LAS Recomp can't write to its data folder:\n%s", root);
#endif
        fail(msg);
    }
    if (!getcwd(g_root, sizeof g_root)) snprintf(g_root, sizeof g_root, "%s", root);
    fprintf(stderr, "[paths] data: %s\n", g_root);
#endif
}

const char *rt_data_root(void) { return g_root; }

bool rt_resource(const char *name, const char *dev_rel, char *out, size_t n) {
    char c[4][1100] = {{0}};
    if (name) {
        snprintf(c[0], sizeof c[0], "%s" SEP "%s", g_exe_dir, name);
        snprintf(c[1], sizeof c[1], "%s" SEP ".." SEP "Resources" SEP "%s", g_exe_dir, name);  // .app/Contents/Resources
    }
    if (dev_rel) {
        snprintf(c[2], sizeof c[2], "%s" SEP "%s", g_start, dev_rel);
        snprintf(c[3], sizeof c[3], "%s" SEP ".." SEP "%s", g_exe_dir, dev_rel);  // build/lasrecomp -> repo root
    }
    for (int i = 0; i < 4; i++)
        if (c[i][0] && exists(c[i])) return (size_t)snprintf(out, n, "%s", c[i]) < n;  // (as is: a venv python must stay a link)
    return false;
}

// ---- game files
// What the setup (runtime/ui/launcher.mm) creates in extracted/, plus what older versions' extraction and manual
// extractions left there (never read at run time).
#ifdef __linux__
// [XCDE] LINUX-004: the selected Linux input boundary is extracted folders only.
// Generated libraries/setup artifacts belong to the separate build/state roots.
static const char *const GAME_FILES[] = {"exefs", "romfs"};
#else
static const char *const GAME_FILES[] = {"exefs", "romfs", "romfs.bin", GAMELIB_NAME, GAMELIB_NAME ".id", ".gamebuild",
                                         ".extract", "extract.log", "xci_secure", "nso_raw", "update_pfs", ".DS_Store"};
#endif

// game_dir must be <data root>/extracted, a real folder (a dev worktree's symlinked extracted/ is refused)
static bool game_dir_ok(const char *game_dir) {
#ifdef __linux__
    (void)game_dir;
    return false;  // [XCDE] LINUX-004: extracted user input is never app-owned/deletable.
#else
    char want[1100], got[1100];
    snprintf(want, sizeof want, "%s" SEP "extracted", g_root);
    if (!g_root[0] || is_link(game_dir) || !resolve(game_dir, got, sizeof got)) return false;
    return path_eq(want, got);
#endif
}

bool rt_remove_game_file(const char *game_dir, const char *name) {
    bool known = false;
    for (size_t i = 0; i < sizeof GAME_FILES / sizeof *GAME_FILES; i++) known |= !strcmp(name, GAME_FILES[i]);
    if (!known || !game_dir_ok(game_dir)) {
        fprintf(stderr, "[paths] refusing to delete %s/%s (not app-created game files in the data root)\n", game_dir, name);
        return false;
    }
    char p[1100];
    snprintf(p, sizeof p, "%s" SEP "%s", game_dir, name);
    if (!exists(p) && !is_link(p)) return true;
    if (is_link(p)) {
        fprintf(stderr, "[paths] refusing to delete %s (a link)\n", p);
        return false;
    }
    return is_dir(p) ? plat_remove_tree(p) : remove(p) == 0;  // (plat_remove_tree doesn't follow links inside)
}

bool rt_remove_game_files(const char *game_dir) {
    if (!game_dir_ok(game_dir)) {
        fprintf(stderr, "[paths] refusing to delete game files in %s (not <data root>/extracted)\n", game_dir);
        return false;
    }
    bool ok = true;
    for (size_t i = 0; i < sizeof GAME_FILES / sizeof *GAME_FILES; i++) ok &= rt_remove_game_file(game_dir, GAME_FILES[i]);
    rmdir(game_dir);  // only if empty: anything else in there stays
    return ok;
}

static unsigned long long tree_size(const char *p) {
    struct stat st;
    if (is_link(p) || stat(p, &st) != 0) return 0;
    if (!S_ISDIR(st.st_mode)) return (unsigned long long)st.st_size;
    unsigned long long n = 0;
    DIR *d = opendir(p);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char c[2048];
            snprintf(c, sizeof c, "%s" SEP "%s", p, e->d_name);
            n += tree_size(c);
        }
        closedir(d);
    }
    return n;
}

unsigned long long rt_game_files_size(const char *game_dir) {
    unsigned long long n = 0;
    char p[1100];
    for (size_t i = 0; i < sizeof GAME_FILES / sizeof *GAME_FILES; i++) {
        snprintf(p, sizeof p, "%s" SEP "%s", game_dir, GAME_FILES[i]);
        n += tree_size(p);
    }
    return n;
}

// Window / Dock icon: icon.png shipped next to the exe or in the .app (none for a plain source build).
void rt_window_icon(SDL_Window *w) {
    char p[1100];
    if (!rt_resource("icon.png", NULL, p, sizeof p)) return;
    SDL_Surface *s = SDL_LoadPNG(p);
    if (s) SDL_SetWindowIcon(w, s), SDL_DestroySurface(s);
}
