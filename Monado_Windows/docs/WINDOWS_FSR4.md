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

Native preparation sources and CLI are implemented in
`tools/setup_windows_fsr4_tools.py`, `tools/prepare_windows_fsr4.py` and
`tools/fsr4/validate_windows.cpp`. They use the accepted recorder, Lite graph
rules and exact five DXIL/SPIR-V source archive pins. The recorder and AMD
extension shim are native Win32 code with Unicode, ordinary-file operations and
explicit module-local DLL loading. The portable Vulkan readback fixture uses a
native checked DLL loader. Source identities are separate from the compatible
model graph identity in `fsr4-preparation-windows.lock.json`.

Build the tools from local verified source archives and explicitly selected
CMake, Ninja and Zig executables. `--translator-only` builds the recorder, shim,
patched DXIL translator, SPIR-V tools and native GPU validator without requiring
vendor/model assets or D3D capture support:

```powershell
python -I -S -B tools/setup_windows_fsr4_tools.py --build --translator-only --archives "C:/Local pinned archives" --cmake "C:/Local CMake/bin/cmake.exe" --ninja "C:/Local Ninja/ninja.exe" --compiler "C:/Local Zig/zig.exe" --vulkan-prefix "C:/Local native dependencies" --output "C:/Owned tools/new-translator-build"
```

Full capture tool builds omit `--translator-only` and add `--support` pointing
to a local folder containing ordinary current-user-owned `d3d12.dll`,
`d3d12core.dll` from vkd3d-proton and `dxgi.dll` from DXVK. System D3D12 is not a
substitute for the recorder's shared-vtable contract. [Upstream documents its
Windows DLL path for development/testing](https://github.com/HansKristian-Work/vkd3d-proton#using-vkd3d-proton). `support.json` must identify
schema1, kind `local-vkd3d-dxvk`, a `provenance` string and exact `files` entries
for those three names, each with `bytes` and `sha256`. The resulting receipt
records the supplied provenance and binary hashes. This local receipt is not a
publisher signature or a claim that arbitrary support binaries were qualified.
The tools builder downloads and installs nothing, accepts no vendor terms and
never modifies a driver or host installation. A translator-only receipt cannot
enable capture preparation.

The native preparation CLI requires that complete tools receipt, an ABI2
component build receipt and the exact local AMD SDK DLL pinned in the lock:

```powershell
python -I -S -B tools/prepare_windows_fsr4.py --prepare --sdk "C:/My supplied AMD SDK/amd_fidelityfx_upscaler_dx12.dll" --tools "C:/Owned tools/complete-build" --component "C:/Owned component/monado-fsr4.dll" --output "C:/Owned models/new-preparation"
```

The pipeline restricts a private SDK copy to Lite, records four fixed synthetic
cases and checks the twelve-pass dispatch order, group counts, tensor layouts
and frame constants. It translates all48 shaders with class bindings and
validates each for Vulkan1.3. The native offscreen validator then exercises all
four model sets for33 real component frames, checking finite output, detail,
reset, sharpening and GPU readback. Unsupported capabilities, shader/graph
mismatch, failed dispatch or failed readback fail preparation. Only actual
successful validator execution can set `gpu_qualified` and permit completion.
These authored images establish component behavior; they do not qualify game
scenes, frame generation, WSI, performance or every driver.

Children start suspended, enter a private kill-on-close Windows Job, then run.
Only their log/NUL handles are inherited. Bounded live progress, timeouts and an
optional `--cancel-file` support the asynchronous worker. Cancellation stops only
owned descendants, retains failed private output and never changes the selected
upscaler or previous model selection. Retrying uses a new directory. Successful
output contains exact52 resource hashes/sidecars, its recipe/command receipts and
`models.id` written last. No game data is read. Private SDK copies, captures,
translated shaders, logs and models are never source/package resources.

The source-only recorder, shim and GPU validator compile as native Windows
components. The pinned native translator build has also compiled and linked
all211 targets and passed its exact eight-PE hash/import closure check: recorder,
shim, DXIL translator and its authored shared library, three SPIR-V tools and
GPU validator. Its translator-only receipt explicitly lists the three missing
capture-support DLLs; full preparation rejects that partial toolset.
Authored fixtures exercise literal Unicode/spaces/quotes, live
progress, failure, timeout, cancellation with an unrelated process surviving,
source locks and exact patch boundaries. Actual pinned translator building and
full model preparation have separate receipts; a compiled validator does not
establish GPU qualification. The graphical Build preparation tools action builds
both these tools and the native ABI2 component from selected local pinned inputs,
then commits their verified paths together. Prepare models separately uses an
explicit supplied SDK; progress/cancel/retry preserve the selected upscaler and
previous preparation on failure. Real SDK/support captures, actual prepared-model
NVIDIA/AMD output, gameplay, resize/reboot and combined frame generation remain
unqualified until their corresponding native runs succeed.

# NVIDIA Smooth Motion on Windows

[NVIDIA's official instructions](https://nvidia.custhelp.com/app/answers/detail/a_id/5621)
configure Smooth Motion per compatible program in NVIDIA App, including Vulkan.
The reviewed [public NVAPI driver settings header](https://github.com/NVIDIA/nvapi/blob/main/NvApiDriverSettings.h)
exposes no Smooth Motion control. This launcher offers a help link only, does not
map the Linux layer/environment request to Windows support, change global settings
or install a driver. Actual activation belongs to the driver. Generated/output
identity is unavailable from a supported native API; no multiplied/subtracted FPS
estimate is used. Disable integrated generation when using external generation.
