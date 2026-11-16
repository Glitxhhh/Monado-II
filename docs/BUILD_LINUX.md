# Build and play on Linux

The recommended download is **`monado-ii-0.1.27-linux-builder-x86_64.AppImage`** from
[Releases](https://github.com/brendonbusker/Monado-II/releases/latest). It provides
folder selection, local build progress and the normal settings launcher after
building. The source/setup archive and terminal route remain available below.

## Graphical setup (recommended)

1. Verify the downloaded AppImage against the release's `SHA256SUMS`. Allow it to
   execute in your file manager's permissions, then open it.
2. Choose **Set up Monado II**. Select your own extracted **Definitive Edition
   2.0.0** folder containing `exefs` and `romfs`, then check the files. No game
   input is copied, moved or uploaded.
3. Choose an empty final installation folder. The default is `~/Games/Monado-II`;
   you can choose another drive or type a new folder path. Keep this location after
   building because native build receipts use absolute paths.
4. Choose whether to include NVIDIA DLSS support and prepare shaders. Advanced
   settings limit parallel build workers to 1–6. These choices do not enable
   experimental frame generation automatically.
5. Build. The window reports actual stages and compilation progress, with details
   and retained logs. When ready, open the settings launcher and press **Play**
   yourself. Later opens of the same AppImage return to that installation.

If FUSE is unavailable, extraction mode works without installing it:

```sh
chmod +x monado-ii-0.1.27-linux-builder-x86_64.AppImage
./monado-ii-0.1.27-linux-builder-x86_64.AppImage --appimage-extract-and-run
```

**Use an existing installation** links a checked installation in place, preserving
its original input, saves, settings and current update. If the input folder moves,
**Locate game files** checks its new location before reconnecting it; no immediate
rebuild or save conversion is required. The full [graphical builder guide](GRAPHICAL_BUILDER.md)
covers cancellation, persistent tool caches, relocation and update behavior.

The graphical AppImage includes CPython 3.14, so this entrypoint does not require a
separately installed Python. It downloads the compiler and pinned private build
dependencies as needed. It still needs compatible host tools and native libraries;
missing requirements receive diagnostics rather than an automatic host install.
The default portable baseline is **Linux x86-64-v2, glibc 2.39+**, and a native GNU C++ runtime
exporting `GLIBCXX_3.4.32` and the required C++ ABI versions. Host tools include
bash, pkg-config, dpkg-deb, binutils (`readelf` and `nm`) and squashfs-tools
(`mksquashfs` and `unsquashfs`). The DLSS build also needs `/usr/bin/bwrap`
(bubblewrap). GNU C++ is discovered through native library directories; an Ubuntu
multiarch path is not required. Compatible graphics, audio and session libraries
remain necessary. Build and run in the same compatible environment when using
Distrobox. No universal distro or handheld-gameplay compatibility is implied.

## Terminal/source setup

Download and extract the latest **`monado-ii-<version>-linux-setup.tar.gz`** into its final folder.
This route additionally requires a host **CPython 3.14**. Open a terminal in the
extracted folder and follow the input/build instructions below. Neither ordinary
builds nor background updates install system packages or run sudo.

The builder fetches hash-locked media, codec and other ordinary user-space
libraries into `Monado_Linux/.deps`. This includes libbluray 1.4.1 (package
`libbluray3` version `1:1.4.1-1`), supplying the `libbluray.so.3` ABI required by
the pinned FFmpeg build. The host may independently provide `.so.4`; keep your
normal system package. No downgrade, major-version symlink or individual-library
version override is needed or supported.
Host glibc, GNU runtimes, driver clients and native audio/session interfaces are
checked rather than replaced. The builder never runs sudo or changes system
packages during ordinary/noninteractive builds. Explicit dependency repair can
request a reviewed package transaction inside a supported Distrobox, as described
below. Drivers and the operating-system baseline are not installed by the build.
This remains an explicitly tested baseline rather than a universal Linux binary
compatibility promise.

### Prerequisite checks and guided repair

Run the same `./Build_Monado_II.sh` command. Before compilation, it reports the
Monado version, current distribution/container, libc baseline, missing tools and
required native libraries. Missing prerequisites include the corresponding
distribution package names and installation instructions. Python 3.14 is still
the build baseline; an available Python 3.9+ can run the dependency diagnosis.
`--help` works without Python and `--plan` never installs or downloads anything.

Inside a supported **Fedora 45 Distrobox**, request package installation with:

```sh
./Build_Monado_II.sh --install-deps
```

The script displays the container and exact package command. Review the package
manager's transaction and confirmation; sudo may ask for your container password.
Only that package command uses sudo. Generation, compilation and AppImage creation
continue as your ordinary user. Do not run the whole build script with sudo.
If no suitable Python is available, this mode first installs Fedora's Python 3.14
to run the fuller checks. An existing complete environment needs no transaction.
Supported private dependency downloads remain automatic and hash-checked.

Native installations receive manual package instructions. On native Bazzite,
`dpkg-deb` is supplied by Fedora's `dpkg` package through the supported layering
route:

```sh
rpm-ostree install dpkg
```

Reboot to apply that deployment, then rerun the build. The script does not layer
host packages, unlock SteamOS, reboot, install drivers or replace libc. A container
below the selected baseline (2.39 by default, 2.43 with `--legacy-runtime`) is
rejected with compatible-image guidance; installing more
ordinary packages does not make an old libc satisfy that baseline. Unknown images
get diagnostic information rather than guessed automatic installations.

Build and launch from the same compatible container. A file visible in the shared
home directory still needs the container's libraries when it runs. Actual Steam
Deck GPU, audio, controller and game coverage requires separate manual testing.

Background updater builds never install packages or ask for sudo. If a new
prerequisite is missing, the working selection stays active and the error explains
what to install before retrying. Original game input, saves, settings and optional
Quick Step metadata remain in their existing locations; no save conversion is
introduced by these setup changes. Build failures include bounded command/loader
diagnostics and the path to the complete log.

### Native prerequisites on Arch / CachyOS

Check `python3.14 --version` and `getconf GNU_LIBC_VERSION` before building.
Rolling distributions can change their default Python major version; this build
still requires CPython 3.14. Typical package providers are:

| Requirement | Arch-family package providers |
|---|---|
| Python, shell and native GNU runtimes | `python`, `bash`, `glibc`, `libgcc`, `libstdc++` (or the distro's equivalent GNU runtime packages) |
| Dependency and ELF tools | `pkgconf`, `dpkg`, `binutils` |
| Launcher packaging and DLSS isolation | `squashfs-tools`, `bubblewrap` |
| Native audio and window decoration | `alsa-lib`, `libpulse`, `libdecor` |
| Native graphics clients | `libdrm`, `mesa` (GBM client), `libglvnd`, `ocl-icd`, `libva`, `libvdpau`, `libvpl` |

Use your distribution's supported driver packages for your GPU. The host client
libraries remain necessary even if the movie path uses software decoding,
because the pinned media build links their client interfaces. The dependency
check reports missing SONAMEs, requester chains and incompatible ABI versions.
It does not install a GPU driver, replace libc or fabricate a library-major alias.
An older host needs a supported runtime baseline; changing a symlink cannot
supply a missing ABI. On Ubuntu the corresponding native runtime packages use
Ubuntu package names; the same SONAME/version checks apply.

`--plan` lists private runtime libraries and explicit host interface requirements.
Setup checks the complete declared private graph. A small authored load-only
probe runs before long game generation; the final game, launcher and native
companions are checked before a build succeeds. Play and shader preparation
repeat loadability checks, and an update that fails them keeps the previous
version selected. These checks do not start the game or establish GPU/gameplay
compatibility. Saves and existing shader caches are preserved on failure.

For Quiver or another desktop app manager, choose **`App_Monado_II.sh`** after
extraction. It uses an installed terminal for the initial build, then opens the
launcher without starting the game. Once built it opens the existing launcher.
The same prerequisites and input layout below apply; see [Quiver setup](QUIVER.md).

## 1. Put your own game files beside the build command

Use your own legally dumped **Definitive Edition 2.0.0** files from your own legally
obtained Switch and game. Only already-extracted folders are accepted. No game
files, keys, links or dumping instructions are provided.

```text
Monado-II/
├── App_Monado_II.sh
├── Build_Monado_II.sh
├── Play_Monado_II.sh
├── game_files/
│   ├── exefs/
│   └── romfs/
└── Monado_Linux/
```

If `game_files` does not exist, running the build command creates this empty input
layout and stops with placement instructions before downloading tools. Put your
files there, then run the same command again. Existing input is never populated,
moved or overwritten by the builder. Game input is not a cache or save location.

Already keep your files elsewhere? Before the first build, link the existing folder
instead of moving or duplicating it:

```sh
ln -sT "/absolute/path/to/your/game_files" game_files
```

The linked folder must contain both `exefs` and `romfs`. This creates only a link in
the kit; it does not move the original files. Do not copy development builds,
virtual environments, caches or saves into the new kit.

## 2. Run one build command

```sh
./Build_Monado_II.sh
```

The first complete build downloads hash-locked Python/compiler tools, pinned
graphics development packages, the pinned official NVIDIA SDK and the checked
official AppImage runtime into this workspace. It checks the game version,
generates game code locally, builds the
game and native launcher, then creates **`Monado-II-x86_64.AppImage`** at the kit
root. Progress, logs and a receipt identify each stage; the command stops at the
first failure. It never starts the launcher or presses Play.

Graphics package downloads retry temporary connection failures with bounded
attempts. Managed updates reuse exact hash-verified package archives from the
original installation when available. A failed download keeps the previous
version selected; its log identifies the package and final error. Certificate,
size/hash and invalid-cache failures remain errors.

The normal build includes native **DLSS Super Resolution / DLAA** support. Select
it in the launcher on compatible NVIDIA hardware and drivers; runtime capability
checks still apply. The SDK retains its own license and stays outside the source
kit and launcher image. SDK setup failures stop the build instead of silently
removing the feature. To explicitly build without this provider and its
DLSS-specific prerequisites:

```sh
./Build_Monado_II.sh --without-dlss --framegen
```

Selecting DLSS in the Linux launcher now defaults to **DLAA / Native (100%)**.
An explicitly saved quality choice is preserved when reopening settings.
**Shaking reported at Quality (67%) remains unresolved**; choose Native (100%) as
the current workaround. This release changes the selection default without
claiming a scaled-DLSS rendering fix.

To inspect the proposed steps without creating files or downloading anything:

```sh
./Build_Monado_II.sh --plan
```

Builds use fresh output directories. Repeating the command preserves previous
builds and saves; an existing root AppImage is replaced only when its previous
receipt and contents verify, with a retained previous copy. An interrupted or
failed build is not reported as successful. A retry may recompile game code;
downloaded verified dependencies can be reused. Do not rename or delete build
outputs while building or playing.

## 3. Open the launcher

```sh
./Play_Monado_II.sh
```

This runs the generated launcher AppImage using its extraction mode, so FUSE is
not required. On desktops with working FUSE, the AppImage can also be opened
directly after allowing executable files in the file manager. Settings and Play
remain ordinary launcher actions. Configuration, logs, caches and saves stay in
`Monado_Linux/play/launcher`, outside `game_files`.

The launcher includes the original generated landscape artwork on the left. See
its [provenance](../Monado_Linux/tools/launcher/assets/ARTWORK_PROVENANCE.md) for
the reviewed asset and rights limitations.

**Keep the complete extracted folder.** This is a workspace launcher AppImage,
not a standalone game bundle: its native launcher payload uses the checked local
game build, scripts, Python environment and graphics libraries beside it. Moving
the folder invalidates absolute build receipts; rebuild in the new location.
The image contains no game input, generated game executable/code, caches or saves.
Do not upload local generated outputs as a project release.

Starting with 0.1.6, a normal Linux build includes experimental frame-generation
support. Its launcher method is Off initially; users choose whether to enable
it. Real gameplay smoothness still needs testing. This source/setup release requires local game
generation. See [platform status](PLATFORM_STATUS.md) and
[remaining work](../TODO.md) for features and validation limits.

Installation paths containing spaces are supported starting with 0.1.4. An older
0.1.3 source kit can fail in dependency compiler flags in such a folder; use the
latest source kit in the same original installation folder. Keep input/profile
directories in place.

### Expanded save slots

Linux 0.1.9 provides 99 manual slots and a separate autosave for each story.
Main-game and Future Connected saves remain separate. Save/Load keeps four native
cards visible and scrolls through the remaining slots; unused slots show No data
without creating files. Existing saves retain their native format and filenames.

Initial Save/Load scanning batches ready native read steps within a small budget
and yields for pending I/O and callbacks. Menu, startup and Continue scans can
still take time, especially with many populated slots. The maintainer accepted
the development build manually; full-capacity and recovery coverage remain
incomplete. See [platform status](PLATFORM_STATUS.md).

### Dedicated Quick Step slot

**Dedicated Quick Step gem slot** under Mods in the launcher is stable in Linux
0.1.11 following manual acceptance. It is **off by default** for players who prefer
the original equipment setup. The character-owned slot uses eligible owned Quick Step
I–VI without consuming an ordinary armour slot, including when no boots or
socketless/unique boots or drones are equipped. It retains normal ownership,
potency and caps; optional assignment metadata is paired with each native save.
Back up the complete save folder, including hidden files, to retain assignments.

Load Game and Continue restore the saved assignment and its actual movement
bonus from matching metadata. Native `.sav` formats and filenames stay unchanged.
See the [Quick Step guide](QUICKSTEP.md) for controls, removal and coverage limits.

### Held-L2 camera and party controls

Linux 0.1.12 restores the native held-L2 camera controls and control-help display
when the optional Xenoblade 3-style character-switching setting is enabled.
Hold L2 and press L1/R1 to request the previous/next party member; a neutral L2
hold still reaches the game's native camera controls. Optional D-pad walking
yields to held-ZL camera controls. The maintainer accepted the revised controls
manually; broader field/battle, Future Connected and controller coverage remains
incomplete.

## Experimental frame generation

Normal Linux builds include native frame-generation support without an extra flag:

```sh
./Build_Monado_II.sh
```

This uses compatible temporal input from DLSS/DLAA, native FSR3 or prepared
FSR4 Lite. Use `--without-dlss --framegen` to include FSR3 and integrated
frame-generation support without NVIDIA DLSS. An explicit `--framegen` request
requires DLSS or FSR3. A managed update with no explicit selection preserves
its verified currently selected game's compiled support. This keeps older
updaters compatible and retains support in newer FSR-only installations. Fresh
source builds retain the modern default, and the graphical builder requests
support explicitly; the runtime option remains
off by default. The `--framegen`
argument remains accepted for compatibility but is unnecessary.
The pinned FidelityFX source is built locally when missing. A verified local SDK
is reused; an altered SDK stops the build for inspection. No SDK binaries are
included in source releases. A launcher AppImage may carry the verified authored
runtime and MIT FidelityFX components, while game code, NVIDIA SDK and FSR4 models
remain local. Runtime frame generation stays **off by default**. In the launcher,
select a working temporal upscaler and then choose
**FSR 3 interpolation (2×, experimental)**. Optional display-refresh matching
limits real FPS to half the display refresh, subject to your saved FPS ceiling.
For example, 180 Hz with a 120 FPS setting requests up to 90 real / 180 output.
It does not provide 60-to-180 or 120-to-180 arbitrary-phase generation.

The FPS setting describes the real-frame ceiling. The on-screen counter separates
**Game** from successful **Output (submitted)** and classified **Generated**
presentation requests; output
is not measured physical scanout. Generated images add GPU work and do not advance
gameplay, input or audio. **Performance
varies: it may run worse, stutter or show visual glitches. Leaving FG off is
recommended for normal play.** At high native frame rates it may offer little
benefit. Compare the same scene/settings with
FG off/on after shader and SDK warmup. Component passes do not qualify physical
presentation, latency, visual quality or every game scene. Maintainer testing of
0.1.5 preferred FG off despite mostly stable gameplay; no broad improvement is claimed.

When supported NVIDIA Smooth Motion driver components are installed, the launcher
also offers that separate 2× method for supported RTX 40-series or newer GPUs.
It requires restarting the game, is never stacked with integrated FSR, and shows
**Output (driver submitted)** when its scoped observer can measure downstream
requests. Generated-only NVIDIA identity remains unavailable; unsupported
measurement is labelled **Output external (unavailable)**. No driver changes are made. See [frame-generation methods and testing](FRAME_GENERATION.md).

Launcher-only updates preserve a compatible FG game. An older DLSS build without
FG support is rebuilt to gain it. Current clients can rebuild ordinary FG builds
automatically; footstep-diagnostic builds still require a manual rebuild.

<a id="4-update-and-roll-back"></a>

## 4. Updates and storage

**The 0.1.2 launcher cannot update itself.** Enable the updater with one manual
0.1.3 or newer source upgrade/build. You can preserve your current profile in the same
original installation folder:

1. Close its launcher, game and shader precompiler. Download the official latest
   Linux source/setup kit and compare its SHA-256 with the published release
   checksum. Extract it into a separate temporary folder first.
2. From that clean extracted kit, run
   `python3.14 -I -B tools/publication_guard.py .`. This checks the source-only
   manifest and contents. Do not run this clean-tree check against your live
   installation, which contains private builds and state.
3. Merge the verified kit's **source-file contents** into your existing original
   installation root, replacing corresponding source files while retaining its
   directories. The kit contains no `game_files`, `Monado_Linux/play`, build,
   generated, dependency or cache directories. Leave those existing directories
   and any input link untouched. Do not delete/replace the installation folder
   or `Monado_Linux`, and do not use `rsync --delete`. If you edited release-owned
   source, merge those changes manually before rebuilding.
4. Run `./Build_Monado_II.sh` from that original root, then
   `./Play_Monado_II.sh`. The build uses fresh outputs and keeps the original
   `Monado_Linux/play/launcher` saves/settings in place.

Alternatively, a fresh kit in a new folder is a separate installation: it does
not automatically import an older folder's saves/settings. Its game-input link
can still point at your existing extracted files. No save/profile copying is
needed for the same-folder upgrade above.

After that bootstrap, normal Linux launcher startup quietly checks the official
GitHub stable release. Only a newer supported stable release opens an offer with
**Update** and **Later**. Later leaves the current installation usable. Update
downloads and verifies the source kit, then builds locally with progress and a
**Cancel** action. Download/build/validation failure or cancellation keeps the
previous version selected. A successful update restarts the launcher after its
current session exits; it never launches the game automatically. Updates cannot
be applied while the game or shader precompiler is running.

Presentation-only updates retain a compatible verified game build and rebuild
the launcher. Runtime, generator or other unrecognized source changes require a
full local game rebuild. The existing DLSS build choice is preserved; standard
builds keep DLSS/DLAA enabled and include FG support. The setting remains off
unless the user enables it. Clients on 0.1.6 or newer can automatically rebuild
ordinary FG builds. Older experimental FG builds using 0.1.5 or earlier updater
code still require a one-time manual upgrade because their old client refuses
that rebuild. Footstep-diagnostic builds remain manual. Frame generation remains
experimental. Edited release-owned source files also stop automatic installation
so local changes can be merged manually. Normal settings changes do not modify
the release-owned source.

**Updating to 0.1.19 requires a full local game rebuild** for the controller and
game-camera changes. The normal updater performs this rebuild before activation.
It preserves the original
`Monado_Linux/play/launcher` save/settings profile and shared input in place;
it does not convert, replace or delete saves. Preserve optional Quick Step metadata
with the complete save folder, including hidden files. Keep the original installation
folder and its current managed directories. A fresh kit elsewhere is a separate
profile, as described above.

Managed updates retain **only the current launcher version and its required game
payload** after successful activation and cleanup. The working version stays in
place while the candidate downloads, builds and passes the available checks. A
failed or cancelled preparation leaves it selected and usable. Allow temporary
space for the existing build, download, generated sources and replacement build
before cleanup can reclaim it.

The first launch of a version supporting this policy also cleans accumulated old
updates and failed attempts from older installations. Cleanup removes recognized,
unmodified updater-owned artifacts only. If another session is using them, or a
permission/unexpected file prevents safe deletion, cleanup is recorded as pending
and retried on a later launch. Close other launcher/game/precompile sessions to
allow it to finish. Its bounded status is in
`Monado_Linux/updates/retention-status.json`.

This first-start maintenance runs before the launcher window appears. An isolated
upgrade rehearsal with a long update history took about **75 seconds** for
maintenance and post-checks; other installations may take more or less time.
Leave the launcher starting while cleanup works. A deletion that cannot safely
finish is left pending for a later launch.

Launcher-only updates reuse one directly referenced game payload. Game-changing
updates build a fresh payload at a permanent path before activation. Existing
version-owned game binaries are detached through a one-time full local rebuild
when first needed by the new updater. In particular, an older updater can build
0.1.12's game inside its version workspace, so the following update may need
one additional full rebuild to create the reusable payload. Later compatible
launcher-only updates can reuse that payload. Build paths stay fixed; never
manually move or rename an installed source/build/image directory.

The original root AppImage remains the entry point and selects the checked active
version. Keep the original folder, its Python support environment and the current
managed directories together. Ordinary host-Python source installations retain
that workspace launcher on update. Installations using the graphical builder's
persistent private Python can package a full standalone setup image on update.
A newly downloaded graphical AppImage can also adopt an existing installation in
place. None of these paths turns the installed game into a single-file game image.
The updater preserves original authored source and support files.
Downloads, native outputs and generated content under `Monado_Linux/updates` stay
local and must not be committed or included in source releases.

Saves, settings, mods, useful shader/driver caches and shared `game_files` are
outside version cleanup. Native save formats are unchanged. Preserve the complete
save folder, including hidden Quick Step metadata. The original folder's
`update-version.json` reflects the active version; its checked original metadata
backup supports source verification rather than retaining an old playable build.

**Local rollback is discontinued.** The legacy `Rollback_Monado_II.sh` compatibility
entrypoint reports this instead of changing the selected version. To downgrade,
download an older Linux source/setup kit from the project's GitHub releases and
build it manually. Keep your original installation/profile intact; a kit in a
new folder uses a separate profile and does not automatically import saves. Back
up the complete save/settings profile before manually transferring it. The updater
never restores or deletes user progress as part of changing program versions.
If the remembered game-input folder disappears, close other sessions and reopen
Monado II. **Locate game files** accepts the supported extracted folder at its new
location and checks its executable hashes against the existing build before
reconnecting only known input links. Cancel or a mismatch preserves the old
selection. It keeps the original profile and does not immediately rebuild the game.
A later update may rebuild once to record that path; native build receipts are
not rewritten. An existing ordinary input directory is never replaced. Moving
the complete installed build remains a separate action requiring a fresh build
at its destination.

The updater remains Linux-only; no Windows updater/backport is included.

### A 0.1.17 update fails at `package-appimage`

Linux 0.1.17 can report `source already contains generated release-only files`
while packaging its replacement AppImage, even after the game and native
launcher build successfully. The downloaded source kit already contains checked
distribution metadata; the full-image update path incorrectly passed it to a
packager that accepts only clean authored source. This is a packaging defect,
not evidence of missing game files or dependencies.

Version 0.1.18 verifies that metadata and creates a normalized temporary export
before repackaging. The strict source checks remain enabled. This also applies
to older installations that have migrated to persistent private Python; it is
not limited to users who first installed through the graphical AppImage. Fresh
graphical setup follows a separate initial-build path.

Use 0.1.18 or later for the next update attempt and keep the existing installation
and profile in place. A failed candidate is not activated, so the previous
working version remains selected. Preserve the reported build log if packaging
still fails. Deleting a separate installation under `~/Games` does not cause or
fix this specific error. Do not delete private Python caches or existing build
directories as a workaround; they may support the active launcher. No save
conversion or game feature change is needed for this correction.

If a stage fails, use the log path printed by the builder. A successful local build
and AppImage check do not establish full-game or other-GPU compatibility. The
project will not provide game files or tell you where to obtain them.


## Optional FSR4 Lite and quality controls

Graphics includes native FSR3 and an optional experimental FSR4 Lite path. Use
**Prepare FSR4 Lite (optional)** in the launcher to prepare its models locally.
Preparation needs an installed compatible Proton and may download pinned tools
and the official AMD SDK. It has progress, cancellation and retry; it does not
start gameplay or install Proton/system packages. Normal builds compile the
optional native module but do not download or select models automatically.
`--without-fsr4` omits the module. GPU/model/size checks retain an existing
upscaling fallback when the optional path cannot run.

The launcher and F1 share additional quality controls. Original settings remain
the default, and options requiring a restart are labelled. Quality presets keep
your upscaler, generation, FPS, gameplay options and saves. Controller vibration
is opt-in. See [frame generation](FRAME_GENERATION.md) for counters and testing.

New builds separate the authored runtime from your locally recompiled game code.
A verified bundled runtime can avoid recompiling the authored portion; compatible
updates can reuse your local game-code library. Input, source, compiler and ABI
checks decide reuse. A verified incompatibility falls back to source compilation;
corrupt content stops with a clear error. `--source-build` on the standalone
AppImage explicitly uses its full source workflow. Existing saves and input
locations remain in place. The source archive remains a supported fallback.


### Desktop build file limits

Linux 0.1.22 fixes `ProcessFdQuotaExceeded` while linking the portable FFmpeg
libraries during setup or updating. Desktop sessions can start applications with
a lower soft open-file limit than terminals. The portable dependency worker now
raises only its own soft limit within the inherited hard limit and restores it
afterward. It does not change the desktop session, system configuration or game
process limits. If the hard limit is insufficient, the build reports the actual
values instead of requesting unrelated dependency packages.

If an earlier update failed at this step, reopen the launcher and retry the new
update. The failed build leaves the previous installation selected and preserves
saves, settings and extracted game input.


### Fresh update workspace prerequisites

Linux 0.1.23 also fixes a cold-cache failure later in launcher AppImage packaging.
The private startup Python builder now prepares its pinned prerequisite
source/sysroot cache before requesting the offline build inputs. A previous game
and native launcher compile could succeed before this later packaging step failed.
The failed update still leaves the prior selection active; retry the new release
through the launcher. Game input, saves and settings remain in place.


### Portable launcher packaging after runtime reuse

Linux 0.1.24 keeps the portable build selection when the updater reuses a verified
local runtime. Previously that CLI path could build the native launcher against
a newer host glibc, then fail the AppImage's glibc 2.39 compatibility check. The
default now uses the portable baseline consistently for fresh builds and reuse;
the explicit `--legacy-runtime` opt-out is retained.

This release also retains the file-limit and cold-cache prerequisite fixes
described above. If an update to 0.1.21, 0.1.22 or 0.1.23 failed, retry the new
release through the launcher. These corrections do not require a system upgrade
or a change to game input, saves or settings.


### Cross-distribution dependency preparation and launcher startup

Linux 0.1.26 separates host build tools from the pinned target development files.
This fixes Fedora package-tool wrapper failures that could make SDL report missing
Wayland support, and prevents unrelated host headers from enabling an unsupported
optional liburing build. These cases need a corrected build recipe rather than
extra random system libraries. The earlier file-limit, Python prerequisite and
portable launcher packaging corrections are retained.

Dependency preparation reports its current operation and elapsed time. Compilation
can take longer on handhelds; an active phase is not by itself a failure. On a
failure, the outer log includes the end of the failing component log. Full SDL
logs are retained under `Monado_Linux/.deps/portable/build/deps-*/logs/`, including
`sdl-configure.log` and `sdl-build.log`. Private startup-Python logs are under
`Monado_Linux/.deps/portable/python/*/logs/`. In a staged update, these paths are
inside its candidate workspace. Review logs before sharing them.

Launcher startup reuses checks only within a protected operation and for exact
verified bytes. Source/runtime checks, changed-file rejection, installation leases
and existing save formats remain in place. Retry through the existing launcher;
failed preparation keeps the previous working build selected.


### Older non-DLSS updater compatibility

Launchers from before the native FSR3 update cannot accept an FSR-only
frame-generation capability in the rebuilt game's receipt. The compatibility
build keeps their original capability contract during that update, while retaining
FSR3 upscaling and the other improvements. Receipts describe the actual compiled
features, and unavailable integrated generation stays disabled in the launcher.
New graphical builds explicitly include modern support. Terminal builders can use
`--without-dlss --framegen`; this includes support without enabling it in settings.

### Existing-installation game-code reuse during an update

Linux 0.1.26 also handles older launchers requesting their active game-code library
from the preceding update folder. After verifying the exact active selection,
the builder regenerates and compiles from your validated game files. This can add
build time. The native path/content/input/ABI checks remain strict; an unrelated
or tampered selection is not accepted. A failed preparation keeps the previous
working version selected. There is no save-format conversion.


### Native host tools and SDL linker compatibility

Linux 0.1.27 prevents the packaged Python runtime's private libraries from
shadowing system OpenSSL when native `readelf` or `nm` runs. Inspection failures
include bounded diagnostic output. SDL builds also disable an optional
linker-generated dependency-file mode unsupported by the pinned Zig linker.
Normal compiler/header dependency tracking and required desktop/audio backends
remain enabled. Retry through the existing launcher; no input or save deletion
is required.
