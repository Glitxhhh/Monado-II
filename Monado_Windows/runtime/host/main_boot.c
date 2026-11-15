// Headless boot host (M3a): loads the modules, sets up the kernel and runs nn::init::Start, with strict logging of
// every service, SVC and import the game touches. No window, no renderer. Derived from LASRecomp runtime/main.c (MIT). [XCDE]
#include "../diag/diag.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#include <windows.h>

#include "../gamelib.h"
#include "../hle.h"
#include "../kernel.h"
#include "../rt.h"

const char *g_romfs_dir;

#ifdef XCDE_GFX
// [XCDE] T194: laptops with two GPUs (NVIDIA Optimus / AMD switchable graphics) run a program on the integrated one
// unless the exe exports these; the driver reads them at start. (device.c then picks the discrete GPU among those
// Vulkan lists; LARECOMP_GPU=<n> overrides.)
__declspec(dllexport) DWORD NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
#endif

static void run_array(Cpu *c, uint64_t arr, uint64_t size) {
    for (uint64_t i = 0; i < size / 8; i++) {
        uint64_t fn = ((uint64_t *)arr)[i];
        if (fn) guest_call(c, fn);
    }
}
static void module_init(Cpu *c, Module *m) {
    LOG("init %s (DT_INIT %llx, %llu init_array entries)\n", m->name, (unsigned long long)m->init,
        (unsigned long long)(m->init_array_sz / 8));
    if (m->init) guest_call(c, m->init);
    else run_array(c, m->init_array, m->init_array_sz);
}
static void rtld_init_sdk(Cpu *c) { module_init(c, &g_modules[1]); }
static void rtld_init_main(Cpu *c) { module_init(c, &g_modules[0]); }

