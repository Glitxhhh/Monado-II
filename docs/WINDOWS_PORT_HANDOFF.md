> **Historical implementation brief.** Windows Experimental0.1.0 now contains
> the native implementation developed from this plan. The checklists below retain
> their original acceptance requirements; unchecked or historical wording is not
> a statement that all code is absent. Current implementation/qualification is in
> [platform status](PLATFORM_STATUS.md) and the [Windows guide](BUILD_WINDOWS.md).

# Windows implementation handoff — accepted Linux R5 reference

Prepared November 14, 2026. **This branch prepares Windows development; it does
not implement or validate the Windows port.** The target is the first modern
native Windows x86-64 release with every applicable feature and correctness fix
from the user-accepted Linux development build **103 + 104 R5**, while retaining
the original Windows capabilities. This replaces the November 13 / Linux 0.1.16
handoff. Read root [AGENTS.md](../AGENTS.md) and [BUILD_WINDOWS.md](BUILD_WINDOWS.md).

The maintainer accepts the current Linux R5 build **as-is**. The small Quality
layout change and a measured performance investigation are deferred until after
Windows is completed. They are recorded in [Deferred notes](windows-parity/DEFERRED_NOTES.md),
not prerequisites for this port. Do not start another Linux development pass,
change the accepted reference or promise to eliminate its reported 5–10 FPS cost.

## Reference and evidence boundary

| Item | Exact reference |
|---|---|
| Accepted implementation | Linux dev103 features plus dev104 startup correction, R5 |
| Reference checkout | `Monado_II_Public-linux-parity`, `codex/linux-parity` in the private development workspace |
| Reference base | `f0cb151af9152172187957095d5266d0777a0144` plus the recorded development patch; not a released Linux version |
| Reference source freeze | 1,219 files; SHA256 `582d67a37368a5917a6b271e827c025fda4c777e593545c6f90734e75f124b3a` |
| Reference replay patch | SHA256 `2fe0974d0cab9e57d8ae50ac9834d45be3af45680a9653d8f117df632a260a45` |
| R5 local AppImage | SHA256 `c821ed50f1f7dad79aa809248a48eb124c6fc7ecd573e04bf233e100311c50e7` |
| R5 game | SHA256 `7fa4c32bfe3c2a0a83f4a6edd4e0f7e205ac762224751872bd63655e5bcc486b` |
| R5 launcher | SHA256 `f551747a4cf3825d5684ed40afad2ef2f0eaff99d3bdd1b226387f4e9122f527` |
| R5 local GameLib | SHA256 `317363a347c33d0595b183c3a1b6200d516493884993ced83c87f930e336fcf0` |
| Previous Windows preparation | November 13 / Linux 0.1.16; branch tip `04493b40`, original Windows source unchanged |
| Windows evidence at this refresh | Original implementation only; no new native Windows build or gameplay validation |

The private reference hash identifies the accepted pre-refresh source freeze.
This branch adds preparation documentation, root test-harness corrections and its
own publication manifest, so do not describe that old hash as the final branch
manifest. Both port implementation trees remain byte-identical to the R5
reference. The test-only follow-up scopes Linux providers to Linux, lets native
fixtures use stock Clang without local dependency directories, and provides
authored Vulkan types for the callback-only worker test. All existing assertions
and negative controls remain; Windows source CI is not Windows gameplay evidence. The checked-in
[source inventory](WINDOWS_PARITY_INVENTORY.json) records **654 Linux / 330 Windows
files, 330 common paths, 58 changed common paths and 324 Linux-only paths**. These
are source-path counts, not implementation estimates or test results. Linux-only
paths include fixtures, scripts and APIs which must not be copied literally.

The R5 startup correction has maintainer-run gameplay evidence. Linux component
qualification includes the final 1,082-test suite, separate namespace and sanitizer
checks, native presentation/DLSS fixtures, actual entry tests and authored physical
GPU checks from dev103. None establishes native Windows execution, full DE/Future
Connected coverage, all-GPU performance or ultrawide correctness. Historical
whole-game Windows playability is author-reported until reproduced by this fork.

## Working decision and authority

Use the **whole current repository**, with `Monado_Windows/` as the implementation
target and the frozen `Monado_Linux/` as the reference. Bring changes across in
coherent, buildable bundles. Preserve Windows fibers/TLS, allocation/write-watch,
process/file behavior and Streamline/FidelityFX providers. Preserve Vulkan/SDL,
the generator and runtime design. This is feature/platform parity work, not a
renderer replacement, shared-core redesign or recompilation optimization campaign.

The Windows agent is authorized to plan each concrete solution and then implement,
compile, run authored component tests, debug and integrate it without repeatedly
asking permission for routine work. The maintainer is the sponsor, not a command
runner or debugger. Keep a bounded implementation ledger, carry failures through
actual build/test/fix cycles and report missing evidence honestly. Do not stop at
another general plan when a useful authorized engineering step is available.

**Gameplay and visible launcher interaction remain manual.** Do not launch/control
the game, press Play, change a human session or terminate it without a new request.
Native loader/entry tests using authored stubs, synthetic GPU tests, builds,
hidden/dummy UI checks and read-only inspection are allowed. Provide a complete
dev build and exact paths for the maintainer's manual test. Manual feedback steers
the same task; it does not erase earlier requirements.

Keep one ignored private root `PROJECT_MEMORY.md`, with exact evidence under
`Monado_Windows/runs/`. Record source/input/build identities, toolchain/flags,
host/GPU/driver, command/cwd, settings, scenario, output, failure and stop reason.
Reconcile process identity before resuming interrupted work. Preserve failing
fixtures and accepted artifacts. Public handoff/docs are reference material, not
a second current-state memory. Private logs, input, saves and game outputs are
never publication material. Source commits can preserve completed Windows work
under its actual validation label; a remote push/release needs its own authority.

## Resume on the Windows installation

Once this refresh is published, the canonical development branch is
`codex/windows-parity`. A fresh PowerShell checkout can use:

