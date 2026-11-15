#!/usr/bin/env bash
# [XCDE] Safe native fallback; no ambient object globs or process-name kills.
set -euo pipefail
if [ -n "${XCDE_SKIP_BOOT_LINK:-}" ]; then
  exec bash "$(dirname "$0")/native_driver.sh" --stage generated --generated "${XCDE_GENERATED:-generated}" "$@"
fi
exec bash "$(dirname "$0")/native_driver.sh" --stage boot --generated "${XCDE_GENERATED:-generated}" "$@"
