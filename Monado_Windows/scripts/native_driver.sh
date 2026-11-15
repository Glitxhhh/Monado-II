#!/usr/bin/env bash
# [XCDE] One native driver owns compile/link membership and all failure codes.
set -euo pipefail
cd "$(dirname "$0")/.."
python=${MONADO_PYTHON:-.deps/python314/python.exe}
[ -f "$python" ] || python=.venv/Scripts/python.exe
[ -f "$python" ] || { echo "Missing project-local Python; see tools/setup_windows.ps1" >&2; exit 1; }
if [ -n "${XCDE_EXE:-}" ]; then
  echo "XCDE_EXE is retired: pass --output <new build installation root>. Existing builds are preserved." >&2
  exit 2
fi
exec "$python" -B scripts/build_windows.py "$@"
