> Public snapshot: detailed local engineering evidence is retained
> privately; evidence links below lead to the public coverage summary.

# Linux change ledger for inherited source

The supplied archive did not contain `docs/UPSTREAM.md`, despite references to it in its contribution guide and notices. This file begins the Linux ledger; it does not reconstruct or certify the author's missing historical ledger.

Source provenance: the user-selected 547-file snapshot is recorded in project-root `docs/linux_port/source_snapshot_20261104.json`. Its notices attribute the generator/runtime base to LASRecomp commit `136f679` under MIT. No upstream checkout or Git history is bundled or assumed.

LINUX-001/002 initially added local packaging safeguards and a Linux component-test script without changing runtime/lifter behavior. Subsequent changes follow below; preserve `[XCDE]` annotations.

## November 4, 2026 — LINUX-002/003

| Inherited file | Linux change and purpose |
|---|---|
| `runtime/hle/fibers.c` | Preserve Windows branch; add guarded native Linux contexts, synchronized transfer/migration, destination thread fields, FP/unwind cleanup and reset. New `fiber_context_linux.{c,h}` and `fibers.h` support this implementation. |
| `runtime/kernel.c`, `runtime/rt.h` | Clean up native roots after guest return/unwind and reclaim fibers after joins; atomic cooperative stop flag, destination TLS accessor barriers and dispatch of the free deterministic baton during blocked shutdown; LP64 trace arguments; bounded Linux diagnostics return an explicit unavailable result when thread-list lock cannot be acquired, avoiding an unlocked walk during reset. |
| `runtime/platform/linux.c`, `runtime/platform.h` | Complete re-exec arguments, checked executable paths/process handles, interrupted waits, affinity/thread names, x86-64-v2 CPU preflight, per-thread alternate signal-stack ownership/cleanup and reliable interrupted PC reporting. |
| `runtime/host/main_boot.c` | Conditional Windows startup/export code, bounded same-PID Linux re-exec, pthread periodic watchdog with checked intervals. |
| `runtime/diag/watchdog.c` | Linux pthread/monotonic reporting and checked stall intervals; Windows branch retained. |
| `tests/native/test_fibers.c` | Preserve Windows tests; add Linux migration/register/FP/stack/lifecycle/death coverage. |
| `NOTICE.md` | Identify the GPL-enabled FFmpeg binaries actually tested in the local Linux development sysroot; preserve original source and Windows dependency attributions. |

New authored Linux test fixtures, focused runner, dependency lock and setup script
are covered by the repository license. No game-derived source/shaders or external
backend implementation was copied into this patch. glibc synchronous contexts and
installed/local libraries supply the platform APIs. Exact evidence and limits are
recorded in root `docs/linux_port/2026-11-04-linux-platform.md`.

Canonical current state lives only in project-root `PROJECT_MEMORY.md`.

## November 4, 2026 — LINUX-002/004/005/006

| Inherited file | Linux change and purpose |
|---|---|
| `runtime/paths.c`, `paths.h` | Explicit validated input/state roots, safe output creation, isolated defaults and refusal to delete shared input; old fixed probe overwrite removed. |
| `runtime/host/main_boot.c` | Initialize paths/write guard before platform logs, worker startup and precompile; checked game suffix paths. |
| `runtime/platform/linux.c`, `platform.h` | State-only logs and real Linux memory residency/map diagnostics with explicit Linux semantics. |
| `runtime/kernel.c` | Atomic heap-size publication to the asynchronous renderer; guest region synchronization unchanged. |
| `runtime/nvn/vulkan/exec.c` | Checked shared host-memory import, compatible coherent memory and explicit failure; retire imports only after render-worker flush and before heap remap; Linux memory labels. |
| `runtime/nvn/vulkan/precompile.c` | Checked POSIX 64-bit file extent and input suffix validation before output writes. |
| `runtime/nvn/vulkan/dlss.cpp`, `temporal.c` | Retain Windows implementation; report DLL-backed optional modes unavailable on Linux. |
| `runtime/ui/menu_vk.cpp`, `runtime/host/settings_ini.c` | Honest Linux options/fallback notices, preserved C linkage and embedded menu font; no false vendor-success result. |
| `runtime/services/kbicons.c`, `runtime/services/fs.c` | Native font/cache support and fail-before-write save mount truncation checks. |
| `scripts/platform_tests_linux.py`, native platform/boot tests | Real path/guard integration, pipe capture, compiler baseline handling, enabled assertions and memory observations. |
| `graphics-deps-linux.lock.json`, `NOTICE.md` | Pin/attribute local zlib dependency; preserve existing license distinctions. |

