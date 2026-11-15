// [XCDE] WINPARITY-002: execute production startup with authored boundaries.
// No Nintendo input, library, function or window is used by this fixture.
#include <assert.h>
#include <windows.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include "../../runtime/kernel.h"
#include "../../runtime/gamelib.h"
#include "../../runtime/settings.h"

// Authored host settings storage; actual parsing is separately qualified by the
// native diagnostics/settings fixtures. Production diagnostic code is linked.
Settings g_settings;
int g_window_w=1280,g_window_h=720;
bool g_party_switch;

extern int authored_boot_main(int, char **);
Module g_modules[4];
int g_nmodules;
static int phase;

bool plat_reserve_guest(uint64_t a, uint64_t b) { (void)a; (void)b; return true; }
void plat_init(void) { phase = 1; }
void plat_sleep_until_ns(uint64_t n) { (void)n; abort(); }
uint64_t plat_now_ns(void) { return 0; }
void guest_call(Cpu *c, uint64_t a) { (void)c; (void)a; abort(); }
void rt_fatal_trace(void) { abort(); }
int diag_minidump_helper(int n, char **v) { (void)n; (void)v; return 24; }
void diag_watchdog_start(void) {
    assert(phase == 1 && gamelib_tables()); phase = 2;
}
void kernel_dump_threads(FILE *f) { (void)f; abort(); }
void loader_load(const char *p) {
    (void)p; assert(phase == 2 && gamelib_tables()); phase = 3;
    fputs("AUTHORED module-loader boundary\n", stderr);
}
void kernel_init(void) { assert(phase == 3 && gamelib_tables()); phase = 4; }
void dispatch_init(void) {
    assert(phase == 4 && gamelib_tables());
    assert(*gamelib_tables()->main_count == 1 && *gamelib_tables()->sdk_count == 1);
    fputs("AUTHORED dispatch boundary; stopped before guest execution\n", stderr);
    exit(0);
}
uint64_t loader_find_symbol(const char *p) { (void)p; abort(); }
uint64_t hle_register(const char *n, HostFn f) { (void)n; (void)f; abort(); }
KThread *kernel_create_main_thread(uint64_t a, uint64_t b, int c) { (void)a;(void)b;(void)c;abort(); }
void kernel_run_main(KThread *t, uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    (void)t; (void)a; (void)b; (void)c; (void)d; abort();
}
int ui_run(void) { abort(); }
bool tmp_fsr4_configure(const char *assets,const char *component) {
    assert(!phase && assets && component);
    assert(!strcmp(assets,"C:/authored models/雪")&&!strcmp(component,"C:/authored component.dll"));
    fputs("AUTHORED FSR4 configuration boundary\n",stderr);return false;
}
const char *tmp_fsr4_status(void) {return "authored local models unavailable";}
int vk_precompile_main(const char *p) {
    (void)p; assert(!phase); fputs("AUTHORED precompile boundary\n", stderr); return 23;
}
bool rt_resource(const char *name, const char *dev, char *out, size_t n) {
    (void)dev; const char *root = getenv("AUTHORED_LIBRARY_ROOT");
    return root && snprintf(out, n, "%s/%s", root, name) < (int)n;
}
bool plat_exe_path(char *out, size_t n) {
    wchar_t *root = _wgetenv(L"AUTHORED_LIBRARY_ROOT");
    char utf8[8192];
    if (!root || !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, root, -1, utf8, sizeof utf8, NULL, NULL)) return false;
    return snprintf(out, n, "%s/host.exe", utf8) < (int)n;
}
int main(int argc, char **argv) {
    assert(argc == 3);
    if (!strcmp(argv[1], "present")) return gamelib_present(NULL) ? 0 : 1;
    char input[8192];
    wchar_t *wide_input = _wgetenv(L"AUTHORED_LIBRARY_ROOT");
    assert(wide_input && WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide_input, -1, input, sizeof input, NULL, NULL));
    if(!strcmp(argv[1],"fsr4")||!strcmp(argv[1],"fsr4-missing")) {
        char *configured[]={argv[0],"--fsr4-assets","C:/authored models/雪","--fsr4-component","C:/authored component.dll",input,NULL};
        return authored_boot_main(!strcmp(argv[1],"fsr4")?6:3,configured);
    }
    char *args[] = {argv[0], !strcmp(argv[1], "precompile") ? "--precompile" : input, input, NULL};
    return authored_boot_main(!strcmp(argv[1], "precompile") ? 3 : 2, args);
}