```powershell
git clone --branch codex/windows-parity https://github.com/brendonbusker/Monado-II.git Monado-II
Set-Location Monado-II
git status --short --branch
Get-Content AGENTS.md
Get-Content docs/WINDOWS_PORT_HANDOFF.md
```

An existing checkout must first reconcile changes, branch, accepted outputs and
running jobs. Integrate without discarding work; do not blindly reset, clean,
overwrite an existing destination or create concurrent builds against one state.
Read root/per-port `CONTRIBUTING.md`, `LICENSE`, `NOTICE.md`, `CREDITS.md`, the
relevant contracts under `Monado_Windows/docs/contracts/` and the actual scripts.
The inherited private `docs/dev/HANDOFF.md` and task history are not shipped in
this public branch; absent historical commands are not prerequisites or passes.

Some general Linux guides retain release-era wording, such as the old builder's
glibc 2.43 baseline, a 0.1.19 download name or FSR4 being absent from that release.
For this port, frozen R5 source and this handoff define the required feature set.
Do not use those older sentences to omit R5's portable runtime, FSR4 or GameLib.

## Input, state and distribution boundaries

- The selected immutable input is root `game_files/exefs/` and
  `game_files/romfs/`: extracted **Switch Definitive Edition 2.0.0**, title
  `0100ff500e34a000`, main build-id `7e1df8e08d60544b`, SDK build-id
  `ad3fdfbd62a5f4b0`. Retain `tools/extract/check_dump.py` and the supported-version
  rejection. Do not add package/key extraction, downloads or a new input campaign.
- The selected main SHA256 is
  `76ffa21b99c0385cfb14d1f90bbeddcdc98c17350b8edd79dfdbbb7341e70c48`;
  SDK SHA256 is
  `31ce67e8701584ba812dceeae9b590d7a6ad642bcb5aa74d2fe3ea5c5474250d`.
  Reconcile Windows-accessible mounts/links before asking about input; the Linux
  absolute pathname is not automatically meaningful after reboot. Never search
  unrelated disks or ask again for already-recorded selection information.
- Windows generated/build/runtime/profile output belongs under its own
  `Monado_Windows/` installation/state roots. Linux profiles/caches/binaries and
  the separate original-Wii Project Divinity are not Windows input or memory.
- Preserve human saves, settings, known-good builds and existing installations.
  New dev profiles use independent writable copies of the private approved 100%
  seed if available. The seed is not in Git. Never reseed a used/human profile,
  download a replacement save, or corrupt originals to test recovery.
- Preserve native `.sav` bytes/filenames and hidden Quick Step metadata. Saves,
  input, settings, useful caches and generated artifacts have distinct ownership.
  A guessed environment variable is not proof of write isolation.
- Source kits contain no game input, generated C/GameLib/game executable,
  translated game shaders/cache, saves, keys, firmware, vendor SDK binaries,
  private logs or unreviewed artwork. Original authored shaders are different
  from game-derived translated output. Review packaging guards and clean exports;
  ignore rules alone are insufficient. Do not run inherited game-copying packagers.

## Product target: graphical portable EXE first, BAT fallback

The primary Windows entrypoint is a **portable graphical setup `.exe`** which
works before any game is built. This is required for the first modern Windows
release; an updated BAT alone does not satisfy onboarding. Its final filename is
chosen with Windows release metadata after implementation. It needs no adjacent
checkout or separately installed Python and does not require an installer, WSL,
Linux sysroot, driver change or administrator launch.

The setup window selects/validates the user's own extracted input, selects an
empty final installation folder, offers the appropriate native provider/build and
shader-preparation options, and reports actual progress, detailed errors and logs.
Input stays in place. Subsequent opens reach the checked current settings launcher;
users press Play themselves. Keep **Use an existing installation** and **Locate
game files**, preserving existing profiles and active updates. The source/terminal
route and planned root `Build_Monado_II.bat` / `Play_Monado_II.bat` remain fallbacks.
Windows-only source/setup kits must work without `Monado_Linux/` present.

Use Unicode Windows process/file APIs and correct argument quoting throughout:
input, installation, state/cache, tool/DLL/model paths, logs/reports, links and
updater helper. Handle spaces, non-ASCII names and hostile literal argument text.
Use pinned, hash-checked project-local tools where permitted, clear missing-tool/
DLL guidance, owned process supervision and no automatic game start. Elevation,
system installation or vendor agreements require real applicable authorization;
ordinary builds/updates must not silently install system packages or accept terms.

## Implementation milestones

Every checkbox below is **required work to implement/qualify on Windows**, not a
claim that copying the reference completed it. Each milestone should leave a
reviewable build/test result before the next. Register every new helper in both
Windows compile and link lists; review HLE registration, reset hooks and Cpu/FP
preservation on the actual COFF build. `_WIN32` branches in copied files do not
prove the branches contain or correctly invoke the Linux repairs.

### 1. Reconcile and harden the native build

- [ ] Inventory Windows version/architecture, CPU/GPU/driver, Git/Bash, Python,
  compiler/linker, SDL/Vulkan/shaderc/VMA, FFmpeg/Opus and vendor versions. Inspect
  actual checkout/input/output paths and live processes before running anything.
- [ ] Pin an appropriate native toolchain. The inherited route uses Git Bash,
  MSYS2 UCRT64, Windows Python and Zig 0.14.1; current tooling requires CPython
  3.14. Verify requirements instead of assuming the old Python 3.12+ guide or a
  Linux dependency lock establishes Windows compatibility.
- [ ] Audit `Monado_Windows/scripts/{build_all,build_boot,build_rt_gfx,link_gfx}.sh`,
  `tools/launcher/build.sh`, `tools/setup_windows.ps1`, `tools/setup_game_files.ps1`,
  `scripts/copy_upscaler_dlls.sh` and `Build Monado.bat` before executing them.
- [ ] Correct failure propagation and explicit source/object lists. Use fresh
  per-build outputs, immutable receipts and atomic selection; preserve previous
  builds. Do not link stale object globs or report a printed success as a pass.
