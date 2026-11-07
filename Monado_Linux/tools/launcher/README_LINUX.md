# Monado II Linux launcher

The public source snapshot uses a neutral navy/crimson theme, a **MONADO II**
wordmark, generic Roman numeral motif and embedded licensed DejaVu fonts. Scenic
development artwork and the local sword icon are not distributed. Startup is
1267×804 logical pixels; the window remains resizable with a640×480 minimum.

Build locally using the [Linux guide](../../../docs/BUILD_LINUX.md), then invoke
`scripts/launcher_linux.py` with explicit paths to your newly built launcher/game.
Its `--dry-run` checks receipts/resources/input without opening a window or writing
normal runtime state. Removing `--dry-run` opens the launcher for manual testing;
Play is a separate user action. The dated developer defaults are not shipped builds.

Supported settings are saved under the private Linux play profile, with game files
kept read-only. Shader preparation is optional, with progress/Stop/explicit retry.
DLSS/DLAA requires an explicitly built/licensed native provider and compatible GPU.
Frame generation is experimental, may be slow/glitchy, and should normally stay off.

No game files, game-derived executables/caches, private SDK binary, Nintendo font
or user data is included. See [project requirements](../../../README.md),
[platform status](../../../docs/PLATFORM_STATUS.md) and [TODO](../../../TODO.md).
Public-only font/theme changes have component/hidden-UI evidence, not full-game or
Windows qualification. Credits and license notices remain with the source.