New authored files include build/CLI/resource/path/GPU runners, Landlock state
write guard, host-import helper, graphics options and focused synthetic fixtures.
No external backend implementation, game source/assets or translated shaders
were copied into tracked source. Source-level render ownership checks complement
actual helper/device tests; they do not establish live renderer integration.
[Detailed evidence](../../docs/PLATFORM_STATUS.md)
records accepted build, failed hypotheses, exact commands and missing coverage.


## November 4, 2026 — LINUX-002/007

| Inherited file | Linux change and purpose |
|---|---|
| `tools/lifter/test_simd.py` | Correct source root, restore binary differential harness, verify complete outputs/failures, bounded exclusive evidence directories/commands/hashes; fix same-base/index synthetic store addresses. No emitted game semantics changed. |
| `runtime/cpu.h`, `platform.h`, `platform/linux.c`, `kernel.c` | Establish and check host FP baseline at fresh Linux guest pthread entry; preserve saved fiber environments. Document portable accessors' role. Exact noninteractive error-dialog control preserves fatal logs/exit. |
| `tests/native/test_kernel_fibers_linux.c` | Reproduce polluted-parent rounding/FTZ/DAZ at actual guest entry and verify parent preservation in both scheduler modes. |
| `runtime/services/am.c` | Implement boot-lifetime suspended-tick event91 with distinct copied handles, and counter90 for the explicitly active-only application lifecycle; normal waits are not process suspension. WindowController10 models idempotent foreground acquisition for the already-focused native app; no new desktop/focus transition. |

New authored SIMD/FP/IPC/fatal-notice fixtures and the bounded strict-boot supervisor
retain reproducible failures and explicit architectural/coverage limits. No game
assets/generated code or external service implementation copied. Detailed evidence
and current acceptance state: [SIMD/FPCR and strict boot](../../docs/PLATFORM_STATUS.md).


## November 4, 2026 — LINUX-007/008 applet lifecycle

| Inherited file | Change and purpose |
|---|---|
| `runtime/services/am.c` | One synchronized boot-state owner, validated focus/notification policy, independent lifecycle event handles and real native-close/exit-lock coordination. Native app stays active; no invented HOME/sleep suspension. |
| `runtime/ipc.c` | Bound domain CMIF payload/object bytes and exclude domain header from service payload size; malformed requests fail before handlers read absent data. |
| `runtime/kernel.c` | Dispatch an idle deterministic baton after external event wake, guarding virtual timer callback reentry. Existing blocked host-notification failure reproduced first. |
| `runtime/nvn/vulkan/device.c` | Route SDL_QUIT and continuous/idle event polling through the tested native close adapter; guest cleanup continues under LockExit. Explicit guest exit behavior retained. |
| `runtime/services/fs.c` | Propagate File.Flush/Commit persistence failures; check recursive traversal/file sync and Linux directory sync before deleting rollback journal. Wider creation/recovery durability remains unproved. |
| `scripts/am_tests_linux.py`, `tests/native/test_am_linux.c` | Actual normal/domain IPC, malformed lengths/flags, state reset/race, real SDL events, close deadlines, blocked wake and virtual timer regression tests. |

New authored `runtime/services/am.h`, `runtime/host/applet_lifecycle_ui.h`, save
fixture/runner and detailed evidence accompany these changes. No external code,
game source/assets or generated shaders were copied. The15second grace period
releases an exit lock; existing GPU teardown can still delay process termination.
[Lifecycle evidence](../../docs/PLATFORM_STATUS.md).

## November4,2026 — LINUX-008 parental client lifecycle

`runtime/services/misc.c` now distinguishes initialized factory0 from factory1
clients awaiting Initialize1, with synchronized per-object ownership, close/reset
cleanup and an explicitly unavailable native parental-policy provider. No host or
console policy is read, disabled or enforced. Inherited policy-query stubs retain
their outputs and remain classified as stubs; unsupported commands remain strict.
`runtime/ipc.h`/`ipc.c` preserve send-PID descriptor presence independently of its
placeholder value. New authored focused IPC fixture/runner cover that contract.
Public libnx client shapes and an ignored local SDK audit inform the interface;
no external implementation or generated game source is copied. The native client
lifecycle does not claim full Horizon initialization or parental-control parity.
[Lifecycle evidence](../../docs/PLATFORM_STATUS.md).

## November4,2026 — LINUX-009 native graphical launcher

