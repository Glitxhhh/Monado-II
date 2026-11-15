# Windows parity source provenance

The native Windows runtime retains the inherited LASRecomp and Monado MIT notices.
This ledger records the native development transfer from the maintainer's
accepted Linux dev103 + dev104 R5 source already present in this repository.
The exact accepted input inventory is `docs/WINDOWS_PARITY_INVENTORY.json` at
repository root. The Linux implementation is preserved as the reference.
This document records provenance, not a complete-parity or release claim.

## Gameplay, state and services

`games/xcde/patches.py`, the checked lifter patch helpers and `runtime/hle/`
adapt the accepted combat/rewards, expanded saves, Quickstep, desktop exit,
fractional frame thresholds, aspect/graphics, foliage, input and rumble behavior.
The accepted pure slot/picker/statistics/title models retain their original
MIT notices. Optional features retain the accepted disabled defaults.

`kernel.c`, `ipc.[ch]`, `cpu.h`, `rt.h` and the AM/HID/filesystem/misc services
carry the accepted synchronization, envelope validation, lifecycle and stop/reset
behavior. Windows keeps native address space, process and thread handling.
`platform/windows_save_{io,dir}.h` are authored Windows adapters: Unicode paths,
ordinary single-link files, checked ancestors, native sharing, flushed writes
and write-through replacement replace POSIX assumptions. Save journaling keeps
paired Quickstep metadata and refuses an inaccessible or invalid journal.

`hle/fibers.[ch]` and `platform/guest_fp_windows.h` are authored native repairs.
Windows fibers retain floating-point state and checked TLS/guest-thread handover.
Nonlocal kernel unwind first returns to the original native root fiber before
`longjmp`; no cross-stack jump or POSIX context conversion is used.

`platform/windows_fma.c` adapts the generic musl FMA implementation bundled with
the pinned Zig compiler, retaining its MIT and FreeBSD notices and exact upstream
hashes in `third_party/musl_math/`. Native differential testing exposed finite
inputs returning NaN through Windows UCRT at the baseline CPU target. The local
correctly rounded implementation also preserves the sign of an underflowed
product with zero addend. It retains the baseline CPU requirement and strict FP
policy and is linked into both the authored runtime and local game-code DLL.

## Configuration, local game library and build boundary

The accepted settings schema and quality/GPU/aspect policies are adapted in
`settings.h`, the policy headers, `host/settings_ini.c`, the launcher and F1 UI.
`host/settings_transaction_windows.h` preserves unknown keys through a native
locked, flushed same-directory replacement rather than deleting old settings.

`gamelib.[ch]`, `host/main_boot.c` and `host/gamelib_entry_windows.c` adapt R5's
authored-runtime/local-game-code ABI. Windows checks an owned ordinary local PE
companion, strict identity and SHA256 sidecars before mapping it with restricted
Unicode DLL search. Startup loads the library before workers, modules, kernel or
dispatch. `platform/windows_library_identity.h` uses the Windows CNG SHA256 API;
the local hash receipt detects a changed library and is not a publisher signature.
The static full-source development fallback remains available.

`scripts/build_windows.py`, `tools/windows_deps.py`, the pinned tool/dependency
locks and the native BAT/PowerShell fallbacks replace inherited unchecked shell
commands with explicit source/object membership, checked owned child processes,
fresh outputs and immutable content receipts. Dependency licensing is recorded
in `NOTICE.md`; a successful local build is not authorization to redistribute its
dependency closure or any generated game code.

## Launcher presentation and portable resources

The launcher/F1 layout, licensed DejaVu fonts and reviewed illustration are
transferred from the accepted Linux launcher. Windows copies have exactly the
same pinned bytes in `publication-policy.json`, with the font license and artwork
provenance beside them. `theme_resource.py` emits build-local authored resource
includes. Native process/argument and report ZIP helpers retain Windows paths
and process ownership and do not upload reports.

## Immutable shader registration and binding

`runtime/nvn/{nvn.h,objects.c,cmdbuf.c,program_binding.h}` and the corresponding
program, constant-buffer and graphics/compute cache paths in `nvn/vulkan/exec.c`
adapt the accepted LINUX-046 fixes. Code, compiler constants, control metadata,
command stage masks and queued pipeline metadata retain one immutable registration.
Windows platform, provider, host-memory import and process code remain native.

## Scaled gathers, optional bloom and filtering

The MIT bloom/scaled-gather helpers were adapted by the Linux reference from
Monado-Reborn commit `3cc9eeb46dc879fe36929d13758754c429b8377b`, retaining
Peiikai/Monado contributors' attribution. The accepted LINUX-103 implementation
adds bounded immutable source/compiler identities and native textureGather scalar
taps preserving base-level behavior when minLod is nonzero. The Windows transfer
retains those semantics and the unity-scale path. Optional bloom stays Original.

