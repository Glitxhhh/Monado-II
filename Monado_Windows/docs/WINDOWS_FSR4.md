# Optional native Windows FSR4 Lite development component

The native AMD64 PE Vulkan component and checked renderer dispatch path exist.
Source-only build/ABI, receipt, capability and failure fixtures are separate from
real model preparation, GPU output and gameplay qualification. The normal game
keeps its existing Windows Streamline/FidelityFX providers. FSR4 is optional;
no model/component setting or frame generation is enabled by default.

The reviewed backend is GPL-2.0-or-later, bbport commit
`0fc2921c45f8ff3900f6bfa43a299e9a4f532af4`, reviewed through Monado Reborn
`3cc9eeb46dc879fe36929d13758754c429b8377b`. Windows adaptations retain the full
GPL and AMD SDK terms in `third_party/fsr4_native`, use Unicode paths and restrict
model reads to the receipted Lite graph. The MIT API header does not replace the
component licence. Combined binary distribution requires applicable GPL terms
and corresponding combined source/build scripts; dynamic linking is no exemption.

From `Monado_Windows`, build with the project's pinned Python/Zig/Vulkan tools:

```powershell
python tools/setup_windows_fsr4.py --build --output "$PWD/.deps/fsr4/build/my-component"
```

This writes a native DLL, SHA256 sidecar, component-source archive, log and
receipt. It reads no game input and downloads nothing. Existing output is never
reused, overwritten or cleaned. Source-lock mismatch fails before compilation.

Explicit import of already prepared local models is available:

```powershell
python tools/setup_windows_fsr4.py --import-assets "C:/My local prepared assets" --output "$PWD/.deps/fsr4/assets/my-models"
```

Import requires a passed receipt for the exact accepted SDK/capture identity and
all four Lite sets: t1080_m0, t1080_m1, t2160_m0, t2160_m1. Each has twelve bounded
aligned SPIR-V files and 27,392-byte weights. Exact membership, identity, sizes,
hashes and SPIR-V magic are checked under ordinary-file leases. Output is fresh
and owned; completion identity is written last. Arbitrary self-authored receipts
are not vendor authentication, and shader headers do not establish GPU output.
Models remain local data, absent from source/public packages.

Supply both explicit local paths to the native launcher:

```powershell
build/xcde_launcher.exe --profile-root "C:/My profile" --fsr4-assets "C:/My native assets" --fsr4-component "C:/My component/monado-fsr4.dll"
```

Then select FSR4 Lite. Runtime checks the actual current-user-owned component,
hash, ABI and every model file/hash. Model files and ancestors remain leased
against writes/deletion. No ambient model ENV, ELF adapter or Proton prefix is
used. Missing/changed resources, unsupported capabilities or context/build/record
failure retain FSR3/spatial fallback and a reason. Required capabilities include
FP16/Int16/integer-dot, compute subgroup quad, FP16 denorm, eleven push descriptors
and workgroup/storage limits. The tiny107x60 Ultra failure guard is **render long
edge >=128**, including valid128x60 input. Lite linear output is encoded before
HUD/FG composition; native Windows FFX3 fallback retains its gamma contract.

The following Windows preparation work remains **unimplemented/unqualified**:

- Native graphical capture/translation worker, phase progress, cancellation,
  retry and selection broker.
- Reviewed local vkd3d-proton/DXVK capture support, recorder/AMD extension shim,
  patched pinned dxil-spirv and SPIR-V validation tool builds.
- Pinned AMD SDK synthetic recorder execution, four real captures, translation
  and structural validation of all48 shaders, and atomic promotion after native
  GPU finite/detail/reset/sharpening/readback checks.
- Actual prepared-model dispatch on NVIDIA/AMD, gameplay, reset/resize/reboot
  and combined FSR4/frame-generation qualification.

The disabled preparation action explains missing tools. It downloads no model,
reports no fake preparation, changes no selection and reads no game input.

# NVIDIA Smooth Motion on Windows

[NVIDIA's official instructions](https://nvidia.custhelp.com/app/answers/detail/a_id/5621)
configure Smooth Motion per compatible program in NVIDIA App, including Vulkan.
The reviewed [public NVAPI driver settings header](https://github.com/NVIDIA/nvapi/blob/main/NvApiDriverSettings.h)
exposes no Smooth Motion control. This launcher offers a help link only, does not
map the Linux layer/environment request to Windows support, change global settings
or install a driver. Actual activation belongs to the driver. Generated/output
identity is unavailable from a supported native API; no multiplied/subtracted FPS
estimate is used. Disable integrated generation when using external generation.
