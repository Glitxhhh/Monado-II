#!/usr/bin/env bash
# [XCDE] Runtime DLLs of the temporal upscalers next to build/xcde.exe (not in git: vendor binaries, ~60 MB):
# AMD FidelityFX SDK 1.1.4 (FSR 3.1, signed prebuilt) and NVIDIA Streamline 2.14.16 (DLSS). Downloaded and
# signature-checked into sdk/ (default); SDK_DIR=<dir> for another location. Get them from AMD / NVIDIA yourself.
cd "$(dirname "$0")/.."
S=${SDK_DIR:-sdk}
for f in "$S/ffx/PrebuiltSignedDLL/amd_fidelityfx_vk.dll" "$S/sl/bin/x64/sl.interposer.dll" "$S/sl/bin/x64/sl.common.dll" \
         "$S/sl/bin/x64/sl.dlss.dll" "$S/sl/bin/x64/nvngx_dlss.dll" "$S/sl/bin/x64/NvLowLatencyVk.dll"; do
  b=build/$(basename "$f")
  [ -f "$b" ] && [ ! "$f" -nt "$b" ] && continue
  cp -f "$f" "$b" 2>/dev/null && echo "copied $(basename "$f")" || echo "missing $f (that upscaler will be unavailable)"
done
