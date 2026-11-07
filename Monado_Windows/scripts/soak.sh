#!/usr/bin/env bash
# T185 soak run: real time from a save copy, input route, LARECOMP_VK_PROF=1 ([mem] every ~10 s), and every minute the
# process's handle count / private bytes / working set from outside (Get-Process). Stops itself after SECS.
#   SOAK_EXE=build/xcde_soak.exe SOAK_SAVE=saves/colony9_20261019_1235 scripts/soak.sh <route> <secs> <outdir>
# Extra env (XCDE_MEM_MAP=1, XCDE_LOWSPEC_CPUS=4, ...) passes through.
cd "$(dirname "$0")/.."
export PATH="/c/msys64/ucrt64/bin:$PATH"
ROUTE=$1; SECS=${2:-7200}; OUT=${3:-build/soak}
EXE=${SOAK_EXE:-build/xcde_soak.exe}; NAME=$(basename "$EXE" .exe)
mkdir -p "$OUT"
SD=build/save_$NAME; rm -rf "$SD"; mkdir -p "$SD"
[ -n "$SOAK_SAVE" ] && cp -r "$SOAK_SAVE"/. "$SD"/
export LARECOMP_SAVE_DIR=$SD LARECOMP_WINDOW=${LARECOMP_WINDOW:-1280x720} LARECOMP_HIDDEN=1 LARECOMP_NO_AUDIO=1 LARECOMP_VK_PROF=1
for t in 1 2 3 4 5; do
  XCDE_INPUT_REPLAY=$ROUTE "$EXE" targets > "$OUT/game.log" 2>&1 &
  PID=$!
  sleep 20
  grep -aq "not free" "$OUT/game.log" && { kill $PID 2>/dev/null; wait $PID 2>/dev/null; continue; }
  break
done
echo "time_s,pid,handles,private_mb,ws_mb,threads,cpu_s" > "$OUT/proc.csv"
T0=$(date +%s)
while kill -0 $PID 2>/dev/null; do
  now=$(( $(date +%s) - T0 ))
  powershell -NoProfile -Command "Get-Process -Name $NAME -ErrorAction SilentlyContinue | ForEach-Object { '{0},{1},{2},{3:F0},{4:F0},{5},{6:F0}' -f $now,\$_.Id,\$_.HandleCount,(\$_.PrivateMemorySize64/1MB),(\$_.WorkingSet64/1MB),\$_.Threads.Count,\$_.TotalProcessorTime.TotalSeconds }" >> "$OUT/proc.csv"
  [ $now -ge $SECS ] && break
  sleep 60
done
WPID=$(cat /proc/$PID/winpid 2>/dev/null)
kill $PID 2>/dev/null; sleep 3
[ -n "$WPID" ] && taskkill //F //T //PID $WPID >/dev/null 2>&1
echo "soak done after $(( $(date +%s) - T0 )) s" >> "$OUT/proc.csv"
