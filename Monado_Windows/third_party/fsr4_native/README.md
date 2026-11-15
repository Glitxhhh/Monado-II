# Optional native FSR 4.1.2 Lite for Linux

The real Vulkan 12-dispatch INT8 Lite graph uses 27,392-byte weights per set.
The C++ backend is GPL-2.0-or-later, from bbport commit
`0fc2921c45f8ff3900f6bfa43a299e9a4f532af4`, reviewed through Monado Reborn
`3cc9eeb46dc879fe36929d13758754c429b8377b`. Copyright and complete license
texts are retained. The C ABI adapter has the same license; the interface header
is MIT. See [ADR 0013](../../docs/adr/0013-native-fsr4-linux.md) for distribution
terms and the corresponding-source requirement for a combined distribution.

Build the source-only native module from `Monado_Linux`:

```sh
python3 scripts/setup_linux_fsr4.py --build --output .deps/fsr4/build/my-build
```

This uses the project compiler/Vulkan dependencies and emits a verified
`libmonado-fsr4.so`, licenses, corresponding component source and receipt.
It does not download models, require Proton or read any game data.

Optional local model preparation uses the pinned official AMD SDK 2.4 DLL and
reviewed synthetic compute recorder, under its complete SDK terms in
`LICENSE-AMD-SDK.md`. Build the hash-locked recorder/translation tools with
`scripts/setup_linux_fsr4_tools.py`, then run `scripts/prepare_linux_fsr4.py`
with a complete installed Proton build and an X11/XWayland desktop display.
The launcher optional preparation action handles this process. Downloads,
prefixes, shader caches and temporary files stay in the selected owned store;
normal game execution remains native Vulkan. No model/DLL/game data belongs in
the tracked source or normal public release bundle. Preparation emits four
model sets, 48 SPIR-V modules, hash manifests and logs; incomplete output is never
selected. Changing the chosen upscaler remains an explicit user action.

Linux expands the prepass `OpFDot2MixAcc32VALVE` in memory to FP32 conversions,
dot and add, retaining the captured files unchanged. This avoids a mandatory
VALVE mixed-dot extension. Lite does not use compute derivatives or cooperative
matrices. All 48 rewritten modules were structurally validated; actual authored
native/scaled/Ultra dispatch and readback pass on RTX 4070 SUPER and integrated
RADV RAPHAEL. These component checks do not establish all-gameplay coverage.

Runtime capability checks are still required: float16/int16, integer dot,
compute subgroup quad operations, float16 denorm preservation, push descriptors
and the shader image/write limits. Unsupported features or missing/changed
models use the existing fallback. Output is bounded to 3840x2160. A render long
edge below 128 pixels is rejected before recording: the captured exposure graph
produced black output below that boundary in the authored tiny-window test.
Missing assets, allocation/pipeline failures and an invalid API request fail
closed. The caller owns GPU completion and must retire old contexts safely.

Local adaptations include source/hash locks, strict Lite selection, bounds and
argument validation, memory unmapping, portable-dot expansion, the exposure
boundary guard and private preparation/cancellation. Original bbport filenames
and some internal diagnostic strings still refer to 4.1.1 for historical reasons.
