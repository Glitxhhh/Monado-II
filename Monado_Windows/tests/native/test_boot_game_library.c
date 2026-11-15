// [XCDE] WINPARITY-002: execute production startup with authored boundaries.
// No Nintendo input, library, function or window is used by this fixture.
#include <assert.h>
#include <windows.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include "../../runtime/kernel.h"
#include "../../runtime/gamelib.h"
#include "../../runtime/platform/window_launch_hint_windows.h"
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

bool plat_reserve_guest(uint64_t a, uint64_t b) {
    (void)a;(void)b;
    const wchar_t *force=_wgetenv(L"AUTHORED_RELAUNCH");
    if(!force)return true;
    if(!wcscmp(force,L"always"))return false;
    const wchar_t *attempt=_wgetenv(L"XCDE_RELAUNCH");
    return attempt&&!wcscmp(attempt,L"1");
}
static bool relaunch_fault(const wchar_t *kind) {
    const wchar_t *fault=_wgetenv(L"AUTHORED_RELAUNCH_FAULT");
    if(fault&&!wcscmp(fault,kind)){SetLastError(ERROR_ACCESS_DENIED);return true;}
    return false;
}
HANDLE WINAPI authored_CreateJobObjectW(LPSECURITY_ATTRIBUTES attributes,LPCWSTR name) {
    return relaunch_fault(L"create")?NULL:CreateJobObjectW(attributes,name);
}
BOOL WINAPI authored_SetInformationJobObject(HANDLE job,JOBOBJECTINFOCLASS kind,LPVOID data,DWORD size) {
    return relaunch_fault(L"configure")?FALSE:SetInformationJobObject(job,kind,data,size);
}
BOOL WINAPI authored_AssignProcessToJobObject(HANDLE job,HANDLE child) {
    fprintf(stderr,"AUTHORED relaunch child PID %lu\n",(unsigned long)GetProcessId(child));
    return relaunch_fault(L"assign")?FALSE:AssignProcessToJobObject(job,child);
}
DWORD WINAPI authored_ResumeThread(HANDLE thread) {return relaunch_fault(L"resume")?(DWORD)-1:ResumeThread(thread);}
static void assert_profile(void) {
    const wchar_t *expected=_wgetenv(L"AUTHORED_PROFILE_ROOT");if(!expected)return;
    wchar_t actual[4096];DWORD count=GetCurrentDirectoryW(4096,actual);
    assert(count&&count<4096&&!_wcsicmp(actual,expected));
    HANDLE marker=CreateFileW(L"profile-marker",GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,0,NULL);
    assert(marker!=INVALID_HANDLE_VALUE);CloseHandle(marker);
}
void plat_init(void) { assert_profile();phase = 1; }
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
    assert_profile();assert(!phase);
    const char *expected=getenv("AUTHORED_PRECOMPILE_TARGET");if(expected)assert(!strcmp(p,expected));
    assert(getenv("XCDE_PRECOMPILE_SESSION")&&getenv("XCDE_PRECOMPILE_ID"));
    fputs("AUTHORED precompile boundary\n", stderr); return 23;
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
#ifdef AUTHORED_DIRECT_ENTRY
    return authored_boot_main(argc,argv);
#else
    assert(argc == 3);
    const wchar_t *expected=_wgetenv(L"AUTHORED_RELAUNCH_EXPECT");
    if(expected)assert(wcsstr(GetCommandLineW(),expected));
    if (!strcmp(argv[1], "present")) return gamelib_present(NULL) ? 0 : 1;
    char input[8192];
    wchar_t *wide_input = _wgetenv(L"AUTHORED_LIBRARY_ROOT");
    assert(wide_input && WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide_input, -1, input, sizeof input, NULL, NULL));
    if(!strncmp(argv[1],"profile-",8)) {
        wchar_t *wide_profile=_wgetenv(L"AUTHORED_PROFILE_ROOT");char profile[16384];
        assert(wide_profile&&WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,wide_profile,-1,profile,sizeof profile,NULL,NULL));
        char *prepared[]={argv[0],"--profile-root",profile,"--precompile",input,"--session","bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
            "--identity","aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",NULL};
        if(!strcmp(argv[1],"profile-missing"))return authored_boot_main(2,prepared);
        if(!strcmp(argv[1],"profile-empty"))prepared[2]="";
        if(!strcmp(argv[1],"profile-relative"))prepared[2]="relative-profile";
        if(!strcmp(argv[1],"profile-duplicate")){prepared[3]="--profile-root";return authored_boot_main(5,prepared);}
        if(!strcmp(argv[1],"profile-game")){prepared[3]=input;return authored_boot_main(4,prepared);}
        return authored_boot_main(9,prepared);
    }
    if(!strcmp(argv[1],"fsr4")||!strcmp(argv[1],"fsr4-missing")) {
        char *configured[]={argv[0],"--fsr4-assets","C:/authored models/雪","--fsr4-component","C:/authored component.dll",input,NULL};
        return authored_boot_main(!strcmp(argv[1],"fsr4")?6:3,configured);
    }
    if(!strncmp(argv[1],"monitor-",8)){
        char *configured[]={argv[0],"--launch-monitor","\\\\.\\DISPLAY2","--precompile",input,
            "--session","bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","--identity","aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",NULL};
        if(!strcmp(argv[1],"monitor-invalid"))configured[2]="\\\\.\\DISPLAY2;anything";
        if(!strcmp(argv[1],"monitor-missing"))return authored_boot_main(2,configured);
        if(!strcmp(argv[1],"monitor-duplicate"))configured[3]="--launch-monitor";
        int result=authored_boot_main(9,configured);
        if(!strcmp(argv[1],"monitor-valid"))assert(!strcmp(g_window_launch_monitor,"\\\\.\\DISPLAY2"));
        return result;
    }
    if(!strncmp(argv[1],"precompile",10)) {
        char *prepared[]={argv[0],"--precompile",input,"--session","bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
            "--identity","aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","--session","bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",NULL};
        int count=7;
        if(!strcmp(argv[1],"precompile-missing"))count=3;
        if(!strcmp(argv[1],"precompile-duplicate"))count=9;
        if(!strcmp(argv[1],"precompile-extra"))count=8;
        if(!strcmp(argv[1],"precompile-session"))prepared[4]="gggggggggggggggggggggggggggggggg";
        if(!strcmp(argv[1],"precompile-identity"))prepared[6]="aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
        return authored_boot_main(count,prepared);
    }
    char *args[]={argv[0],input,input,NULL};return authored_boot_main(2,args);
#endif
}
