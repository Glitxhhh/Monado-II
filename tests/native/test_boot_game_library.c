// [XCDE] LINUX-103: production boot entry with authored boundaries, never guest execution.
#include "gamelib.h"
#include "kernel.h"
#include "paths.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int authored_boot_main(int argc, char **argv);
static const char *library_root, *state_root, *input_root;
static bool loaded, initialized;
Module g_modules[4];

__attribute__((noreturn)) static void forbidden(const char *what) {
    fprintf(stderr, "FORBIDDEN authored boundary: %s\n", what);
    _Exit(92);
}
bool plat_reserve_guest(uint64_t lo, uint64_t hi) { (void)lo; (void)hi; return true; }
void plat_init(void) {}
void plat_exec(char *const argv[]) { (void)argv; forbidden("re-exec"); }
uint64_t plat_now_ns(void) { forbidden("clock"); return 0; }
void plat_sleep_until_ns(uint64_t n) { (void)n; forbidden("sleep"); }
void kernel_dump_threads(FILE *out) { (void)out; forbidden("watchdog"); }
void diag_watchdog_start(void) {}
int rt_radv_dcc_prepare(char *error, size_t size) { (void)error; (void)size; return 0; }
void rt_fatal_trace(void) { forbidden("fatal trace"); }
void guest_call(Cpu *cpu, uint64_t addr) { (void)cpu; (void)addr; forbidden("guest call"); }
uint64_t loader_find_symbol(const char *name) { (void)name; forbidden("guest symbol"); return 0; }
uint64_t hle_register(const char *name, HostFn fn) { (void)name; (void)fn; forbidden("guest registration"); return 0; }
KThread *kernel_create_main_thread(uint64_t entry, uint64_t stack, int prio) {
    (void)entry; (void)stack; (void)prio; forbidden("guest thread"); return NULL;
}
void kernel_run_main(KThread *thread, uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    (void)thread; (void)a; (void)b; (void)c; (void)d; forbidden("guest run");
}
int ui_run(void) { forbidden("visible UI"); return 92; }
bool tmp_fsr4_set_asset_dir(const char *path) { (void)path; forbidden("FSR4 assets"); return false; }

bool rt_paths_init_linux(const char *input, const char *state, char *error, size_t size) {
    (void)error; (void)size;
    assert(!strcmp(input, input_root) && !strcmp(state, state_root));
    return true;
}
const char *rt_data_root(void) { return state_root; }
const char *rt_input_root(void) { return input_root; }
bool rt_resource(const char *name, const char *unused, char *out, size_t size) {
    (void)unused;
    return snprintf(out, size, "%s/%s", library_root, name) < (int)size;
}
int authored_host_add(int value) { return value + 1; }

void loader_load(const char *path) {
    (void)path;
    if (!gamelib_tables()) {
        fprintf(stderr, "AUTHORED regression: module loader reached before game library\n");
        _Exit(91);
    }
    fprintf(stderr, "AUTHORED module-loader boundary\n");
    loaded = true;
}
void kernel_init(void) {
    assert(loaded && gamelib_tables());
    fprintf(stderr, "AUTHORED kernel boundary\n");
    initialized = true;
}
void dispatch_init(void) {
    const GameLibTables *tables = gamelib_tables();
    assert(initialized && tables && *tables->main_count == 1 && *tables->sdk_count == 1);
    fprintf(stderr, "AUTHORED dispatch boundary; stopped before guest execution\n");
    _Exit(0);
}
int vk_precompile_main(const char *root) {
    assert(!strcmp(root, input_root) && !loaded && !initialized);
    fprintf(stderr, "AUTHORED precompile boundary\n");
    return 23;
}

int main(int argc, char **argv) {
    assert(argc == 5);
    library_root = argv[2]; state_root = argv[3]; input_root = argv[4];
    char *game_args[] = {argv[0], "--state-root", argv[3], argv[4], NULL};
    char *precompile_args[] = {argv[0], "--state-root", argv[3], "--precompile", argv[4], NULL};
    if (!strcmp(argv[1], "precompile")) return authored_boot_main(5, precompile_args);
    assert(!strcmp(argv[1], "game"));
    return authored_boot_main(4, game_args);
}
