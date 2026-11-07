#!/usr/bin/env bash
# Drive the game headlessly in deterministic mode through a recorded input route and take window captures.
#   scripts/drive.sh <route.txt> <shot frames, comma separated> [seconds=90] [name=drive]
# Route format: tests/routes/README.md ("<frame> <buttons hex> <lx> <ly> <rx> <ry>"; A = 1, B = 2, X = 4, Y = 8, PLUS = 0x400,
# D-pad left/up/right/down = 0x1000/0x2000/0x4000/0x8000). Captures: build/<name>_<frame>.png (game assets: never commit them).
# The run stops after the last capture. Log: build/<name>.log.
cd "$(dirname "$0")/.."
export PATH="/c/msys64/ucrt64/bin:$PATH"
ROUTE=$1; SHOTS=$2; SECS=${3:-90}; NAME=${4:-drive}
taskkill //F //IM xcde.exe >/dev/null 2>&1
XS=""
for f in ${SHOTS//,/ }; do XS="$XS,$f:build/${NAME}_$f.png"; done
LAST=${SHOTS##*,}
rm -f build/${NAME}_*.png
XCDE_INPUT_REPLAY=$ROUTE XCDE_DETERMINISTIC=1 XCDE_USAGE=build/${NAME}_usage.tsv LARECOMP_HIDDEN=1 LARECOMP_NO_AUDIO=1 \
  XCDE_SHOTS="${XS#,}" timeout "$SECS" build/xcde.exe targets > build/$NAME.log 2>&1
rc=$?
echo "rc=$rc (124 = timeout, 127 = crash with report)"
ls build/${NAME}_*.png 2>/dev/null
grep -a "STRICT\|\[crash\]\|\[stall\]\|FATAL\|UNIMPLEMENTED\|unimplemented" build/$NAME.log | head -8
exit 0
