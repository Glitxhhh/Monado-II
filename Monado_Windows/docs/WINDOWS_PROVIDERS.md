# Local Windows graphics SDKs

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

Use **Use local NVIDIA DLSS SDK** or **Use local AMD FSR3 SDK** to choose your own
complete SDK folder. Before the first build the source is selected for Build.
For an existing installation, the manager validates it and creates a fresh runtime
using compatible checked local game code. Only a completed checked build replaces
the active runtime selection. The stable profile, saves, settings, old runtime and
unknown files remain in place. Future builds/updates revalidate the selected local
source; changed or missing files cause a clear failure rather than a silent
provider downgrade.

DLSS use is subject to NVIDIA's license. The picker displays the exact local
`nvngx_dlss.license.txt`, offers **Open full license**, and requires an explicit
**Use local DLSS SDK** choice. Cancel leaves the previous selection unchanged.
Neither setup nor the launcher downloads vendor files, accepts terms silently,
installs a driver or changes NVIDIA App settings. Source/public portable payloads
exclude these private vendor binaries and their staged copies.

The local checker accepts the official SDK root or one unambiguous complete DLL
folder. FidelityFX requires `amd_fidelityfx_vk.dll` with the five Vulkan FFX entry
points and its license. Streamline requires `sl.interposer.dll`, `sl.common.dll`,
`sl.dlss.dll`, `nvngx_dlss.dll`, the SDK license and the DLSS license. Required
dependencies are checked without loading vendor code. Bounded local copies and
license identities are recorded in `windows-providers.json` and the completed
native receipt. System dependencies remain system files.

Source/BAT builds can explicitly supply `--fidelityfx "C:/Local AMD SDK"` and
`--streamline "C:/Local NVIDIA SDK"` to the complete fresh native build recipe.
Read and agree to the vendor's actual license before requesting its use. A source
build without an authenticated portable manager gives this fallback guidance;
it never discovers or executes an arbitrary manager from settings or PATH.

These integrity and staging checks do not establish image quality, real provider
output, frame generation, gameplay, resize/reboot or performance qualification.
