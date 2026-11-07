# Changelog

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
