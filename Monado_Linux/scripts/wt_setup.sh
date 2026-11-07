#!/usr/bin/env bash
# Prepare a git worktree of this repo to build and run the game (parallel sessions).
#   scripts/wt_setup.sh [main checkout]   (default: this checkout)
# Junctions (shared, read-only use): targets/ (game dump, 14 GB), .venv/, generated/.
# Copies (private, written by builds/runs): build/obj, build/rt, build/nvn, build/inc, build/xcde.exe, cache/, save/.
# Don't regenerate generated/ from a worktree (it is the main checkout's): ask the main session.
cd "$(dirname "$0")/.."
MAIN="${1:-$(cd "$(dirname "$0")/.." && pwd)}"
[ "$(cd "$MAIN" && pwd -W)" = "$(pwd -W)" ] && { echo "this is the main checkout"; exit 1; }
for d in targets .venv generated; do
  [ -e "$d" ] || cmd //c mklink /J "$(cygpath -w "$PWD/$d")" "$(cygpath -w "$MAIN/$d")" >/dev/null || exit 1
done
mkdir -p build
for d in obj rt nvn inc; do [ -d build/$d ] || cp -r "$MAIN/build/$d" build/; done
[ -f build/xcde.exe ] || cp "$MAIN/build/xcde.exe" "$MAIN/build/xcde.pdb" build/
[ -d cache ] || cp -r "$MAIN/cache" .
[ -d save ] || { [ -d "$MAIN/save" ] && cp -r "$MAIN/save" .; }
echo "worktree ready: $(pwd -W)"