`sampler_filter_policy.h`, production sampling and its bounded immutable cache
retain accepted LINUX-103 eligibility and GPU-idle ownership. Native Windows
records the actually enabled samplerAnisotropy device feature before permitting
an override. Existing guest samplers retain their ownership.

Native authored fixtures exercise registration, allocator callbacks, stage binds,
constant resolution, program/compute cache identity, bloom choices, translated
gather eligibility and sampler cache bounds/lifetime. These component checks do
not establish GPU pixel correctness, provider/gameplay parity or performance.
Native game/launcher build and physical startup evidence is recorded separately.

## Native presentation qualification

Windows keeps FIFO V-Sync and the configured game frame limiter. Its default
does not multiply presentations by SDL's nominal monitor refresh: the Windows
compositor/driver can pace a Vulkan swapchain differently from that nominal mode,
especially after moving between displays. The accepted Linux repeat policy is
retained on Linux. Explicit legacy repetition remains a diagnostic override on
Windows. No game frame, effect or simulation step is omitted by this adaptation;
accepted presentation calls are not claimed to measure physical scanout.

## Native fused arithmetic

`runtime/platform/windows_fma.c` adapts the musl generic `fma` / `fmaf` bundled
with pinned Zig 0.14.1. `third_party/musl_math/PROVENANCE.json` records exact
original hashes and the narrow adaptations; its COPYRIGHT and the retained
FreeBSD notice cover the incorporated source. Native differential fixtures
found Windows UCRT `fma` returning NaN for finite operands at the supported
x86-64-v2 baseline. Runtime and locally built game DLL each link this helper
so compiler-lowered fused operations keep correct rounding without requiring
new CPU features. The zero-addend path preserves an underflowed product's sign.

## Temporal, feedback and aspect correctness

The accepted temporal history/camera/math/GLSL and compositor decisions were
adapted in `temporal.c`, `temporal_camera.h`, `shaders.c`, `exec.c` and `textures.c`.
The Windows Streamline and FidelityFX DLL/API paths remain native; Linux syscall,
NGX and model-preparation adapters were not substituted. World-space camera cuts,
finite motion inputs, reactive HUD separation, successful evaluation continuity,
zero sharpening and complete feedback snapshots retain the accepted policies.
Native feedback-loop use remains disabled pending a qualified feature enable;
the complete per-subresource snapshot fallback is active.

`nvn/vulkan/host_import.[ch]` transfers the accepted portable LINUX-005 checked
Vulkan import contract: external-buffer capability and pointer/buffer memory-type
intersection, coherent host-visible storage, aligned allocation, dedicated
requirements, checked binding and complete failed-import cleanup. The native
`host_import_windows.inc` lifecycle retires recorded and submitted work before
heap growth replaces the import; reset releases the import before guest-page
reset. `host_import_memory_windows.h` validates and commits only the checked
prefix of the existing private VirtualAlloc reservation without remapping it or
changing protected pages. Unsupported/failed global memory never supplies dummy
zeros. Linux RADV/dma-buf/placed-mmap backing remains Linux-specific. Native
authored tests separately cover controlled Vulkan failures, actual VirtualAlloc
lifetime and real NVIDIA Vulkan coherent alias transfers; these do not establish
full gameplay, provider or performance qualification.

The accepted LINUX-017/094 lifecycle order is retained in the native executor:
drain queued rendering and native SDK presents, wait for the GPU, clear dynamic
resolution/FPS/cap history, invalidate borrowed executor/provider references,
then destroy textures. Non-game clears also invalidate game/repeated-output
classification. The bounded LINUX-093 aspect output transition diagnostics are
retained on the render owner thread.

`ui/window_windows.h` and the strict launch-monitor prefix adapt initial display
selection to the launcher's native monitor device. Windowed mode restores a
decorated resizable client fitted inside that monitor's work area; fullscreen
transitions verify the actual SDL/native state and report failure without
pretending that a request succeeded. Hidden authored SDL/Win32 windows exercise
both real displays, failed transitions and saved-size restoration without game
input or human-profile mutation.

## Launcher and FPS controls

The accepted ImGui theme, reviewed embedded artwork/fonts, quality controls and
FPS display were adapted with native UTF-16 process paths, explicit owned child
handles, atomic INI writes preserving unknown keys, and bounded local-only support
ZIPs. Font/art licenses and provenance remain adjacent to the copied resources.
Unimplemented Linux-only update/model-preparation components are not advertised
as working Windows features.

