// [XCDE] LINUX-004: real mount path validation; synthetic paths, no user save.
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); abort(); } } while (0)
#include "../../runtime/services/fs.c"

const char *rando_seed_dir(void) { return NULL; }
void rt_fatal_trace(void) { exit(97); }
Service *service_new(const char *name, ServiceHandler handler, void *state) {
    (void)name; (void)handler;
    static Service result;
    result.state = state;
    return &result;
}
int main(int argc, char **argv) {
    CHECK(argc == 2);
    CHECK(setenv("XCDE_SAVE_JOURNAL_OFF", "1", 1) == 0);
    if (!strcmp(argv[1], "overflow")) {
        char root[1016]; memset(root, 'x', sizeof root - 1); root[sizeof root - 1] = 0;
        CHECK(setenv("LARECOMP_SAVE_DIR", root, 1) == 0);
        open_host_fs("save_255");
        return 3; // A truncated mount must never be returned.
    }
    char root[1015] = "private-save";
    if (!strcmp(argv[1], "boundary")) {
        memset(root, 'x', sizeof root - 1); root[sizeof root - 1] = 0;
        for (size_t i = 100; i < sizeof root - 1; i += 101) root[i] = '/';
    }
    CHECK(setenv("LARECOMP_SAVE_DIR", root, 1) == 0);
    Service *mount = open_host_fs("save_255");
    HostFs *fs = mount->state;
    CHECK(!strncmp(fs->root, root, strlen(root)) && !strcmp(fs->root + strlen(root), "/save_255"));
    struct stat st; CHECK(stat(fs->root, &st) == 0 && S_ISDIR(st.st_mode));
    CHECK(fs->save && fs->journal == -1);
    free(fs);
    puts("Synthetic save mount preserves full root: OK");
    return 0;
}