- [ ] Implement native root Build/Play fallback drivers and an explicit selected
  input/state protocol. Preflight the complete native DLL closure before long
  generation, then verify game/GUI/companions before success, Play or preparation.
- [ ] Preserve x86-64-v2 support, `-ffp-contract=off`, no fast math and applicable
  aliasing/FP flags. Record any justified Windows flag difference. No silent
  weakening of strict CPU/generator/service diagnostics.

Known inherited hazards: `build_boot.sh`'s `cc1` can turn compile failure into an
echo; `build_rt_gfx.sh` can default to `taskkill /F /IM xcde.exe`, has unquoted C++
paths and hardcoded MSYS2 locations; object globs can retain stale code. Setup
performs broad `pacman -Syuu` twice, unpinned pip and winget agreement acceptance.
The old BAT can offer to launch automatically. Replace process-name-wide kills
with owned handles/jobs; normal compilation is unelevated. The inherited Windows
recipe is source evidence, not a qualified safe one-command build.

### 2. Preserve native platform, file transactions and orderly shutdown

- [ ] Retain `runtime/platform/windows.c`, Windows fibers in `hle/fibers.c`,
  TLS/synchronization, guest allocation/write-watch and crash diagnostics. Review
  `cpu.h`, `platform.h`, `kernel.c`, `ipc.[ch]` and `rt.h` corrections individually.
  Test MXCSR/guest FP state, cross-thread fiber resumption/TLS, locks, reset and
  cleanup; inherited `CreateFiberEx` flags0 is not proof of FP preservation.
- [ ] Build disposable authored Win32 file-transaction fixtures first: ordinary
  file/reparse identity, bounded reads, exclusive temp creation, flush/replace/
  commit errors, sharing/locked-file behavior, journal retention, retry and
  interrupted operations. Do not turn POSIX flags into zero or claim unavailable
  durability succeeded.
- [ ] Port `services/fs.c` and `hle/quickstep_save.c` together. The Linux helper
  uses directory descriptors, `openat/mkdirat/fstatat/renameat/unlinkat`,
  `O_NOFOLLOW/O_DIRECTORY` and directory fsync. Provide actual Windows adapters
  and failure semantics rather than mechanical API renaming.
- [ ] Integrate Quick Step metadata into the payload transaction with story/UID/
  fingerprint/hash/generation checks. Cover atomic INI and shader/pipeline-cache
  writes as well. Preserve originals and require tests on disposable files.
- [ ] Transfer `services/am.[ch]`, applet lifecycle/exit locks, `host/main_boot.c`,
  watchdog and guest/worker/render/GPU lifetime coherently. Normal title/System
  Exit returns without a crash report, unwanted autosave or forced-close freeze.

### 3. Transfer the generator and every retained gameplay/QoL bundle

- [ ] `tools/lifter/{gen,patchfmt}.py`, `games/xcde/patches.py`, HLE classifications
  and applicable CPU/dispatch fixes: FunctionEntries before partition discovery,
  checked audited trading switch destinations and wrong-input rejection. Generate
  locally from verified Windows-accessible input; no Linux generated output reuse.
- [ ] High-FPS animations and held-repeat timing, **Normal30 / Fast60** menu
  navigation, footstep phase/threshold fixes and exact30 cutscenes:
  `hle/framepace.c`, `foley_trace.inc`, settings/parser and checked hooks.
- [ ] R5 fractional threshold correction: `hle/frame_threshold.h` plus framepace
  integration. Retain durations at144/165/360 and fractional rates without rounding
  the game to a multiple of30 or imposing the old8× ceiling. Count native frames,
  reject duplicate pad/output polls, reset on pause/backward/reboot/settings and
  preserve guest FP flags/atomic publication. Generated frames do not advance it.
- [ ] **99 manual slots plus separate autosave per story**, four native cards,
  No data without empty files, bounded initial scan, thumbnails/Continue/title:
  `hle/expanded_saves.[ch]`, `expanded_save_scan.[ch]` and their checked patches.
- [ ] Native title and System **Exit to desktop**, negative-default confirmation,
  existing Continue/Event Theatre/Extra Story rows and restrictions:
  `title_exit.[ch]`, `title_exit_model.h`, `system_exit.[ch]`,
  `desktop_exit_caption.[ch]` and ordered shutdown. No implied autosave.
- [ ] Complete stable dedicated **Quick Step slot, OFF default**:
  `quickstep_{slot,save,native,picker,stats,stats_ui,ui_memory}.[ch]` and
  `quickstep_native_ui.h`. Preserve native animation/picker/sounds, owned Quick
  Step I–VI only, ownership/transfers/removal, row and portrait E indicators,
  Save/Load/Continue restoration and ordinary-picker navigation. Include all
  characters, no boots/no native slot, empty slots, unique/slotted/filled gear
  and Fiora's drones. Continue must compose with expanded saves; no rejected
  ImGui-overlay replacement or partial backend-only implementation.
- [ ] NPC trading dispatch and Bionis queued shader/constants lifetime repairs:
  `nvn/{nvn.h,objects.c,cmdbuf.c,program_binding.h}` and
  `nvn/vulkan/{exec.c,shaders.c,shaders.h}`. Code/constants snapshots, stage masks,
  pipeline metadata, reset and cache identity form one bundle.

Unicorn tooling/differential tests are not runtime game-CPU fallback. Audit actual
strict/unimplemented paths; never insert success stubs, skip required effects or
reinterpret native service/GPU compatibility work as game-CPU emulation proof.

### 4. Controls, controller recovery, rumble and cheats

- [ ] Immediate held-L2 camera/HUD prompt behavior, shoulder party-switch chords,
  optional D-pad walking arbitration and neutral release/reset/disconnect/menu/
  focus/rebind behavior: `hle/{input.c,pad_modifier.h,party.c}` plus kernel reset.
