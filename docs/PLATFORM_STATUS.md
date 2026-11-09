# Platform status

Monado II targets native Linux x86-64 and Windows x86-64 with SDL/Vulkan. It is
not affiliated with Nintendo/Monolith Soft and includes no game files.

Linux development has run on Ubuntu26.04.1, Ryzen7900X and RTX4070SUPER with NVIDIA
595.99.02. The tested toolchain is CPython3.14, Zig0.14.1/Clang19.1.7 and a pinned
Ubuntu development sysroot. This is a tested development combination, not a broad
minimum-spec or portable binary compatibility promise.

Maintainer gameplay reached opening cutscenes, battles and first Shulk control;
high-FPS menus/repeat and reported footstep chatter were corrected. Optional shader
preparation was manually accepted. Native DLSS/DLAA had positive manual feedback;
synthetic output tests do not establish every game's temporal scenario.
The one-command Linux build includes the native DLSS/DLAA provider by default;
`--without-dlss` explicitly omits it. Selection still requires compatible NVIDIA
hardware/drivers and the existing runtime capability checks. Build prerequisites
are listed in the [Linux guide](BUILD_LINUX.md).
**Frame generation remains experimental and off by default.** The earlier
implementation was slow/glitchy in reported gameplay. Version0.1.5 fixes specific
warmup/history and deliberate pacing problems and adds an explicit local build
option. Manual testing found it mostly stable with remaining jitter/stutter and
preferred FG off. **Performance varies and may be worse with FG enabled.** Authored
tests do not establish improved physical display cadence, latency or all scenes.
Linux FSR3 Super Resolution is unavailable.

Version0.1.6 includes FG support in normal Linux builds and automatic rebuilds,
with the effect disabled in settings by default. Users enable it in the launcher
with DLSS/DLAA selected. Explicit DLSS-free builds omit both providers. This is an
availability correction, not a new pacing/performance fix.

Version 0.1.4 fixes Linux dependency flag handling in installation paths containing
spaces. It preserves existing input/profile locations and the variable-query
contract used by launcher setup.

Version 0.1.3 adds a Linux stable-release updater with source verification, local
build progress/cancellation and checked launcher restart. Presentation-only
updates can retain the compatible game; other source changes trigger a full local
rebuild. Original input and profile paths remain stable across managed versions.
Failure/cancellation during preparation preserves the previous selection, and
edited release-owned source stops installation. Full automated rebuilds of custom
footstep-diagnostic builds are refused safely. Current clients support ordinary
FG rebuilds; older experimental FG installations with pre-0.1.6 updater code need
a one-time manual upgrade.
The 0.1.2 launcher needs a one-time manual 0.1.3 source upgrade/build to gain
update support. The verified source kit can be applied to the same original
folder while keeping its profile/input/build directories; fresh kits in another
folder do not import old profiles. See [updating/rollback](BUILD_LINUX.md#4-update-and-roll-back).
This implementation does not establish broad clean-machine or interrupted-update
qualification; coverage limits below remain applicable.

Windows retains the original Monado implementation. Its original author reported
whole-game playability on NVIDIA RTX, including Future Connected. This fork has
not independently built or run that Windows source. The Linux improvements listed
in [TODO](../TODO.md) have not been backported.
There is no Windows updater in this release.

The public launcher restores the exact original generated landscape artwork;
[provenance](../Monado_Linux/tools/launcher/assets/ARTWORK_PROVENANCE.md) records
its origin and limitations. It is not a game screenshot or extracted asset. The
separate unreviewed sword icon and proprietary-font decoding remain excluded.
Plain user/system TTF/OTF/TTC font input is supported. Public-only changes require
further integrated testing. Full-game, save-recovery, long-session, other-GPU and
clean-machine coverage remain incomplete. A regular 0.1.7 release does not imply
Windows game-build or full-game qualification.