The Windows native FG adapter retains the FidelityFX swapchain. Match display
publishes a cap only after valid current preparation and recent successful native
interpolation dispatch. Output FPS uses the SDK-reported total; generated-only
FPS is unavailable because this SDK boundary supplies no accepted-output identity.
Callbacks and reset epochs use atomic state. Provider configuration/preparation
failure disables generation; failed disable is fatal instead of reusing stale
inputs. These authored component checks do not qualify real SDK/GPU gameplay.

Native presentation verifies preparation belongs to the current rendered frame,
including when spatial scaling bypasses temporal reconstruction. Context changes,
swapchain recreation and shutdown use the native SDK's checked
`ffxDispatchDescFrameGenerationSwapChainWaitForPresentsVK` boundary before releasing
resources; a GPU-idle call alone does not drain future work owned by a host
presentation worker. The descriptor retains the bundled SDK 1.1.4 ABI documented
in AMD's [FSR 3.1 integration overview](https://gpuopen.com/presentations/2024/FidelityFX_Super_Resolution_3-1_Release-Overview_and_Integration.pdf).

## Native Windows presentation correction

The inherited nominal-refresh repeat loop was reproduced at 10 real FPS on a
focused 179.88-Hz mixed-display Windows desktop: six duplicate FIFO submissions
per requested 30-FPS game frame produced only about 60 accepted outputs/second.
The same authored binary with repetition disabled restored about 30 real FPS and
removed the slot wait at the same window size and monitor. Windows now presents
once per real frame by default and leaves repetition to its compositor. Explicit
`XCDE_REPEAT_PRESENT=1` retains the inherited diagnostic path. No game effects or
guest time steps are skipped, and accepted API requests are not physical scanout.

## Portable native setup and checked Windows updates

The native Win32 frontend/payload reader, persistent backend and local source
export adapt the accepted builder/input-recovery/update policies with Windows
handles, named maintenance leases, owned job cancellation and Unicode argument
arrays. No Linux executable, namespace, system package manager or temporary
interpreter path is substituted. Hash-pinned original Python/tool archives and
the successful native dependency receipt define the private setup payload.

`tools/update_windows_release.py` uses the established official HTTPS metadata
transport but requires a separate Windows channel/version/source archive and
runs the installed publication guard before candidate code. Runtime and stable
profile roots remain separate. `tools/update_windows_retention.py` adapts the
accepted current-only policy using exact current-user handle identities,
registry-bound snapshot hashes and no-follow junction-object deletion; unknown,
modified, locked and live trees are deferred. No rollback selection is added.

`tools/game_library_reuse_windows.py` verifies the actual previous local PE DLL,
sidecars, immutable receipt, canonical generator/guest closure, compiler/input/
ABI and host function/data contract without executing it. A verified intact
compatibility mismatch permits a new source build; corruption is a failure.
Native fixtures use authored PEs/data and disposable profiles. Actual SDK/GPU
dispatch, interactive setup, gameplay and full DE/FC remain separate qualification.

## Optional native Windows FSR4 Lite foundation

`third_party/fsr4_native` retains the accepted GPL-2.0-or-later backend and full
AMD SDK terms, with narrow Unicode-path and fixed reviewed-postpass adaptations.
`fsr4-windows.lock.json` records source/model identity. Optional native DLL build,
local prepared-asset import, immutable model/DLL leases, capabilities and temporal
dispatch preserve Windows providers/fallback. Component fixtures do not qualify
real models/GPU/gameplay. Native preparation sources now include the accepted
GPL recorder/trace translator and MIT AMD extension shim, narrow Win32 Unicode/
module-local I/O adaptations, pinned local native tool builds, owned suspended
child Jobs, cancellable fixed synthetic capture stages and a portable Vulkan
readback validator. The separate preparation lock records exact source/tool
identities. Real SDK/support execution and actual model/GPU/gameplay qualification
require separate native evidence; see [Windows FSR4](WINDOWS_FSR4.md).
The native source-only build compiled all211 pinned translator targets and passed
the exact eight-PE translator-only closure, with the three unavailable local
capture-support DLLs declared explicitly. Full preparation still requires them.
Smooth Motion offers an official NVIDIA App help link only; no Linux enable
request or generated-frame counter is presented as native Windows support.

## Native shader preparation and cache transactions

The accepted checked discovery/metadata/counters/job-joining behavior is adapted
to native Windows session cancellation, owned child supervision and Unicode
ordinary-file transactions. Native SPIR-V repair/flush and pipeline/variant cache
guards retain compatible local work without shipped seeds or permanent crash
blacklists. Native fixtures use authored containers/GLSL and disposable files;
game/GPU coverage remains separate. See [shader preparation](WINDOWS_SHADER_PREPARATION.md).