- [ ] Explicit Windows party worker safety: current `party.c` still retains a
  Windows infinite Sleep-loop scanner/direct reads while stop/join/reset/guarded
  read fixes are Linux-gated. Adapt epochs, object validation, lock/thread/event
  ownership and stop/join while preserving Windows discovery. Copying the file
  does not port those repairs.
- [ ] SDL open-failure fallback, delayed arrival, active removal/reselection,
  bounded diagnostics and single ImGui handle ownership:
  `platform/sdl_controller_{diagnostics,recovery}.h`, input and launcher. Add019's
  bounded rescans/focus-return retries without stealing healthy selection or
  retaining held buttons after overlays/disconnect. Preserve applicable Steam/
  SDL mappings/filter/virtual-pad settings without duplicate physical+virtual
  inputs or a hardcoded controller name.
- [ ] Optional **controller vibration, OFF default**: `hle/rumble_{ipc,policy}.h`,
  `services/hid_set.c` and input-owner pump. Validate libnx command/count/player/
  actuator inputs and finite amplitudes, bounded expiry/envelope; stop on focus,
  menu, disable, reboot, exit, disconnect or handle replacement. Test with SDL
  virtual callbacks, then manually qualify physical devices.
- [ ] Dedicated Cheats tab: independent enables for damage dealt/received,
  EXP/AP/SP/gold gained and no fall damage, all **OFF**. Six sliders use integer
  percentages0…2000, exact0.01× steps over0×…20×; preserve round trips/reopen.
- [ ] Port `cheats_{combat,rewards}.[ch]`, `cheats_math.h`, settings/UI and checked
  generator hooks together. Disabled/exact1× identity, party source/target and
  AI eligibility, caps and single award scaling remain. Native healing/spending/
  Expert Mode transfers of earned EXP stay unchanged; earned gains remain after
  disabling. No fall damage covers ordinary landings, not general invulnerability.
  No faster-Arts-recharge cheat is requested. Generic cheat declarations do not
  need a Linux condition removed; helper objects and hooks must actually link.
- [ ] Keep the XC3 party-switch toggle in Mods immediately beneath Quick Step,
  preserving its existing key/behavior. Preserve keyboard/controller mappings,
  hotplug, prompts and focus behavior from the original Windows implementation.

Linux Steam metadata snapshots, replaced-inode handling, `/dev/hidraw` permissions
and Landlock rules are not Windows device APIs. Implement their applicable native
lifetime/environment behavior, with native hardware claims based on native tests.
Later Linux Deck/ROG reporter success does not qualify Windows handhelds.

### 5. Launcher, graphics controls, adapters and support reports

- [ ] Reuse native launcher presentation from
  `tools/launcher/{launcher.cpp,theme.h,theme_resource.py,assets/}`, including
  branding,1267×804 default/min640×480, responsive layout, language/audio/input/
  mod/cheat/save/diagnostic controls and all original settings. Adapt DPI/fonts,
  Unicode process/file paths and owned Windows handles.
- [ ] Generate reviewed artwork/font includes in the Windows build and copy only
  reviewed licensed assets into Windows-only kits. Artwork guard rules are
  path-specific; extend exact reviewed Windows paths with tests, not general
  image admission. Preserve DejaVu/notices/provenance; no local sword PNG.
- [ ] Atomic INI writes preserve unknown/custom keys and bindings; report write
  failure and reload after game exit. Launcher, F1 and game parse identical
  typed values. Restore defaults must not erase unrelated custom preferences.
- [ ] Port `graphics_options.h`, `quality_policy.h`, `dynamic_resolution_policy.h`,
  `grass_lod.h`, `prop_lod.h`, `hle/graphics.c`, `foliage_distance.[ch]`,
  `services/romfs_virtual.c` and checked patches as a coherent typed bundle.
- [ ] All extra effects/quality controls stay **Original**. Shared launcher/F1
  controls include game temporal AA, sharpening, bounded host Dynamic minimum/
  maximum, shadows/mask, cloud/AO/reflections/bloom/godrays/light shafts/motion/
  fur blur, filtering2×…16×, grass and propLOD, original1024/high2048 shadows and
  experimental bloom stability. Preserve restart labels and reversible per-frame
  working-copy overlays; authored/modded files are not mutated.
- [ ] Presets Balanced/Performance/Low hardware/Custom alter only explicit
  resolution/DOF policy; they preserve GPU/upscaler/FG/FPS/gameplay preferences.
  Loading `quality_preset` never reapplies it. Zero sharpening is genuinely zero;
  default is distinct. Native temporal reconstruction disables the game's AA on
  its jittered input. DRS budget counts native render frames, not generated output.
- [ ] Do not revive retired Full effects resolution (native occlusion failure)
  or4096 shadow maps (render-pool failure). Preserve legacy normalization4096→2048.
- [ ] Port `gpu_selection.h`, `gpu_requirements_vk.h` and the launcher probe
  policy: Auto default, optional stable UUID selection on restart, real capability
  and eligibility revalidation, honest reasons/fallback. Use native Windows GPU
  enumeration/Vulkan mechanisms; selecting an index is not stable identity.
- [ ] Adapt `tools/launcher/linux_report.h` to a native local reviewable ZIP:
  bounded selected text/INI/run metadata/crash text, CRC/manifest, no shell or
  automatic upload, no environment dump, save/game/private binary core contents.
  Ordinary-file/reparse/size/collision validation needs Windows fixtures.

The maintainer's requested future placement of extra controls immediately below
Show FPS, with GPU selection staying put, is deferred. Port the accepted R5 layout
first; do not silently convert that note into a pre-port cleanup requirement.

### 6. All aspect choices and issue12 corrections

- [ ] Transfer `host/xcde_aspect.[ch]`, `nvn/{aspect_canvas,aspect_composite}.h`,
  `output_scale.[ch]`, camera/party hooks, device/exec integration, settings/UI
  and checked patches together. Reference [aspect behavior](ASPECT_RATIOS.md).
