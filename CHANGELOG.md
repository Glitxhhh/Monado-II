# Changelog

## 0.1.4 — November 8, 2026

- Fix Linux dependency compiler/linker flags when the installation folder contains
  spaces, such as `My Games/Monado II`. Quote the pkg-config fragment separately
  from its shell assignment while preserving literal variable queries used by
  launcher setup. Existing folders do not need to be renamed or moved.
- Add real pkg-config regression coverage for spaced paths. Preserve the existing
  input location, save profile, native DLSS/DLAA and updater behavior. Users build
  locally from their own input; no game-derived binaries or caches are shipped.
- Continue regular Linux and Windows source/setup kits. This is a Linux build
  tooling fix; Windows runtime/backports and full-game coverage are unchanged.

## 0.1.3 — November 7, 2026

- Add a **Linux stable-release updater**. Normal startup quietly checks the
  official release; a newer version offers **Update** or **Later**. Update
  verifies/downloads the source kit, builds locally with progress and Cancel,
  then restarts the launcher without pressing Play.
- Keep every managed version at a fixed path under `Monado_Linux/updates/`.
  Compatible presentation-only updates retain the verified game; other changes
  rebuild it locally. Preserve the existing DLSS build choice and the original
  shared input, saves and settings. Download/build/validation failure or
  cancellation leaves the previous selection active. Edited release-owned source
  stops automatic installation so local changes can be merged manually.
- Add explicit previous-version selection with
  `Monado_Linux/.venv/bin/python -I -B tools/update_linux.py --rollback` from the
  original installation root while its default profile is closed. This changes
  the active pointer; it does not restore/delete builds, input or saves.
- Bootstrap requires one manual **0.1.3 source upgrade/build**: 0.1.2 has no
  updater. Merge the verified source kit into the same original folder and rebuild
  to retain its profile; fresh kits elsewhere do not import old profiles. Full
  automated rebuilds of custom
  experimental frame-generation or footstep-diagnostic builds currently stop
  safely and require a manual rebuild. No Windows updater/backports are included.
- Retain the existing native Linux port, high-refresh animation/menu/footstep
  corrections, menu-speed preference, shader preparation, default DLSS/DLAA and
  generated launcher artwork. Frame generation remains optional and experimental.
  Continue **TheMonadoGuy / Monado contributors' Monado** and **Peiikai's
  LASRecomp**, with their notices preserved; see [credits](CREDITS.md).
- Publish regular Linux and Windows **source/setup kits**, requiring the player's
  own legally dumped DE 2.0.0 input and local game generation. No game files,
  compiled game binary, private SDK or runtime update directory is distributed.
  Existing Windows/full-game validation limits remain unchanged.

## 0.1.2 — November 7, 2026

- Restore the original generated landscape artwork to the launcher's left panel,
  with the exact asset recorded in
  [provenance](Monado_Linux/tools/launcher/assets/ARTWORK_PROVENANCE.md). No game
  screenshot or extracted game input was used; the separate local sword icon
  remains excluded.
- Include native **DLSS Super Resolution / DLAA** in the one-command Linux build
  by default. Fetch the pinned official NVIDIA SDK locally and pass the existing
  native-provider build option. `--without-dlss` explicitly omits it; setup
  failures stop the build instead of silently removing the feature.
- Check DLSS prerequisites up front: bubblewrap at `/usr/bin/bwrap`, `nm`, and
  the readable GNU C++ runtime. Preserve runtime GPU capability, state isolation,
  namespace and network checks.
- Keep the verified workspace GUI accessible when the AppImage launcher enters
  the DLSS private `/tmp` namespace. Retain embedded-GUI identity verification and
  the original build/input/state checks.
- Publish regular Linux and Windows source/setup releases. Players still provide
  their own legally dumped input and generate game code locally; no game binary
  or private SDK is shipped. Windows game-build and full-game coverage remain
  incomplete. Frame generation remains separate, optional and experimental.

## 0.1.1-source-preview — November 7, 2026

- Repair fresh source builds on hosts without `ensurepip`: bootstrap pip from its
  exact official locked wheel inside the workspace, with useful failure messages.
- Restore the omitted licensed bcdec header required by keyboard button prompts;
  add a source-kit relative-header closure check for both ports.

- Add a one-command Linux local build: `./Build_Monado_II.sh` checks prerequisites
  and input, installs locked workspace tools/dependencies, generates game code and
  builds the game, launcher and a local **Monado-II-x86_64.AppImage**.
- Create an empty `game_files/exefs` and `game_files/romfs` layout on the first
  explicit build if absent, then stop for the player's own extracted input.
  Existing folders or links are never filled or overwritten; external input can
  remain in place through a symlink.
- Add stage progress/logs/receipts, fresh outputs and protection of previous
  launcher images/builds and saves when rebuilding.
- The AppImage contains the native launcher and uses its sibling build workspace.
  Existing game/input/state/lease/receipt checks remain in effect; the bundled GUI
  must exactly match the verified original build. No game-derived data is included.
- Add `./Play_Monado_II.sh` to open the generated image without requiring FUSE.
  Opening the launcher and pressing Play remain user actions.
