# Local Windows graphics SDKs

**Windows DLSS/DLAA is experimental.** The maintainer's latest manual test
still reports purple flashes and flickering after the temporal camera and SDK
corrections. This rendering issue remains unresolved. Native builds, shader
preparation and authored SDK/GPU checks do not establish a gameplay image-quality
pass. Windows development is paused at the maintainer's request; the accepted
Linux implementation is unchanged.

The settings launcher shows which optional SDKs were staged with its actual
runtime. That identifies local files; the game still checks provider entry points,
GPU capability, temporal inputs and dispatch results before activation. An SDK
selection does not change the chosen upscaler or enable frame generation.

Choose **Manage local graphics SDKs and updates** to open the checked portable
manager for this installation. Close game/settings windows before applying a new
runtime. Reopening the portable setup normally opens settings and leaves its
management window available, minimized in the taskbar. `--manage` opens management
without automatically opening settings; `--manage --installation "C:/My install"`
selects one previously checked installation explicitly.

Use **Set up NVIDIA DLSS** to download the pinned official Streamline 2.14.16 SDK
or choose an existing local SDK. The download checks the exact archive size and
SHA256 and extracts only the production provider/dependency files and licenses
into private builder state. It reports **downloaded, unselected**. Cancellation
or failed verification never selects a provider. No SDK code runs during download.
**Use local AMD FSR3 SDK** continues to select an existing complete local folder.
Before the first build the authorized source is selected for Build.
For an existing installation, the manager validates it and creates a fresh runtime
using compatible checked local game code. Only a completed checked build replaces
the active runtime selection. The stable profile, saves, settings, old runtime and
unknown files remain in place. Future builds/updates revalidate the selected local
source; changed or missing files cause a clear failure rather than a silent
provider downgrade.

DLSS use is subject to NVIDIA's license. The picker displays the exact local
`nvngx_dlss.license.txt` and the dependent Vulkan helper's `reflex.license.txt`
when that helper is supplied. **Open full license** opens each exact original
file, and **Use local DLSS SDK** explicitly authorizes the listed terms. Cancel
leaves the previous selection unchanged. The Reflex agreement is Windows-1252;
its preview decodes that encoding while the original bytes and SHA remain intact.
Setup never accepts terms silently, installs a driver or changes NVIDIA App settings.
Source/public portable payloads
exclude these private vendor binaries and their staged copies.

The local checker accepts the official SDK root or one unambiguous complete DLL
folder. FidelityFX requires `amd_fidelityfx_vk.dll` with the five Vulkan FFX entry
points and its license. Streamline requires `sl.interposer.dll`, `sl.common.dll`,
`sl.dlss.dll`, `nvngx_dlss.dll`, the SDK license and the DLSS license. Streamline
2.14.16's `sl.common.dll` also delay-imports `NvLowLatencyVk.dll`; its exact helper
and license are included in that dependency closure. Unimported unrelated
plugins/helpers are not staged. Required
dependencies are checked without loading vendor code. Bounded local copies and
license identities are recorded in `windows-providers.json` and the completed
native receipt. System dependencies remain system files.

Source/BAT builds can explicitly supply `--fidelityfx "C:/Local AMD SDK"` and
`--streamline "C:/Local NVIDIA SDK"` to the complete fresh native build recipe.
Read and agree to the vendor's actual license before requesting its use. A source
build without an authenticated portable manager gives this fallback guidance;
it never discovers or executes an arbitrary manager from settings or PATH.

The settings launcher distinguishes requested DLSS from local file presence.
The game's F1 panel reports the actual temporal dispatch/fallback and exact SDK
failure reason. Missing files, unsupported GPU requirements, incomplete required
Vulkan hooks and failed frame dispatch never become an active-DLSS claim merely
because DLSS was selected in settings.

These integrity and staging checks do not establish image quality, real provider
output, frame generation, gameplay, resize/reboot or performance qualification.