- [ ] Original16:9 default; Match display and explicit16:10/21:9/32:9 offered on
  every monitor, independently of window-size presets. Fixed ratios fit centered
  with bars; no stretching/cropping. Match follows actual drawable shape. Exact
  21:9 at1440 high is3360 wide, so small bars on3440×1440 are intentional.
- [ ] Adapt actual perspective cameras and CPU visibility frustums; preserve a
  centered proportional16:9 HUD/text/menu canvas and world-marker mapping. Keep
  first-camera, subsequent targets, fixed-fit canvas and temporal content extents
  consistent. Fixed artwork/video cannot reveal extra picture.
- [ ] Keep093 reduced scene-composite/multiple-input classification and logical
  camera/FOV aspect correction; retain bounded diagnostic decisions.
- [ ] Keep103's narrow competing-resolution fix: in non-Original aspect modes,
  suppress guest `red_Auto` through the virtual INI. Preserve Original real GPU
  timestamps/occlusion and independent host Dynamic behavior. Do not transplant
  a global frozen-timestamp workaround or disable unrelated rendering.
- [ ] Mark opt-ins experimental until native Windows camera/culling/HUD/markers/
  dialogue/menu/effects/cutscene/live-resize/Alt-Tab tests are available. Linux
  fixtures and user acceptance do not prove the actual3440 issue12 is resolved.

### 7. Temporal providers and shared renderer correctness

- [ ] Preserve native Windows Streamline DLSS/DLAA and FidelityFX Vulkan FSR/FG.
  Apply portable corrections in `temporal.[ch]`, `temporal_camera.h`, `dlss.cpp`,
  exec/device/textures/shaders and provider bridges with actual Windows resources.
  Do not replace Windows providers with Linux NGX or `dlopen` modules.
- [ ] Newly selecting DLSS chooses **DLAA / Native100%**, but reopening preserves
  an explicitly saved quality. Implement the actual Windows UI branch. The old
  Linux-only transition does not do this automatically. Scaled67% shaking is
  unverified/unresolved; Native100% remains the default/workaround.
- [ ] World-space camera history/cut detection, invalid/extreme motion sanitation,
  reactive inputs, consecutive successful evaluation ownership and proper resets
  apply to all temporal providers. Linux NGX receives a bias-current-color mask;
  supply the appropriate Windows/Streamline equivalent with validation. Retain
  HUD separation, resource lifetime and guest reboot/reset/shutdown ownership;
  inherited empty Windows reset/shutdown branches need implementation/review.
- [ ] Provide current native temporalFSR3 support and NativeAA/scaled quality
  modes with checked inputs, output color-space conversion, allocation/capability
  handling and fallback. Linux's SDK UMA allocation correction specifies required
  memory semantics, not a reason to force its allocator code onto Windows.
- [ ] Optional experimental **FSR4 Lite** must have explicit local preparation,
  capability/receipt/model identity/size validation, progress/cancel/retry and
  fallback preserving existing preferences. Review `fsr4_api.h`, `fsr4_policy.h`,
  `temporal_fsr4_linux.inc`, the lock, ADR and preparation/worker/broker fixtures.
  Adapt to native Windows SDK/provider/preparation, not a required Proton prefix.
  No automatic model download/selection, game input access or ambient model ENV.
- [ ] Retain four quality/model sets and finite/detail/reset/sharpening checks.
  Preserve the tiny107×60 Ultra failure reproducer: accepted minimum guard is
  **long edge≥128**, not an arbitrary128×72 rule. Failure remains a reason for the
  existing fallback, not a falsely available quality mode.
- [ ] Correct zero/default sharpening and nativeFSR linear output→sRGB encoding
  before the existing HUD/FG composition. Provider/output extents and history
  ownership must agree. Integration tests need actual provider inputs, not just
  a checked UI box. SpatialFSR1/bilinear provide no temporal FG input.
- [ ] Legal Vulkan attachment feedback: feature/usage/layout/pipeline agreement,
  read-only eligibility throughout the rendering instance, separate clear-only
  pass where required and complete snapshot fallback. BY_REGION alone does not
  establish cross-pixel visibility. Track all subresources, allocation identity,
  visibility, aliases, destruction/reallocation and GPU lifetime.
- [ ] Preserve `maxwell/scaled_gather.h` and translator scalar taps using native
  textureGather so base-level/minLod semantics remain correct. Unity-scale fast
  path stays. A textureLod replacement has a retained failing counterexample.
- [ ] Optional `bloom{,_policy,_prefilter}.h` Full-footprint/Karis modes retain
  Original default, immutable compiler/program/cache identities and constants/
  stage-mask lifetime. Do not advertise these component corrections as fixes for
  every historical Makna/Valak/pipe artifact.
- [ ] `sampler_filter_policy.h` plus production `textures.c` cache: only eligible
  mipmapped linear color sampling, excluding depth/shadow/integer/point/nonmip/
  reduction cases. Clamp enabled device anisotropy; immutable bounded cache,
  no borrowed guest wrapper ownership and destruction only at GPU idle/reset.
- [ ] Preserve shader preparation as a separate owned child operation with honest
  complete/partial/cancel/fatal outcomes, safe cache commit and explicit retry.
  Remaining variants may compile during play; preparation does not guarantee all
  runtime pipelines or start a game. Preserve native Windows shader/provider
  features, compatible cache identities and actionable bounded diagnostics.

FSR4 optional backend/capture code is GPL-2.0-or-later with AMD SDK terms, recorded
in `Monado_Linux/docs/adr/0013-native-fsr4-linux.md`, component licenses and notices.
This is not covered merely by the root MIT license, and dynamic linking is not a
copyleft exemption. Any combined distribution must meet applicable licensing and
corresponding-source obligations. Preserve exact source/provenance/notices/build
scripts; no opaque optional DLL distribution. Models, SDK DLLs and converted
shaders/captures are local preparation outputs, absent from normal source kits.

