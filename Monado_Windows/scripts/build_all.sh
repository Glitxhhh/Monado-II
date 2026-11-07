#!/usr/bin/env bash
# Everything from a checked dump in targets/ to a playable build: recompile the game code (generated/), compile it
# and the runtime (build/xcde.exe), build the launcher. "Build Monado.bat" runs it; it also works on its own:
#   scripts/build_all.sh            (XCDE_EXE=build/other.exe: link the game under another name, e.g. for tests)
# Needs: MSYS2 UCRT64 at C:\msys64 with the libraries, and .venv with capstone unicorn lz4 ziglang (see README).
set -e
cd "$(dirname "$0")/.."
step() { echo; echo "=== $* ==="; }
[ -x .venv/Scripts/python ] || [ -f .venv/Scripts/python.exe ] || { echo "missing .venv (run \"Build Monado.bat\" or see the README)"; exit 1; }
step "1/5 Checking the game files (targets/)"
.venv/Scripts/python -X utf8 tools/extract/check_dump.py targets
step "2/5 Recompiling the game code into C (generated/, ~1 min)"
(cd tools && ../.venv/Scripts/python -X utf8 gen_all.py --out ../generated)
step "3/5 Compiling the game code (build/obj/, several minutes the first time)"
XCDE_SKIP_BOOT_LINK=1 scripts/build_boot.sh
step "4/5 Compiling the runtime and linking ${XCDE_EXE:-build/xcde.exe}"
scripts/build_rt_gfx.sh
exe=${XCDE_EXE:-build/xcde.exe}
[ -f "$exe" ] || { echo "the link failed (see build/link_gfx.err)"; exit 1; }
step "5/5 Building the launcher (build/xcde_launcher.exe)"
tools/launcher/build.sh
echo
echo "Done: $exe and build/xcde_launcher.exe. Start the game with \"Play Monado.bat\" (the first time: Precompile shaders)."
