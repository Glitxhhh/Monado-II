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
**Frame generation is experimental, slow/glitchy in reported gameplay and should
normally remain off.** Linux FSR3 Super Resolution is unavailable.

Windows retains the original Monado implementation. Its original author reported
whole-game playability on NVIDIA RTX, including Future Connected. This fork has
not independently built or run that Windows source. The Linux improvements listed
in [TODO](../TODO.md) have not been backported.

The public launcher restores the exact original generated landscape artwork;
[provenance](../Monado_Linux/tools/launcher/assets/ARTWORK_PROVENANCE.md) records
its origin and limitations. It is not a game screenshot or extracted asset. The
separate unreviewed sword icon and proprietary-font decoding remain excluded.
Plain user/system TTF/OTF/TTC font input is supported. Public-only changes require
further integrated testing. Full-game, save-recovery, long-session, other-GPU and
clean-machine coverage remain incomplete. A regular 0.1.2 release does not imply
Windows game-build or full-game qualification.
