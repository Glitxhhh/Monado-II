#!/usr/bin/env bash
# Real-time measurement run in its own folder (its own cache/, save copy, logs), e.g. cold-cache first runs (T187).
#   RT_EXE=build/xcde_pc1.exe RT_SAVE=saves/colony9_20261019_1235 [RT_CACHE=<cache dir to start from>] \
#     scripts/rt_run.sh <route> <secs> <outdir>
# The run's cwd is <outdir> (a junction to targets/ is made there), so cache/ starts empty unless RT_CACHE is given.
# LARECOMP_LOG_PACE=1 always (frame periods); __GL_SHADER_DISK_CACHE_PATH=<outdir>/nvcache (the driver's own cache
# starts empty too). RT_SHOTS=<frames>: deterministic captures instead. Extra env passes through. Prints the summary of scripts/rt_summary.py at the end.
cd "$(dirname "$0")/.."
REPO=$(pwd -W)
export PATH="/c/msys64/ucrt64/bin:$PATH"
ROUTE=$(cygpath -w "$(realpath "$1")"); SECS=${2:-420}; OUT=$3
EXE=$(cygpath -w "$(realpath "${RT_EXE:-build/xcde.exe}")")
rm -rf "$OUT"; mkdir -p "$OUT/save" "$OUT/nvcache"
[ -e "$OUT/targets" ] || powershell -NoProfile -Command "New-Item -ItemType Junction -Path '$(cygpath -w "$OUT")\targets' -Target '$(cygpath -w "$REPO/targets")' | Out-Null"
[ -n "$RT_SAVE" ] && cp -r "$RT_SAVE"/. "$OUT/save"/
[ -n "$RT_CACHE" ] && cp -r "$RT_CACHE" "$OUT/cache"
[ -n "$RT_NVCACHE" ] && cp -r "$RT_NVCACHE"/. "$OUT/nvcache"/  # (the driver cache a precompilation filled)
cd "$OUT"
export LARECOMP_SAVE_DIR=save LARECOMP_WINDOW=${LARECOMP_WINDOW:-1280x720} LARECOMP_HIDDEN=1 LARECOMP_NO_AUDIO=1 LARECOMP_LOG_PACE=1
export __GL_SHADER_DISK_CACHE_PATH="$(cygpath -w "$PWD/nvcache")"
# RT_SHOTS=<frame>,<frame>,...: a deterministic run instead (XCDE_DETERMINISTIC=1), shot_<frame>.png, exits after the last
if [ -n "$RT_SHOTS" ]; then
  XS=""; for f in ${RT_SHOTS//,/ }; do XS="$XS,$f:shot_$f.png"; done
  export XCDE_DETERMINISTIC=1 XCDE_EXIT_AFTER_SHOTS=1 XCDE_SHOTS="${XS#,}"
fi
for t in 1 2 3; do
  XCDE_INPUT_REPLAY="$ROUTE" timeout "$SECS" "$EXE" targets > game.log 2>&1
  grep -aq "not free" game.log || break
done
cmd //c rmdir targets
"python" "$REPO/scripts/rt_summary.py" game.log
