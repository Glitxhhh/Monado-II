#!/usr/bin/env bash
# [XCDE] Safe native fallback; no ambient object globs or process-name kills.
set -euo pipefail
exec bash "$(dirname "$0")/native_driver.sh" --stage all --generated "${XCDE_GENERATED:-generated}" "$@"