The accepted R5 includes reviewed MIT-derived history/renderer adaptations with
local contract tests and attribution. The later RebornPR72 own-pixel scaled-read
default is **not in R5** and is not a new parity requirement. Do not silently add
it or copy another project's unreviewed/GPL code under an MIT label.

### 8. Stable experimental2× generation, FPS and run diagnostics

- [ ] FG stays **experimental/OFF default**. Preserve exclusive method selection,
  ordinary Off behavior and all existing Windows provider features. Reference
  [frame-generation semantics](FRAME_GENERATION.md), not Linux API dependencies.
- [ ] Port persistent generated/real output timeline, bounded look-ahead/worker
  overlap, safe serial fallback, missed-deadline recovery without catch-up bursts,
  explicit image/queue/GPU ownership, acquire capacity and reset/swapchain drainage.
  Relevant reference: `framegen_policy.h`, `framegen_pacing_linux.h`,
  `framegen_present_linux.h`, `temporal_fg_linux.inc`, device/exec and WSI tests.
  Use native Windows synchronization/provider/presentation adapters.
- [ ] **Match display refresh** caps native production at half fractional display
  Hz, respecting the saved game FPS ceiling. Activate only with successful valid
  generation/history; restore normal limiter on disabled/unavailable/failure.
  For example180Hz/120 setting requests90 native+90 generated, not60→180.
  Test actual game build feature guards;101 fixed a cap compiled out of production.
- [ ] VSync uses FIFO for integratedFSR at all caps. External driver methods may
  have different actual presentation policy; log it, do not infer it from a box.
  Cutscene-only remains30→60. No Auto3×/arbitrary-phase MFG: the prototype failed
  the quality gate. Prerecorded-video interpolation is unimplemented.
- [ ] Broader integrated2× eligibility requires usable DLSS/FSR3/preparedFSR4
  temporal inputs, provider receipt/capability and actual successful dispatch.
  Preserve ordinary output during menus, unsupported scenes, resets and fallback.
- [ ] Independent OFF-default on-screen FPS plus F1 setting: `ui/fps_meter.h`,
  `fps_overlay_draw.h`, menu/launcher/device/exec. Keep the title-bar preference.
  Draw input-transparent text after FG and outside retained histories.
- [ ] Game counts actual acquired native frames; Output counts accepted submits;
  integrated Generated counts classified successful interpolated submissions,
  excluding native/repeated images. None measures physical scanout. Preserve
  sampling warmup/unavailable/error states; never estimate Game×2 or infer native/
  generated identity by subtracting unrelated counters.
- [ ] Native per-run requested settings, parsed/effective settings, display,
  presentation mode, policy and actual applied limiter logs. Retain up to16
  unmodified owned run records; omit unknown/private keys and bindings. No run
  record for shader preparation/dry runs. Bounded measured samples about every
  two seconds while generation is selected, even when the overlay is hidden.
- [ ] Decide any NVIDIA Smooth Motion/native driver option from actual Windows
  capability and provider evidence. Linux driver-component detection, Vulkan
  downstream observer, `/proc/self/task` CUDA exception and host Play broker are
  Linux-specific. Preserve truthful native equivalents where supported; never
  expose a Linux request as working Windows support. Windows generated-only or
  output identity unavailable from an API remains labelled unavailable.

The Linux observer counts accepted downstream presentation requests, not distinct
scanout images; CUDA broker changes permit only an opted-in game's own metadata
writes. These are useful security/measurement contracts, not Windows filesystem
exceptions to copy. No driver/system installation or global FG overrides.

### 9. Portable builder, local GameLib and updater integration

- [ ] Implement the portable GUI `.exe` protocol from `tools/builder_backend.py`,
  `builder_bootstrap.py`, the authored setup frontend/runtime boundary and
  `Monado_Linux/tools/launcher/input_recovery.h`. Native Windows packaging must
  include only an explicit reviewed setup/source/interpreter/tool payload with
  notices; no game/derived code/assets/vendor SDK/model payload.
- [ ] Fresh setup/check/select/build/error/cancel/retry, actual progress and
  safe source staging; reject overlapping input/state/source/install roots and
  preserve nonempty/unknown/edited destinations. Optional worker limit1…6.
- [ ] Persist interpreter/tool/source cache outside temporary EXE extraction;
  preserve venv bases across reopen/update. Prevent bytecode/cache mutation from
  invalidating a checked runtime. Reopen current launcher; adopt existing checked
  installations in place, no save import/conversion or automatic Play.
- [ ] Locate missing input with the strict existing-build executable inventory,
  known-link transaction, busy/lease checks and unchanged original receipt. No
  immediate rebuild; a later update may rebuild once at the new path. Preserve
  current input selection on mismatch/cancel/failure.
- [ ] Port017–018 update-repair semantics: verify downloaded source kit and its
  generated distribution metadata, normalize only a temporary packaging export,
  acquire locked runtime before packaging, test the staged native pair rather
  than an old active launcher, retain wrapper sibling imports/private interpreter.
  Do not weaken clean-source guards or rewrite installed input/settings/source.
- [ ] Adapt split authored-runtime/local game-code design from `runtime/gamelib.[ch]`,
  `host/gamelib_entry_linux.c`, `scripts/gamecode_identity.py`, generation/build/
  run/launcher/precompile/updater paths and `tools/portable_runtime.py`. Native
  Windows uses PE runtime plus locally generated `lasgame.dll`, not an ELF `.so`.
- [ ] Bind reuse to actual selected main/SDK hashes, generator/profile/patch/HLE/
  recursive guest-header closure, compiler/flags/architecture and GameLib ID/ABI.
  A stale generatedC stamp cannot acquire a new ABI. Verify owned receipt/library/
  ID/file hashes and source closure before local game-code reuse. Runtime/UI-only
  updates may reuse a compatible local library; generator/ABI/input changes rebuild.
- [ ] **Preserve104's startup ordering fix:** successful `gamelib_load` precedes
  watchdog/workers/module loader/kernel/`dispatch_init`. The old split R4 crashed
  by dereferencing NULL tables before loading. Test the actual production entry
  with authored stubs, missing/wrongABI/bad-library failures and static/precompile
  bypass. Library unit tests alone missed this integration error.
