# Build and play on Linux

Download and extract **`monado-ii-0.1.11-linux-setup.tar.gz`** from
[Releases](https://github.com/brendonbusker/Monado-II/releases). Open a terminal in
the extracted folder. The current pinned baseline is **Linux x86-64 (x86-64-v2),
CPython 3.14, glibc 2.43 or newer**, and a native GNU C++ runtime exporting
`GLIBCXX_3.4.32` and the required C++ ABI versions. Required tools are bash,
pkg-config, dpkg-deb, binutils (`readelf` and `nm`) and squashfs-tools
(`mksquashfs` and `unsquashfs`). The standard DLSS build also needs executable
`/usr/bin/bwrap` (bubblewrap). GNU C++ is discovered in the host's native library
directories; an Ubuntu multiarch path is not required. Compatible host graphics,
audio and session libraries are also required as described below.

The builder fetches hash-locked media, codec and other ordinary user-space
libraries into `Monado_Linux/.deps`. This includes libbluray 1.4.1 (package
`libbluray3` version `1:1.4.1-1`), supplying the `libbluray.so.3` ABI required by
the pinned FFmpeg build. The host may independently provide `.so.4`; keep your
normal system package. No downgrade, major-version symlink or individual-library
version override is needed or supported.
Host glibc, GNU runtimes, driver clients and native audio/session interfaces are
checked rather than replaced. The builder never runs sudo or changes system
packages or drivers. This remains an explicitly tested baseline rather than a
universal Linux binary compatibility promise.

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
./Build_Monado_II.sh --without-dlss
```

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
support. Its launcher checkbox is off initially; users choose whether to enable
it. Reported slow/glitchy behavior remains unresolved. This source/setup release requires local game
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

## Experimental frame generation

Normal Linux builds include native frame-generation support without an extra flag:

```sh
./Build_Monado_II.sh
```

This requires the normal DLSS/DLAA build. Explicit `--without-dlss` omits both
providers; contradictory `--without-dlss --framegen` is rejected. The old
`--framegen` argument remains accepted for compatibility but is unnecessary.
The pinned FidelityFX source is built locally when missing. A verified local SDK
is reused; an altered SDK stops the build for inspection. No SDK binaries are
included in source releases or launcher images. Runtime frame generation stays
**off by default**. In the launcher, select NVIDIA DLSS/DLAA and then enable the
experimental frame-generation option.

The FPS setting/counter continues to describe real rendered game frames. Generated
images add GPU work and do not advance gameplay, input or audio. **Performance
varies: it may run worse, stutter or show visual glitches. Leaving FG off is
recommended for normal play.** At high native frame rates it may offer little
benefit. Compare the same scene/settings with
FG off/on after shader and SDK warmup. Component passes do not qualify physical
presentation, latency, visual quality or every game scene. Maintainer testing of
0.1.5 preferred FG off despite mostly stable gameplay; no broad improvement is claimed.

Launcher-only updates preserve a compatible FG game. An older DLSS build without
FG support is rebuilt to gain it. Current clients can rebuild ordinary FG builds
automatically; footstep-diagnostic builds still require a manual rebuild.

## 4. Update and roll back

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

**The 0.1.11 Quick Step and queued-rendering fixes require a full local game
rebuild.** The normal updater performs this rebuild before activation. It preserves the original
`Monado_Linux/play/launcher` save/settings profile and shared input in place;
it does not convert, replace or delete saves. Preserve optional Quick Step metadata
with the complete save folder, including hidden files. Keep the original installation
folder and its retained update directories. A fresh kit elsewhere is a separate
profile, as described above.

Subsequent managed updates leave installed sources in place. Each new version
stays at a fixed path under
`Monado_Linux/updates/<id>/workspace`, with separate generated code, dependencies,
builds and logs. Once a build starts, its source/build/image paths must never be
moved or renamed. The original root AppImage remains the entry point and selects
the checked active version. Managed updates keep the original installation's
`game_files` read-only and leave saves/settings in its
`Monado_Linux/play/launcher` profile; they do not copy or replace that profile.
Keep the original folder and retained version directories together. Update
downloads and runtime artifacts under `Monado_Linux/updates` are local only and
must not be committed or included in source releases.

Starting with 0.1.6, the original folder's **`update-version.json` reflects the
active version**. An older installation gains this behavior when its updated
launcher first starts. Its original source version is retained in a checked
backup for rollback; the game input and profile are not moved. Keep the update
directories, including the retained updater used for rollback.

To select the previous version, close the launcher, game and shader precompiler
using the original default profile. From the **original installation root**, run:

```sh
./Rollback_Monado_II.sh
./Play_Monado_II.sh
```

The rollback entrypoint is included in fresh 0.1.6 kits and installed at the
original root during its first version-aware managed startup. It validates and
uses the retained capable updater even when the previous game version is older.
Use this entrypoint after upgrading; the old pre-0.1.6 `tools/update_linux.py
--rollback` command refuses the newer version records safely.

Rollback changes the active-version pointer and visible version, including
returning to the original bootstrap after the first managed update. It does not restore or delete game
builds, input, saves or settings. This command uses the original default profile;
it is not a profile migration or save-recovery tool. The updater is Linux-only;
no Windows updater or backport is included.

If a stage fails, use the log path printed by the builder. A successful local build
and AppImage check do not establish full-game or other-GPU compatibility. The
project will not provide game files or tell you where to obtain them.
