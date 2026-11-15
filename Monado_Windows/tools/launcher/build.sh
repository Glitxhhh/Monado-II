#!/usr/bin/env bash
# [XCDE] Fresh native launcher output; compile/link errors propagate unchanged.
set -euo pipefail
exec bash "$(dirname "$0")/../../scripts/native_driver.sh" --stage launcher "$@"