// [XCDE] WINPARITY-006: resolve the selected read-only input junctions separately
// from writable save/state paths, whose guards continue refusing reparse points.
static bool selected_input_directory(const char *path, char *out, size_t capacity) {
    wchar_t input[4096], final[4096];
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, input, 4096)) return false;
    HANDLE directory = CreateFileW(input, FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (directory == INVALID_HANDLE_VALUE) return false;
    BY_HANDLE_FILE_INFORMATION info;
    DWORD length = GetFinalPathNameByHandleW(directory, final, 4096, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    bool valid = GetFileInformationByHandle(directory, &info) && (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
    CloseHandle(directory);
    if (!valid || !length || length >= 4096) return false;
    const wchar_t *resolved = final;
    if (!wcsncmp(final, L"\\\\?\\UNC\\", 8)) { final[6] = L'\\'; resolved = final + 6; }
    else if (!wcsncmp(final, L"\\\\?\\", 4)) resolved = final + 4;
    return capacity <= INT_MAX &&
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, resolved, -1, out, (int)capacity, NULL, NULL) > 0;
}

__attribute__((constructor(101))) static void reserve_guest_space(void) {
    if (!plat_reserve_guest(GUEST_LO, GUEST_HI)) {
        // [XCDE] T135: something the loader placed at random (ASLR) sits in the guest range in ~1 of 5 starts. A new
        // process gets a new layout: start this one again (same command line, same console), up to 8 times.
        char n[16] = "0";
        GetEnvironmentVariableA("XCDE_RELAUNCH", n, sizeof n);
        int tries = atoi(n);
        fprintf(stderr, "[boot] guest address range %llx-%llx is not free (try %d)%s\n", (unsigned long long)GUEST_LO,
                (unsigned long long)GUEST_HI, tries + 1, tries < 8 ? ": starting again" : "");
        if (tries < 8) {
            snprintf(n, sizeof n, "%d", tries + 1);
            SetEnvironmentVariableA("XCDE_RELAUNCH", n);
            STARTUPINFOA si = {.cb = sizeof si};
            PROCESS_INFORMATION pi;
            char exe[MAX_PATH];
            GetModuleFileNameA(NULL, exe, sizeof exe);
            // the new process dies with this one (a test's timeout or a closed console must not leave it running)
            HANDLE job = CreateJobObjectA(NULL, NULL);
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION jl = {0};
            jl.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            if (job) SetInformationJobObject(job, JobObjectExtendedLimitInformation, &jl, sizeof jl);
            if (CreateProcessA(exe, GetCommandLineA(), NULL, NULL, TRUE, CREATE_SUSPENDED, NULL, NULL, &si, &pi)) {
                if (job) AssignProcessToJobObject(job, pi.hProcess);
                ResumeThread(pi.hThread);
                WaitForSingleObject(pi.hProcess, INFINITE);
                DWORD code = 1;
                GetExitCodeProcess(pi.hProcess, &code);
                ExitProcess(code);
            }
        }
        fprintf(stderr, "FATAL: guest address range %llx-%llx is not free\n", (unsigned long long)GUEST_LO,
                (unsigned long long)GUEST_HI);
        exit(1);
    }
}

// [XCDE] Watchdog (T027, first version): every few seconds the state of every guest thread; XCDE_WATCHDOG=<seconds>.
static DWORD WINAPI watchdog_main(LPVOID p) {
    unsigned secs = (unsigned)(uintptr_t)p;
    for (;;) { Sleep(secs * 1000); LOG("[wd] ---- %u s ----"" %c", secs, 10); kernel_dump_threads(stderr); }
}


#include "run_diagnostics_windows.h"
int main(int argc, char **argv) {
    if (argc >= 6 && !strcmp(argv[1], "--minidump")) return diag_minidump_helper(argc, argv);  // [XCDE] T028: crash helper process
    int argument=1;
#ifdef XCDE_GFX
    const char *fsr4_assets=NULL,*fsr4_component=NULL;
    while(argument<argc && (!strcmp(argv[argument],"--fsr4-assets") || !strcmp(argv[argument],"--fsr4-component"))) {
        const char **slot=!strcmp(argv[argument],"--fsr4-assets")?&fsr4_assets:&fsr4_component;
        if(*slot || argument+1>=argc || !*argv[argument+1]) {fprintf(stderr,"FSR4 options require one explicit path each\n");return 2;}
        *slot=argv[argument+1];argument+=2;
    }
    if((fsr4_assets!=NULL)!=(fsr4_component!=NULL)){fprintf(stderr,"FSR4 requires both --fsr4-assets and --fsr4-component\n");return 2;}
    if(fsr4_assets) {
        extern bool tmp_fsr4_configure(const char*,const char*);
        extern const char *tmp_fsr4_status(void);
        if(!tmp_fsr4_configure(fsr4_assets,fsr4_component))LOG("[fsr4] %s; existing fallback retained\n",tmp_fsr4_status());
    }
    if (argc >= argument+2 && !strcmp(argv[argument], "--precompile")) {
        extern int vk_precompile_main(const char *root);
        run_diag_begin(true);
        return vk_precompile_main(argv[argument+1]);
    }
#endif
    plat_init();
    setvbuf(stderr, NULL, _IONBF, 0);
    if (argc <= argument || argv[argument][0]=='-') { fprintf(stderr, "usage: xcde_boot [--fsr4-assets ABS --fsr4-component ABS] <dump dir with exefs/ and romfs/> [seconds]\n"); return 2; }
    const char *root = argv[argument];
    int seconds = argc > argument+1 ? atoi(argv[argument+1]) : 20;
    char exefs[1024], romfs[1024], npdm_path[1024];
    snprintf(exefs, sizeof exefs, "%s/exefs", root);
    snprintf(romfs, sizeof romfs, "%s/romfs", root);
    snprintf(npdm_path, sizeof npdm_path, "%s/main.npdm", exefs);
    g_romfs_dir = strdup(romfs);

    // [XCDE] WINPARITY-002 / R5: the split companion must be validated before
    // any watchdog, module loader, kernel or dispatch reads its function tables.
    // Static builds use the same contract through gamelib_static.c.
    char gamecode_error[512];
    if (!gamelib_load(root, gamecode_error, sizeof gamecode_error)) {
        fprintf(stderr, "FATAL: cannot load game code: %s\n", gamecode_error);
        return 1;
    }
    char resolved_exefs[4096], resolved_romfs[4096];
    if (!selected_input_directory(exefs, resolved_exefs, sizeof resolved_exefs) ||
        !selected_input_directory(romfs, resolved_romfs, sizeof resolved_romfs)) {
        fprintf(stderr, "FATAL: selected game input directories are missing or inaccessible\n");
        return 1;
    }
    if (snprintf(exefs, sizeof exefs, "%s", resolved_exefs) >= (int)sizeof exefs ||
        snprintf(romfs, sizeof romfs, "%s", resolved_romfs) >= (int)sizeof romfs ||
        snprintf(npdm_path, sizeof npdm_path, "%s/main.npdm", exefs) >= (int)sizeof npdm_path) {
        fprintf(stderr, "FATAL: selected game input path exceeds the supported runtime bound\n");
        return 1;
    }
    free((void *)g_romfs_dir);
    g_romfs_dir = strdup(romfs);
    // A real validated game startup only: shader preparation and failed/dry
    // entry checks return before creating any per-run diagnostic state.
    run_diag_begin(false);
    diag_watchdog_start();
    if (getenv("XCDE_WATCHDOG")) CreateThread(NULL, 0, watchdog_main, (LPVOID)(uintptr_t)atoi(getenv("XCDE_WATCHDOG")), 0, NULL);
    loader_load(exefs);
    kernel_init();
    dispatch_init();

    uint32_t stack_size = 0x100000;
    int prio = 44;
    FILE *f = fopen(npdm_path, "rb");
    if (f) {
        uint8_t meta[0x80];
        if (fread(meta, 1, sizeof meta, f) == sizeof meta && !memcmp(meta, "META", 4)) {
            prio = meta[0xE];
            stack_size = *(uint32_t *)(meta + 0x1C);
        }
        fclose(f);
    }
    LOG("main thread: prio %d, stack 0x%x\n", prio, stack_size);

    uint64_t start = loader_find_symbol("_ZN2nn4init5StartEmmPFvvES2_");
    if (!start) FATAL("nn::init::Start not found\n");
    uint64_t init_fn = hle_register("<rtld_init_sdk>", rtld_init_sdk);
    uint64_t fini_fn = hle_register("<rtld_init_main>", rtld_init_main);
    KThread *t = kernel_create_main_thread(start, stack_size, prio);
    kernel_run_main(t, t->handle, 0, init_fn, fini_fn);
#ifdef XCDE_GFX
    extern int ui_run(void);  // window + Vulkan presenter on the main thread (runtime/nvn/vulkan/device.c)
    LOG("[boot] running with the window\n");
    (void)seconds;
    return ui_run();
#else
    LOG("[boot] running; stopping after %d s\n", seconds);
    plat_sleep_until_ns(plat_now_ns() + (uint64_t)seconds * 1000000000ull);
    LOG("[boot] timeout reached, exiting\n");
    return 0;
#endif
}