- [ ] Audit native loading rather than inheriting `LoadLibraryA`, weak Windows
  `.id` reads or ambient `LARECOMP_GAME_LIB` candidate overrides. Use Unicode safe
  library search, ordinary owned file/receipt/ABI validation and actionable errors
  before execution. Do not load arbitrary code while merely checking existence.
- [ ] Optional game-free authored-runtime bundle: externally trusted archive SHA,
  source/ABI/features/runtime/dependency closure, bounded ordinary members, no
  overwrite/traversal/reparse links and inspectable game-code exclusion. Verified
  compatibility mismatch may select full-source fallback; corrupt outer/interior
  identity is a failure, not a fallback excuse. No GameLib/game code in the bundle.
- [ ] Preserve full source/static-game fallback, legacy receipt migration and
  explicit feature/argument validation. Forward new options through GUI/BAT/build/
  update entrypoints with real argument arrays.103 caught an old-client wrapper
  rejection; test spaces, literal `$()`/backticks and missing flag pairs safely.
- [ ] Pin native tool/dependency closure with atomic content-inventoried source
  caches and owned bounded process teardown. Support spaces/Unicode/relocation;
  no stale temporary build paths in final runtime/metadata. Retain failed logs.
- [ ] Native updater reuses `tools/{update_release,update_retention}.py` policy,
  Linux driver/build/lease fixtures and new builder/runtime integration. Quietly
  offer only a newer compatible **Windows** implementation; verify staged source,
  build, manifest, DLL closure and actual active version before activation.
- [ ] Rebuild locally where required, progress/cancel, activate safely with native
  exe/DLL locks, restart launcher without Play. **Current-only retention**: keep
  current on failed/cancelled prep; only after successful activation prune known
  obsolete owned versions/downloads. No local rollback-history/helper feature.
  Preserve saves/Quick Step metadata/settings/input/useful caches. Defer busy,
  unknown, modified or unsafe trees; do not follow junctions or kill unrelated work.

Linux Noble/glibc2.39, source-builtSDL/FFmpeg/Python, ELF/rpath, `dpkg`, Distrobox,
GNU host ABI, private `/tmp` builds and Linux lock recipes are not Windows
prerequisites. Port their reproducibility/closure/error/ownership guarantees using
appropriate native dependencies. Full source fallback remains available. The
private Linux runtime export is development evidence, not a Windows redistributable
or approval to bundle arbitrary third-party/vendor binaries.

### 10. Qualify, package and version the first Windows implementation

- [ ] Port-selectable native fixtures for substantive changes. Many current
  wrappers hardcode `Monado_Linux`, ELF/rpath/pthread/mmap/process_vm_readv or skip
  outside Linux. Adapt pure models/math/pad policies and actual production TUs;
  a green source CI Windows job or a skipped native test is not Windows parity.
- [ ] Build clean/incremental/failure paths, spaces/Unicode, missing tools/DLLs,
  compile/link failure propagation, wrong source/input/ABI, locked files/reparse/
  interruption, atomic config/cache/save writes, actual entry ordering and owned
  shutdown. Preserve reproducers and test failure paths before human profiles.
- [ ] Actual authored GPU/provider/presentation tests on each available Windows
  GPU/driver, plus settings/launcher/DPI/preparation/report/builder hidden fixtures.
  Absence of a GPU/scene is unqualified, not an implicit pass.
- [ ] Complete a native dev build with every applicable parity bundle, exact
  executable/DLL/source/receipt hashes and independent seeded profile. Explain
  actual fallback/unavailable features; do not conceal missing functionality
  behind polished setup UI. Preserve accepted builds and Linux behavior.
- [ ] Give the maintainer the exact graphical EXE, installed launcher/game,
  build/profile/log paths and manual checklist below. Do not run Play on their
  behalf. Use feedback to fix concrete Windows failures through bounded cycles.
- [ ] Update Windows guides/status/changelog/source-kit metadata to actual results.
  Until implemented, retain **original implementation** and version-free original
  Windows download identity. Never borrow Linux0.1.20 or the R5 source label as a
  Windows release number/update signal. Establish independent compatible Windows
  release metadata only with the implemented source.
- [ ] Packaging must preserve complete component licenses, provenance, legal
  resource boundaries and source guards. A public Windows binary/setup payload
  needs the applicable dependency/vendor/GPL/corresponding-source review; a clean
  source kit and local private build do not grant redistribution rights.

## Manual acceptance and actual completion criteria

The goal is native Windows **full DE and Future Connected** with existing QoL,
correct real-time behavior, reliable saves and a usable portable graphical build/
update experience. A title screen/opening battle/component suite is intermediate.
Do not claim a complete full-game pass from the small checklist or100% seed.
Document tested scenes, story/character/save lineage, durations and remaining gaps.
No arbitrary FPS percentage gate applies. The accepted R5's reported5–10FPS cost
is allowed in this parity baseline; correctness/feature preservation takes priority
over deferred optimization, without excusing a newly demonstrated Windows defect.