`tools/launcher/launcher.cpp` retains the inherited SDL3/ImGui settings interface
and Windows branch, adding native Linux paths/guarded state, checked atomic
settings saves, preserved custom settings/last-key precedence, supported-capability
UI and child process lifecycle. `linux_host.h` supplies Linux argv/exec/wait,
local report, atomic file and bounded SDL test-event helpers. Existing translation
strings are treated as text where appropriate to remove format-string warnings.
No external implementation or new UI library is copied. Bundled ImGui/SDL licenses
remain unchanged.

New Linux build/entry/UI/settings test scripts and authored fixtures preserve
receipted builds, state locks, actual settings-parser parity and actual UI Play
validation. Workspace shell/desktop entries open the native launcher. Game source
and generated code are unchanged by this launcher cycle. Linux precompile is
explicitly unavailable until failure/skip semantics are validated; no false
success or automatic skip was added. Atomic GUI save retention is tested before
rename; post-rename directory-sync failure cannot promise rollback. Inherited
in-game settings-save and malformed-parser gaps remain recorded.
[Launcher evidence](../../docs/PLATFORM_STATUS.md).


The launcher’s existing post-child settings reload now rescans mods and clears
stale binding capture; a real UI-loop fixture reproduces success/failure exits.
The bounded test supervisor records pending TERM/HUP/INT signals and raises only
between bounded pipe-drain polls, preventing a reproduced consume-before-retain
output loss. One absolute deadline and descendant cleanup remain enforced.
Actual-UI test receipts also preserve interruptions and evidence-write failures,
and reject forced cleanup as a clean pass. These are local test-tool correctness
changes, not changes to the game’s signal/timing behavior.


## November4,2026 — LINUX-010 native play-report refusal

`runtime/services/misc.c` now registers Linux-only `prepo:u` command10103
(SaveReportWithUserOld2). It validates the IPC envelope, then returns the known
Prepo PermissionDenied result under the project's explicit no-telemetry policy.
It never inspects, stores or transmits report contents. Unknown commands retain
strict diagnostics and cannot report success outside strict mode. This is not
full Horizon play-report/offline-setting parity. Public command/result contracts
and the selected SDK caller were audited; no external implementation was copied.
New synthetic actual-IPC tests preserve the original abort and validate normal/
domain responses, protected buffers, malformed requests, reset and strictness.
Launcher entry defaults select the new receipted game while preserving older
builds and the existing GUI. [Evidence](../../docs/PLATFORM_STATUS.md).


## November4,2026 — LINUX-010 idle-time extension state

`runtime/services/am.c` adds Linux ISelfController62/63 setter/getter state,
validated enum/envelopes and existing mutex/reset ownership. The active native
application retains SDL's existing game-lifetime screen-idle inhibition request
for all modes; no fake console timers/transitions or guest-thread SDL calls.
This does not guarantee compositor/system suspend behavior. Existing AM fixture
and runner add normal/domain/shared-object/reset/concurrent state checks and
malformed-request failures, preserving a before-fix strict62 reproducer. No
external implementation, game code, renderer or platform changes were copied.
[New Game evidence](../../docs/PLATFORM_STATUS.md).

## November5,2026 — LINUX-011 high-FPS GUI timing

`runtime/hle/framepace.c` supplies the existing engine's elapsed seconds to the
GUI animation clock at its own update callback, through a checked instruction
hook in `games/xcde/patches.py`. The original two-reference-period frame clamp,
GUI/node speed and pause behavior remain; world/skeletal/cutscene clocks are not
globally rescaled. The same file corrects rounded native-cutscene limiter targets
by switching the exact interval/period pair. Synthetic native tests cover clock
boundaries, state preservation and failures; optional selected-input component
analysis remains local and generates no distributable game content. No outside
implementation was copied. Integrated behavior still needs manual validation.
[Timing evidence](../../docs/PLATFORM_STATUS.md).

## November5,2026 — LINUX-012 held-input repeat

`runtime/hle/framepace.c` and `games/xcde/patches.py` replace the fixed reference
period at27 checked held-duration reads with the existing engine's elapsed time,
capped at the framework's two reference periods. This preserves borrowed input
records, raw held/press/release masks, per-object timer reset and repeat policy.
The scalar load and its upper-vector clearing remain explicit. Boundary tests and
selected-code consumer probes cover native30Hz and variable/high rates; manual
title/menu/battle validation remains separate. No external implementation copied.
[Repeat evidence](../../docs/PLATFORM_STATUS.md).

## November5,2026 — LINUX-013 passive grass/footstep diagnostics

