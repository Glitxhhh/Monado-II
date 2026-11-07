#!/usr/bin/env bash
# Native (C) runtime tests, Windows host: fibers (T032b) and condvar timeout semantics (T031). No game data needed.
set -e
cd "$(dirname "$0")/.."
Z=".venv/Scripts/python -m ziglang cc"
mkdir -p build/test build/rt
$Z -c runtime/platform/windows.c -o build/rt/platform_windows.o -O1 -w -Iruntime
$Z -O1 -w -Iruntime -o build/test/test_fibers.exe tests/native/test_fibers.c runtime/hle/fibers.c
$Z -O1 -w -Iruntime -o build/test/test_condvar.exe tests/native/test_condvar.c build/rt/platform_windows.o runtime/diag/diag.c runtime/diag/sampler.c -lwinmm -ldbghelp
build/test/test_fibers.exe
build/test/test_condvar.exe
$Z -O1 -w -Iruntime/nvn/maxwell -o build/test/test_translate_tex.exe tests/native/test_translate_tex.c runtime/nvn/maxwell/translate.c
build/test/test_translate_tex.exe
$Z -O1 -w -Iruntime -o build/test/test_movie_buf.exe tests/native/test_movie_buf.c
build/test/test_movie_buf.exe
$Z -O1 -w -Iruntime/nvn/maxwell -o build/test/test_translate_ldc.exe tests/native/test_translate_ldc.c runtime/nvn/maxwell/translate.c
build/test/test_translate_ldc.exe
$Z -O1 -w -Iruntime/nvn/maxwell -o build/test/test_translate_xfb.exe tests/native/test_translate_xfb.c runtime/nvn/maxwell/translate.c
build/test/test_translate_xfb.exe
$Z -O1 -w -Iruntime/nvn -o build/test/test_tegra_block_height.exe tests/native/test_tegra_block_height.c runtime/nvn/tegra.c runtime/nvn/formats.c
build/test/test_tegra_block_height.exe
$Z -O1 -w -Iruntime/nvn -o build/test/test_tegra_copy.exe tests/native/test_tegra_copy.c runtime/nvn/tegra.c runtime/nvn/formats.c
build/test/test_tegra_copy.exe
$Z -O1 -w -o build/test/test_time_math.exe tests/native/test_time_math.c
build/test/test_time_math.exe