| Area | Required native Windows checks |
|---|---|
| Setup | Portable GUI EXE with no adjacent checkout/host Python; own-file check, empty destination, exact progress/logs, cancel/retry/reopen/adopt/LocateInput, spaces/Unicode, missing tools/DLLs, immutable input, BAT fallback and Windows-only kit |
| Launcher | Art/fonts/DPI/responsive layout, every retained tab/control, bindings/unknown keys, defaults and round trips, game return, preparation/report paths, no automatic Play |
| Timing |30/60/120/144/165/fractional/Unlimited as appropriate, Normal/Fast menus, Arts held-repeat, animations/footsteps/cutscenes, ordinary real-time speed independently of submitted FPS |
| Saves | Main story and FC, four visible cards through99manual+autosave, No data/no empty files, thumbnails/Continue/fresh-process reload, disposable interruption/recovery/journal/locked-file tests |
| Quick Step | All characters/ranks/gear cases, owned transfer/removal, native sounds/row+portraitE/picker restore, Save/Load/Continue/restart, OFF path and unchanged native save format |
| Exit | Title rows/restrictions, Continue/Event Theatre/Extra Story, System, Cancel/No default, orderly guest/render/GPU exit without autosave/freeze/crash report |
| Controls | ImmediateL2 camera/HUD, party chords/walking arbitration, menu/focus/reset/disconnect/rebinding, failed-first/delayed/replaced pads, no duplicate/stale input, rumble off/on/stops/expiry |
| Cheats | Each OFF/0×/1×/fractional/20× and reopen precision, caps/partyAI, native healing/spending/Expert transfers, ordinary falls; no Arts-recharge feature |
| Aspect | Original/Match/fixed16:10/21:9/32:9, camera/culling/first target, proportional HUD/markers/dialogue/shop/menu, effects/video/cutscenes, resize/fullscreen/Alt-Tab/highFPS and native temporal extents |
| Graphics | Original defaults, presets never reapplied on load, zero/default sharpness, quality controls/restart/restore, eligible AF only, grass/LOD/shadows2048, no retired settings or missing effects |
| Providers/rendering | Windows NVIDIA/AMD separately where available; native Streamline/FidelityFX/optionalFSR4, correct fallback/history/motion/reactive/color inputs, feedback/subresource/alias/GPU lifetime, trading/Bionis scenes, precompile cancel/retry and remaining-variant diagnostics |
| FG/counters | Off baseline first; valid2× temporal inputs, fractional Match cap/fallback, FIFO/native provider policy, output-worker/reset/error ownership, actualGame/Output/Generated/unavailable labels, hidden-overlay logs; no physical-scanout/MFG/movie claims |
| Runtime/bundle | Local GameLib generation/reuse/rejection, actual boot ordering, static/full-source fallback, wrongABI/source/input/hash/corrupt bundle, no game code in authored runtime/setup |
| Updater | Compatible Windows version popup/no-popup, checksum/manifest/closure rejection, rebuild/cancel/failure, locked activation/restart, active marker, current-only cleanup, intact saves/metadata/input/cache, no Linux asset chosen |
| Coverage | DE and FC transitions, menus, battles, movies/audio, demanding scenes and long sessions; name actual coverage and leave missing devices/scenes explicit |

Validate same-build noise before interpreting deterministic/visual comparisons.
Native FPS, distinct images, simulation progress, presentation submissions and
physical scanout are different measures. Deterministic hidden paths can disable
asynchronous/temporal behavior; ordinary manual real-time coverage is necessary.
Historical author logs or Linux fixture passes never become Windows passes by
being copied into a release note.

## Linux mechanisms to adapt, and accepted limits to retain

- RADV driver-owned heap/dma-buf/fixed mappings and Mesa `nodcc` are Linux driver
  policies, not generic Windows AMD settings. Preserve Windows memory-import/
  vendor policy and validate actual Windows GPU memory behavior. Retain
  patrickksmdev's Linux provenance without advertising an invented Windows fix.
- No `ucontext`, Linux sysroot/ELF/glibc floor, Landlock, `/proc`, `/dev/hidraw`,
  private mount/UID/file-mode/fsync assumptions as Windows requirements. Native
  counterparts must meet applicable safety/lifetime/semantic contracts.
- Scaled DLSS67% shaking, historic Valak/Makna boxes/sparkles/blue pipe and the
  user-confirmed pre-existing Colony9 railing-shadow flicker are not proven fixed.
  Fullscreen helped one symptom, not every cause. Do not weaken shadows or claim
  all graphics issues resolved from authored component checks.
- FG remains optional/experimental; it can stutter, add latency or lower real
  FPS. Linux's qualitative acceptance is not a universal Windows performance claim.
- Deferred layout/performance notes are retained after the port, with no cleanup
  of accepted Linux source before Windows work. PR72 is outside R5. FullDE/FC,
  long-session/all-GPU/ultrawide/handheld/67% coverage remains explicit until tested.

## Concrete first work after host reconciliation

Start with native build failure/ownership hardening, authored Windows FP/fiber/
file-transaction fixtures and the graphical builder/backend skeleton. Then carry
the coherent bundles above into fresh outputs, add production-entry/GameLib tests,
and continue through provider/render/build/update checks before manual handoff.
Do not spend the first session recreating another Linux plan or waiting for a game
launch when independent build/component work is available.

The following are **source-inspected probes, not Windows passes**. Use disposable
owned outputs and bounded child timeouts. After the native venv exists, these
three pure generator contract cases can run from repository-root PowerShell:

```powershell
.\Monado_Windows\.venv\Scripts\python.exe -B .\tests\test_function_entries.py EntryContract.test_guards_before_returning_entries_and_other_module_ignored EntryContract.test_wrong_build_or_any_changed_word_fails_closed EntryContract.test_whole_generator_emits_every_checked_dispatch_destination -v
```

They initially inspect the Linux reference generator. Adapt port selection after
transfer; do not run mmap-based dispatch cases as if they were native Windows
coverage. Likewise native timing/renderer/file fixtures require real adapters.
From Git Bash in `Monado_Windows`, after verifying the pinned native Zig tool:

```sh
mkdir -p build/windows-parity-component-r1
.venv/Scripts/python -m ziglang cc -std=gnu11 -O1 -UNDEBUG -march=x86_64_v2 -ffp-contract=off -fno-strict-aliasing -w -Iruntime -o build/windows-parity-component-r1/test_fibers.exe tests/native/test_fibers.c runtime/hle/fibers.c
build/windows-parity-component-r1/test_fibers.exe
```

That inherited fixture checks basic switching/migration, not complete FP/TLS/
ownership qualification. Extend meaningful assertions and retain failures. Audit
`scripts/native_tests.sh` before use because it overwrites fixed platform objects.
No first probe needs the user's game files or human saves.