`runtime/hle/foley_trace.inc`, included by framepace.c, logs bounded local event
metadata only when explicitly compiled with `scripts/build_linux.py --foley-trace`.
Six checked hooks retain original instructions and observe accepted markers,
contact-count changes, authored/surface sound requests and animation resets.
Ordinary builds disable the helper; no game-state edits, sound suppression,
playback-rate changes or audio correction is claimed. Native/selected-code negative
controls and passive CPU/memory/budget tests are recorded separately from live
listening. Existing clock/object synchronization is reused; enabled logging has
overhead. No external implementation copied.
[Audio investigation](../../docs/PLATFORM_STATUS.md).


## November5,2026 — LINUX-017 native Linux NGX SR / DLAA

Adapted `runtime/nvn/vulkan/{dlss.cpp,temporal.c,temporal.h,device.c,exec.c,textures.c}` and `runtime/nvn/output_scale.c` for the separately licensed native NGX provider, portable temporal prep/HUD, completed-submission resource retirement, reset/borrowed-pointer lifetime, and native DLAA pixel extents. New authored provider/bridge/API files use pinned ignored SDK headers/library; no SDK implementation copied. `runtime/{host/settings_ini.c,ui/graphics_options.h,ui/menu_vk.cpp}` and the existing native launcher retain supported mode mappings while exposing qualified DLSS and keeping Linux FSR3/framegen unavailable. Linux build/checked entry isolate and verify local companion resources. No generator/timing/game patches changed; no renderer replacement or DLSS5/FG integration. Component evidence and manual integration limits: [Linux DLSS record](../../docs/PLATFORM_STATUS.md).

LINUX-017 startup correction: checkedentry now isolates desktop XDG runtime locks while preserving Wayland/audio/DBus endpoints. Native Linux dlss.cpp enables the supported bufferDeviceAddress feature explicitly when NGX requests its extension; unsupported hardware falls back without feature use. Exact pre-fix failures and focused tests remain in the active Linux DLSS record.

## LINUX-020 native frame generation

Adapted `runtime/nvn/vulkan/{temporal.c,temporal.h,device.c,exec.c}` plus newLinuxinterpolation/pacinghelpers to the locally source-built AMD FidelityFXSDK1.1.4 compute API. ExistingVulkan/SDLrenderer/ordinaryswapchain/guestclockretained; generated+retainedrealimagesonrenderthread, no SDKWindowsproxy/presenter. NewproviderwrapperverifiesABI/source/version andcontexttimelineownership; LinuxF1/parser/launcher/build/checkedentrycapabilitygates retainunsupportedfallback. SDKsource/license/provenance arepinnedby `fsr-linux.lock.json`/`scripts/setup_linux_fsr.py`; originalsourceignored, build-working-copy portabilitypatches preserveasserts/typedalignment/bounds andallrequiredshaderpermutations. Newauthoredfixtures validatepixels,HUD,history/lifetime,sourcepresentationorder/monotonicpacing andUI/config. No generator/gameCPUoptimization, no framegeneration fromduplicatedsimulation. NativeGPUcomponentpassesdo notqualifyphysicalgamepacing; [Linuxrecord](../../docs/PLATFORM_STATUS.md).

## LINUX-021 experimental feature status

Linux-only wording in `runtime/ui/menu_vk.cpp` and `tools/launcher/launcher.cpp`
marks frame generation experimental and warns about slow performance/visual
glitches after user testing. Behavior, defaults, Windows labels and renderer are
unchanged. Checked entry and UI test defaults select the fresh023 pair; obsolete
local outputs were removed after archiving their metadata. No external code copied.
[Cleanup and validation record](../../docs/PLATFORM_STATUS.md).


## November 7, 2026 — LINUX-025 public Linux onboarding

The reviewed public export overrides `scripts/launcher_linux.py` with a narrow
verified-copy option for the launcher AppImage: original GUI/game receipt, state,
input, namespace and lease checks remain; only the native GUI executable path is
substituted after byte-identical ELF validation. Existing private development
launcher/build defaults are preserved. Root build/Play commands and local-only
AppImage packager/driver make the public source kit easier to build. Game-derived
code/binaries and all runtime state stay out of the image and source releases.
Focused authored and isolated clean-build evidence is in the project Linux record;
full gameplay and broad distribution compatibility are separate validation gates.


## November 8, 2026 — LINUX-028 spaced installation paths

`scripts/setup_linux_graphics.py` now quotes the sysroot for pkg-config flag
fragment expansion separately from the shell export. Literal variable/metadata
queries keep their existing raw path contract. Real authored pkg-config fixtures
cover spaces and shell metacharacters, variable queries and a tiny C compile; no
external implementation copied. Native build flags/dependency pins are unchanged.
[Build-path repair record](../../docs/PLATFORM_STATUS.md).
