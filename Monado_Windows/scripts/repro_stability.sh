#!/usr/bin/env bash
# Repro loop for T067 (intermittent hang) / T108 (intermittent crash while loading).
#   scripts/repro_stability.sh [runs=30] [seconds=30] [outdir=build/rep]   (extra env vars pass through to the game)
# Classifies each run: OK (timeout reached, frames still advancing), HANG (frame counter stalled before the
# timeout), CRASH (exit code != 124). Logs: <outdir>/run_NN.log
cd "$(dirname "$0")/.."
export PATH="/c/msys64/ucrt64/bin:$PATH"
N=${1:-30}; SECS=${2:-30}; OUT=${3:-build/rep}
mkdir -p "$OUT"; rm -f "$OUT"/run_*.log "$OUT"/summary.txt
ok=0; hang=0; crash=0
for i in $(seq -w 1 "$N"); do
  log="$OUT/run_$i.log"
  LARECOMP_HIDDEN=1 LARECOMP_FPS_INTERVAL=1 timeout "$SECS" build/xcde.exe targets > "$log" 2>&1
  rc=$?
  last=$(grep -a '^\[vk\] t=' "$log" | tail -1 | sed -E 's/\[vk\] t=([0-9.]+) frame ([0-9]+).*/\1 \2/')
  lt=${last% *}; lf=${last#* }
  if [ "$rc" -ne 124 ]; then cls=CRASH; crash=$((crash+1))
  elif [ -z "$lt" ] || awk "BEGIN{exit !($lt < $SECS - 6)}"; then cls=HANG; hang=$((hang+1))
  else cls=OK; ok=$((ok+1)); fi
  echo "run $i rc=$rc $cls last_vk_t=${lt:-none} frame=${lf:-none}" | tee -a "$OUT/summary.txt"
done
echo "TOTAL ok=$ok hang=$hang crash=$crash" | tee -a "$OUT/summary.txt"
