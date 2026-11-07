#!/usr/bin/env bash
# Link the windowed executable (M4): headless runtime + NVN/Vulkan backend + SDL3.
# --stack 256 MB: the default thread stack *reservation* (committed on use); guest threads rely on it (kernel.c thread_start).
cd "$(dirname "$0")/.."
Z=".venv/Scripts/python -m ziglang c++"
M=/c/msys64/ucrt64
RT="build/rt/loader.o build/rt/dispatch.o build/rt/kernel.o build/rt/ipc.o build/rt/fnprof.o build/rt/platform_windows.o \
 build/rt/services_fs.o build/rt/services_romfs_virtual.o build/rt/services_am.o build/rt/services_acc.o build/rt/services_time.o \
 build/rt/services_hid_set.o build/rt/services_kbicons.o build/rt/services_misc.o build/rt/hle_heaps.o build/rt/hle_pl.o build/rt/hle_vi.o build/rt/hle_nvn_boot.o build/rt/hle_fibers.o \
 build/rt/host_stubs.o build/rt/host_settings_ini.o build/rt/host_stubs_gfx.o build/rt/services_audio.o build/rt/services_hwopus.o build/rt/hle_input.o build/rt/hle_party.o build/rt/hle_bindings.o build/rt/hle_movie.o build/rt/hle_framepace.o build/rt/host_gamelib_static.o build/rt/host_main_boot_gfx.o build/rt/diag_diag.o build/rt/diag_sampler.o build/rt/diag_watchdog.o build/rt/diag_crashdump.o"
NVN="$(ls build/nvn/*.o | tr '\n' ' ')"
$Z -o "${XCDE_EXE:-build/xcde.exe}" $RT $NVN build/obj/main/*.o build/obj/sdk/*.o -march=x86_64_v2 \
  $M/lib/libSDL3.dll.a $M/lib/libshaderc_shared.dll.a $M/lib/libvulkan-1.dll.a $M/lib/libopus.dll.a $M/lib/libavcodec.dll.a $M/lib/libavformat.dll.a $M/lib/libavutil.dll.a $M/lib/libswscale.dll.a $M/lib/libswresample.dll.a $M/lib/libz.a -lwinmm -ldbghelp -Wl,--stack,268435456 2> build/link_gfx.err
rc=$?; grep "undefined symbol" build/link_gfx.err | sed 's/.*undefined symbol: //' | sort -u | head -60; exit $rc
