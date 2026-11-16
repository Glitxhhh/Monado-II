# Platform status: Linux 0.1.23 and Windows Experimental 0.1.0

Monado II provides native x86-64 SDL/Vulkan implementations. Each player builds
from their own extracted Definitive Edition2.0.0 input. Downloads contain no game
input, generated game-code library, game shader cache, saves or keys.

| Platform | Current status |
| --- | --- |
| Linux0.1.23 | Maintainer-accepted desktop/NVIDIA dev104R5, with current renderer, frame generation, graphics controls and portable graphical setup. Physical ultrawide and handheld performance testing remains requested. |
| WindowsExperimental0.1.0 | Native portable setup, modern launcher and gameplay options. Native builds and title startup were tested on Windows11/RTX4070SUPER/NVIDIA595.97. Purple DLSS/DLAA/temporal flashes and flickering remain reported. |

## Linux packaging hotfix in 0.1.23

Fresh update workspaces prepare the private startup Python builder's pinned
prerequisite source/sysroot cache before requesting its offline build inputs.
This addresses a packaging-stage failure after game/native launcher compilation.
It retains source/archive verification and the previous selection on failure.

## Linux build hotfix retained from 0.1.22

Desktop-launched setup and updates no longer rely on the terminal's open-file
limit for the portable FFmpeg link. The dependency worker raises its own soft
limit within the existing hard cap, records the values and restores the soft
limit afterward. An insufficient hard cap produces a specific diagnostic.
System settings, launcher/game limits and gameplay behavior are unchanged.

## Linux changes retained from 0.1.21

- Native FSR3 temporal upscaling and optional experimental FSR4Lite preparation,
  while retaining NVIDIA DLSS/DLAA and saved quality choices.
- Experimental2× FSR frame generation with bounded asynchronous presentation,
  corrected display-refresh matching, logs and separate Game/Output/Generated
  counters. Optional NVIDIA Smooth Motion depends on installed driver support.
- Original-default graphics controls, optional vibration, GPU selection and a
  local support report. Optional features remain disabled unless selected.
- Renderer feedback visibility, scaled gathers, zero-sharpness and temporal
  history corrections. A competing render-resolution controller is addressed for
  non-original aspect modes, targeting issue12's GPU-load/high-FPS stretching.
- A pinned glibc2.39+ baseline, private source-built Python/SDL/media components,
  and a game-free authored runtime with locally generated game-code libraries.

Authored CPU, scheduler, service, save, renderer, GPU and setup fixtures provide
component evidence. They do not establish every scene, physical display cadence,
input latency, full-game completion or performance on every device. The maintainer
reported roughly5–10FPS lower performance in some comparisons after the broader
renderer changes; this was not a controlled benchmark. Performance recovery and
additional quality-control placement remain queued. Original16:9 stays the default;
non-original ratios and frame generation remain experimental.

The existing RADV memory path and RADV-only nodcc workaround remain. Thanks to
**patrickksmdev** for isolating and testing the RX7600 artifact workaround. NVIDIA
acceptance is not AMD/handheld qualification; report hardware and requested/effective
settings when sharing results. See [AMD notes](AMD_RADV.md),
[aspect choices](ASPECT_RATIOS.md), [frame generation](FRAME_GENERATION.md) and
[Linux setup](BUILD_LINUX.md).

## Windows experimental boundaries

Windows now implements applicable high-refresh timing, expanded saves, dedicated
Quick Step, cheats, native Exit, L2/party controls, aspect choices, on-screen FPS,
modern settings and graphics/provider adapters. Its branded portable EXE builds
locally, reopens the checked settings launcher, and has independent verified update
metadata. Linux-only providers and OS mechanisms are not promised on Windows.

Windows0.1.0 is independently versioned. **It remains experimental:** reported
purple temporal/DLSS flashes are unresolved; full-game, FutureConnected, ordinary
save/reload, long sessions, optional provider/model preparation, clean-machine
setup, other GPUs and handheld/ultrawide hardware need broader qualification.
An authored fixture or title screen is not a pass for those scenarios. The setup
is unsigned. See the [Windows guide](BUILD_WINDOWS.md),
[release materials](../Monado_Windows/docs/WINDOWS_RELEASE.md) and historical
[implementation requirements](WINDOWS_PORT_HANDOFF.md).

## Shared defaults and save ownership

Quick Step, cheats, vibration and frame generation remain off by default. Existing
settings and native save formats are preserved. Keep the complete save folder,
including hidden QuickStep metadata, when backing up or moving a profile. Optional
QuickStep may still have quirks in untested character/equipment/story combinations.
Exit does not autosave progress. Rebuilds and updates never include or upload user
input, saves or locally generated game code. Full DE/FutureConnected, recovery,
all-hardware and all-driver compatibility remain incompletely qualified.

Original project and component attribution remain in [credits](../CREDITS.md),
[notice](../NOTICE.md) and the per-component licenses. Historical downloads before
this Windows experimental release retain the original implementation and their
version-free Windows asset names.
