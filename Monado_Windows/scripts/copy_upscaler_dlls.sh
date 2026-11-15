#!/usr/bin/env bash
# [XCDE] Explicit local-only optional SDK copy. No downloads or automatic terms.
set -euo pipefail
cd "$(dirname "$0")/.."
source=${SDK_DIR:-sdk}
destination=${XCDE_RUNTIME_DIR:?Set XCDE_RUNTIME_DIR to the selected installation build directory}
[ -d "$destination" ] && [ ! -L "$destination" ] || { echo "Runtime output must be an ordinary owned directory" >&2; exit 1; }
for name in ffx/PrebuiltSignedDLL/amd_fidelityfx_vk.dll sl/bin/x64/sl.interposer.dll sl/bin/x64/sl.common.dll sl/bin/x64/sl.dlss.dll sl/bin/x64/nvngx_dlss.dll sl/bin/x64/NvLowLatencyVk.dll; do
    file="$source/$name"
    if [ ! -f "$file" ]; then
        echo "Optional provider component unavailable: $file"
        continue
    fi
    [ ! -L "$file" ] || { echo "Refusing a linked SDK binary: $file" >&2; exit 1; }
    target="$destination/$(basename "$file")"
    [ ! -e "$target" ] || { echo "Existing provider preserved: $target (select a fresh build output to update)"; continue; }
    temporary="$target.$$.tmp"
    cp -- "$file" "$temporary"
    cmp -- "$file" "$temporary"
    mv -- "$temporary" "$target"
    echo "Copied local optional provider $(basename "$file"); vendor authenticity/ABI and redistribution review are required separately."
done
