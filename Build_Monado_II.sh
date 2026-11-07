#!/usr/bin/env bash
# Build locally from the player's extracted input. Never run the game.
set -euo pipefail
monado_source_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
if ! command -v python3.14 >/dev/null 2>&1; then
    printf '%s\n' "Monado II needs Python 3.14. Install your distribution's python3.14 and venv support, then run this script again." >&2
    exit 1
fi
exec python3.14 -I -B "$monado_source_root/tools/build_linux_app.py" "$@"
