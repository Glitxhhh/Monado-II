# Monado II Linux launcher

The public source snapshot uses a navy/crimson theme, a **MONADO II** wordmark,
generic Roman numeral motif and embedded licensed DejaVu fonts. The original
generated landscape artwork is restored on the left; its exact asset and origin
are documented in [artwork provenance](assets/ARTWORK_PROVENANCE.md). The separate
local sword icon remains excluded. Startup is 1267×804 logical pixels; the window
remains resizable with a 640×480 minimum.

From the kit root, run `./Build_Monado_II.sh` after placing your own extracted
input in `game_files/exefs` and `game_files/romfs`. Then `./Play_Monado_II.sh` opens
the generated launcher AppImage using the verified sibling workspace, without
requiring FUSE. See the [Linux guide](../../../docs/BUILD_LINUX.md). The image
contains only the launcher and must stay beside its local builds/dependencies.
Build/packaging checks never open a game or visible launcher; Play is manual.

Supported settings are saved under the private Linux play profile, with game files
kept read-only. Shader preparation is optional, with progress/Stop/explicit retry.
The one-command Linux build includes the native DLSS/DLAA provider by default,
fetching the pinned official NVIDIA SDK locally. Selection requires compatible
NVIDIA hardware/drivers and runtime capability checks. `--without-dlss` explicitly
omits the provider; see the Linux guide for its prerequisites. Frame generation
remains a separate optional experimental build feature, may be slow/glitchy, and
should normally stay off.

No game files, game-derived executables/caches, private SDK binary, Nintendo font
or user data is included. See [project requirements](../../../README.md),
[platform status](../../../docs/PLATFORM_STATUS.md) and [TODO](../../../TODO.md).
Public-only launcher changes have component/hidden-UI evidence, not full-game or
Windows qualification. Credits and license notices remain with the source.
