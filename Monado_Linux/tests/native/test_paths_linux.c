// [XCDE] LINUX-004: actual paths implementation, synthetic temporary roots only.
#define _GNU_SOURCE
#include "../../runtime/paths.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s (%s)\n", __LINE__, #x, strerror(errno)); abort(); } } while (0)
static char base[PATH_MAX], input[PATH_MAX], state[PATH_MAX];
static int tests;

// Only platform services unused by initialization are supplied here. Link actual
// SDL for the inherited resource helpers; no GUI/SDL initialization is performed.
bool plat_exe_path(char *out, size_t n) {
    ssize_t size = readlink("/proc/self/exe", out, n);
    if (size < 0 || (size_t)size >= n) return false;
    out[size] = 0; return true;
}
bool plat_remove_tree(const char *path) { (void)path; CHECK(!"Linux path helpers must never delete shared input"); return false; }

static void join(char *out, const char *a, const char *b) {
    int n = snprintf(out, PATH_MAX, "%s/%s", a, b); CHECK(n >= 0 && n < PATH_MAX);
}
static void directory(const char *path) { CHECK(mkdir(path, 0700) == 0); }
static void put(const char *path, const char *text) {
    FILE *f = fopen(path, "wb"); CHECK(f); CHECK(fputs(text, f) >= 0); CHECK(fclose(f) == 0);
}
static void equal_file(const char *path, const char *text) {
    char buf[128] = {0}; FILE *f = fopen(path, "rb"); CHECK(f);
    CHECK(fread(buf, 1, sizeof buf - 1, f) == strlen(text)); CHECK(fclose(f) == 0); CHECK(!strcmp(buf, text));
}
static void init_fixture(const char *root, int n) {
    char name[32]; snprintf(name, sizeof name, "case-%02d", n); join(base, root, name); directory(base);
    join(input, base, "input é"); directory(input);
    char p[PATH_MAX]; join(p, input, "exefs"); directory(p); join(p, input, "romfs"); directory(p);
    join(p, input, "romfs/original"); put(p, "original input\n");
    join(p, input, ".write_test"); put(p, "input sentinel\n");
    join(state, base, "linux state");
}
static void assert_unchanged_input(void) {
    char p[PATH_MAX]; join(p, input, "romfs/original"); equal_file(p, "original input\n");
    join(p, input, ".write_test"); equal_file(p, "input sentinel\n");
}
static void child_case(int mode) {
    char error[512], p[PATH_MAX], q[PATH_MAX], cwd[PATH_MAX];
    const char *in = input, *st = state;
    CHECK(unsetenv("LARECOMP_SAVE_DIR") == 0);
    if (mode == 0 || mode == 1 || mode == 10 || mode == 18 || mode == 20 || mode == 25 || mode == 26) {
        if (mode == 26) { join(q, state, "new/nested"); strcpy(state, q); }
        if (mode < 25) { directory(state); join(p, state, ".write_test"); put(p, "state sentinel\n"); }
        if (mode == 1) { CHECK(chdir(base) == 0); in = "input é"; st = "linux state"; }
        if (mode == 10) { join(p, base, "input-link"); CHECK(symlink(input, p) == 0); in = p; }
        if (mode == 18) { CHECK(setenv("LARECOMP_SAVE_DIR", "profiles/disposable", 1) == 0); }
        if (mode == 20) {
            join(p, state, "internal"); put(p, "internal sentinel\n");
            join(q, state, "internal-link"); CHECK(symlink("internal", q) == 0);
        }
        CHECK(rt_paths_init_linux(in, st, error, sizeof error));
        CHECK(!strcmp(rt_input_root(), input)); CHECK(!strcmp(rt_data_root(), state));
        CHECK(getcwd(cwd, sizeof cwd) && !strcmp(cwd, state));
        CHECK(!strcmp(getenv("MONADO_INPUT_ROOT"), input)); CHECK(!strcmp(getenv("MONADO_STATE_ROOT"), state));
        CHECK(!strcmp(getenv("LARECOMP_DATA_ROOT"), state));
        const char *names[] = {"TMPDIR", "XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME", "__GL_SHADER_DISK_CACHE_PATH", "MESA_SHADER_CACHE_DIR"};
        const char *values[] = {"tmp", "config", "data", "cache", "cache/nvidia", "cache/mesa"};
        for (size_t i = 0; i < sizeof names / sizeof *names; i++) {
            join(p, state, values[i]); CHECK(!strcmp(getenv(names[i]), p)); struct stat sb; CHECK(!stat(p, &sb) && S_ISDIR(sb.st_mode));
        }
        join(p, state, mode == 18 ? "profiles/disposable" : "save"); CHECK(!strcmp(getenv("LARECOMP_SAVE_DIR"), p));
        put("xcde.ini", "synthetic config\n"); put("cache/synthetic", "synthetic cache\n");
        put("save/synthetic", "synthetic save\n"); put("logs/synthetic", "synthetic log\n");
        if (mode < 25) equal_file(".write_test", "state sentinel\n");
        else CHECK(access(".write_test", F_OK) != 0);
        CHECK(!rt_remove_game_file(input, "romfs")); CHECK(!rt_remove_game_files(input));
        CHECK(rt_game_files_size(input) == strlen("original input\n"));
        CHECK(!rt_paths_init_linux(in, st, error, sizeof error)); CHECK(strstr(error, "already initialized"));
        assert_unchanged_input(); return;
    }
    switch (mode) {
    case 2: st = input; break;
    case 3: join(p, input, "state"); st = p; break;
    case 4: st = base; break;
    case 5: CHECK(symlink(input, state) == 0); break;
    case 6: join(p, base, "link"); CHECK(symlink(input, p) == 0); join(q, p, "newstate"); st = q; break;
    case 7:
        directory(state); join(p, state, "cache"); join(q, input, "romfs"); CHECK(symlink(q, p) == 0); break;
    case 8:
        directory(state); join(p, state, "save"); directory(p); join(p, state, "save/link"); join(q, input, "romfs/original"); CHECK(symlink(q, p) == 0); break;
    case 9:
        directory(state); join(p, state, "hardlink"); join(q, input, "romfs/original"); CHECK(link(q, p) == 0); break;
    case 11:
        join(p, input, "exefs"); CHECK(rmdir(p) == 0); CHECK(symlink(base, p) == 0); break;
    case 12: { static char long_path[8192]; memset(long_path, 'a', sizeof long_path - 1); st = long_path; break; }
    case 13: join(p, base, "missing input"); in = p; break;
    case 14: join(p, input, "exefs"); CHECK(rmdir(p) == 0); break;
    case 15: st = NULL; break;
    case 16: put(state, "state file sentinel\n"); break;
    case 17: CHECK(setenv("LARECOMP_SAVE_DIR", input, 1) == 0); break;
    case 19: CHECK(setenv("LARECOMP_SAVE_DIR", "../input é", 1) == 0); break;
    case 21: directory(state); join(p, state, "dangling"); CHECK(symlink("missing", p) == 0); break;
    case 22: directory(state); join(p, state, "fifo"); CHECK(mkfifo(p, 0600) == 0); break;
    case 23: join(p, base, "input é/../state"); st = p; break;
    case 24: in = ""; break;
    case 27: {
        char segment[121]; memset(segment, 'l', sizeof segment - 1); segment[sizeof segment - 1] = 0;
        strcpy(p, base);
        for (int i = 0; i < 9; i++) { join(q, p, segment); strcpy(p, q); directory(p); }
        join(q, p, "exefs"); directory(q); join(q, p, "romfs"); directory(q); in = p; break;
    }
    default: CHECK(!"unknown test");
    }
    CHECK(getcwd(cwd, sizeof cwd));
    CHECK(!rt_paths_init_linux(in, st, error, sizeof error)); CHECK(*error);
    char after[PATH_MAX]; CHECK(getcwd(after, sizeof after)); CHECK(!strcmp(cwd, after));
    CHECK(!*rt_data_root() && !*rt_input_root());
    if (mode == 3) CHECK(access(p, F_OK) != 0);
    if (mode == 13 || mode == 14 || mode == 15 || mode == 17 || mode == 19 || mode == 24) CHECK(access(state, F_OK) != 0);
    if (mode == 16) equal_file(state, "state file sentinel\n");
    assert_unchanged_input();
}
int main(int argc, char **argv) {
    CHECK(argc == 2);
    for (int mode = 0; mode <= 27; mode++) {
        init_fixture(argv[1], mode);
        pid_t pid = fork(); CHECK(pid >= 0);
        if (!pid) { alarm(5); child_case(mode); _exit(0); }
        int status; CHECK(waitpid(pid, &status, 0) == pid); CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
        assert_unchanged_input(); tests++;
    }
    printf("Linux paths: %d synthetic isolation cases OK\n", tests); return 0;
}