- Replace the long Linux command sequence with setup/build/play instructions,
  prerequisite guidance, external-input linking and honest workspace requirements.
  Optional DLSS/FG setup remains separate; Windows backports remain pending.
- Keep all generated AppImages, staging directories and filesystem payloads out
  of tracked source/releases. This download remains a source/setup kit; the
  launcher AppImage and game executable are generated locally.

## 0.1.0-source-preview — November 7, 2026

First public Monado II source preview, continuing **TheMonadoGuy / Monado
contributors' Monado** and **Peiikai's LASRecomp**. See [credits](CREDITS.md).
Separate Linux and Windows source/setup kits are provided. Neither kit is a
precompiled playable game; each player must build locally using their own legally
dumped DE 2.0.0 files from their own legally obtained Switch and game.

### Linux additions and improvements

- **Native Linux x86-64 port:** Linux fibers, thread-local and floating-point state
  handling, synchronization, platform services, reproducible local build tooling,
  and SDL/Vulkan integration without Wine or Proton.
- **High-refresh timing corrections:** menu/interface animations use elapsed game
  time, and held-direction navigation no longer accelerates with rendering FPS.
  This includes title/menu navigation and battle arts selection. The corrections
  adapt to the actual frame timing, rather than targeting only 165 FPS.
- **Menu navigation speed setting:** choose Normal (30 FPS style, default) or
  Fast (60 FPS style). This changes held menu/arts navigation cadence without
  changing world speed or animation speed.
- **High-FPS footstep correction:** animation phase and walk/run threshold repairs
  address the repeated, rapid footstep/grass sounds reported at high refresh rates.
- **30 FPS cutscene limiter correction:** retain the original cutscene period
  when a higher or fractional output rate is selected.
- **Native DLSS Super Resolution and DLAA:** optional NVIDIA NGX/Vulkan integration,
  native-resolution DLAA handling, temporal-history resets and resource-lifetime
  safeguards. SDK binaries/models are not bundled; optional setup is separate.
- **Working shader precompilation:** launcher progress, Stop, explicit retry and
  resume using locally recorded variants. Completed, partial, cancelled and failed
  runs have distinct results. Cache writes are checked and atomic; cancellation
  preserves useful completed work. This does not promise every shader or no hitches.
- **Experimental AMD frame generation:** optional FSR 3 interpolation, including a
  cutscene-only setting and compatibility with DLSS/DLAA. Off by default. Reported
  gameplay is slow and visually glitchy; leaving it off is recommended.
- **Redesigned native launcher:** responsive navy/crimson/cyan styling, embedded
  licensed fonts, clearer sections and action buttons, bilingual controls, and a
  larger 1267×804 default opening size. Existing settings and input-remapping flows
  are retained. The public preview uses a neutral Monado II wordmark; the local
  scenic artwork and sword icon are excluded pending redistribution review.
- **Safer launcher settings and state:** preserve custom/unknown settings, save
  configuration atomically, reload after game exit, and keep configuration, logs,
  caches and saves separate from read-only game input. A profile lease prevents
  conflicting game/precompiler sessions; Linux process/filesystem isolation is
  checked before launch.
- **Runtime correctness repairs:** applet focus/lifecycle and graceful close,
  IPC validation and scheduler synchronization, explicit save/commit error
  propagation with journal retention on failure, checked Vulkan host-memory
  import, and shader/pipeline-cache validation and safer persistence.

These are targeted corrections, not a claim that all high-FPS behavior or save
recovery is solved. See [platform status](docs/PLATFORM_STATUS.md) and
[remaining work](TODO.md).

### Windows source kit

- Preserves the original Windows Monado implementation and its attribution,
  including its existing DLSS/DLAA, AMD frame generation and shader precompilation.
  Those features originated upstream.
- Adds Monado II project documentation, build guidance and source-release
  safeguards. The public snapshot accepts plain TTF/OTF/TTC font input and omits
  proprietary-font decoding on both platforms.
- **The Linux gameplay fixes and launcher redesign have not been backported to
  Windows.** They are tracked in [TODO](TODO.md). This fork has not independently
  built or gameplay-tested the Windows port; Windows tooling CI is not a game-build
  validation.

### Publication and known limits

- Explicit reviewed source manifests, content/provenance checks, Linux/Windows
  authored tooling fixtures, deterministic platform archives, readback verification
  and SHA-256 checksums protect the source-only release boundary.
- No game input, generated game code/executables, translated game shaders, caches,
  keys, firmware, saves, private SDK binaries or uncertain artwork is included.
  The project will not provide game files or tell anyone where to obtain them.
- Linux manual testing reached opening cutscenes, battles and first Shulk control;
  DLSS and shader precompilation received positive feedback. Full DE, Future
  Connected, long sessions, save/recovery, other hardware and clean-machine builds
  remain incompletely validated. Public-only font/theme changes need further
  integrated validation.
- Linux FSR 3.1 Super Resolution, NVIDIA native frame generation, DLSS 5 and Ray
  Reconstruction are not implemented. Frame-generation repairs and Windows
  backports remain future work.
