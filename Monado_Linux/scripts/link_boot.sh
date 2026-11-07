#!/usr/bin/env bash
# Link the headless boot executable (M3a): runtime objects + generated game objects.
cd "$(dirname "$0")/.."
Z=".venv/Scripts/python -m ziglang cc"
RT="build/rt/loader.o build/rt/dispatch.o build/rt/kernel.o build/rt/ipc.o build/rt/fnprof.o build/rt/platform_windows.o \
 build/rt/services_fs.o build/rt/services_romfs_virtual.o build/rt/services_am.o build/rt/services_acc.o build/rt/services_time.o \
 build/rt/services_hid_set.o build/rt/services_misc.o build/rt/hle_heaps.o build/rt/hle_pl.o build/rt/hle_vi.o build/rt/hle_nvn_boot.o build/rt/hle_fibers.o build/rt/hle_framepace.o \
 build/rt/host_stubs.o build/rt/host_settings_boot.o build/rt/host_gamelib_static.o build/rt/host_main_boot.o build/rt/diag_diag.o build/rt/diag_sampler.o build/rt/diag_watchdog.o build/rt/diag_crashdump.o"
$Z -o build/xcde_boot.exe $RT build/obj/main/*.o build/obj/sdk/*.o -march=x86_64_v2 -lwinmm -ldbghelp -Wl,--stack,268435456 2> build/link.err
rc=$?; grep "undefined symbol" build/link.err | sed 's/.*undefined symbol: //' | sort -u | head -30; exit $rc
