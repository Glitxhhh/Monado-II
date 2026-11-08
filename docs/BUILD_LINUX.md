# Build and play on Linux

Download and extract the **Linux source/setup kit** from
[Releases](https://github.com/brendonbusker/Monado-II/releases). Open a terminal in
the extracted folder. The supported baseline is **Linux x86-64 with CPython 3.14**,
bash, pkg-config, dpkg-deb, binutils (`readelf` and `nm`) and squashfs-tools
(`mksquashfs` and `unsquashfs`). The standard DLSS build also requires executable
`/usr/bin/bwrap` (bubblewrap) and a readable GNU C++ runtime at
`/usr/lib/x86_64-linux-gnu/libstdc++.so.6`. A compatible Vulkan driver and host
libraries are also required.
The builder reports missing prerequisites; it never runs sudo or changes system
packages/drivers. This is not yet a distribution-independent build.

## 1. Put your own game files beside the build command

Use your own legally dumped **Definitive Edition 2.0.0** files from your own legally
obtained Switch and game. Only already-extracted folders are accepted. No game
files, keys, links or dumping instructions are provided.

```text
Monado-II/
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

Frame generation remains a separate optional experimental build feature. Enabling
DLSS in the normal build does not enable it; reported slow/glitchy behavior remains
unresolved. Version 0.1.3 is a regular source/setup release requiring local game
generation. See [platform status](PLATFORM_STATUS.md) and
[remaining work](../TODO.md) for features and validation limits.

Installation paths containing spaces are supported starting with 0.1.4. An older
0.1.3 source kit can fail in dependency compiler flags in such a folder; use the
latest source kit in the same original installation folder. Keep input/profile
directories in place.

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
builds keep DLSS/DLAA enabled. Automated full rebuilds currently refuse custom
frame-generation or footstep-diagnostic builds rather than silently dropping
those options; those builds require a manual rebuild. Frame generation remains
experimental. Edited release-owned source files also stop automatic installation
so local changes can be merged manually. Normal settings changes do not modify
the release-owned source.

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

To select the previous version, close the launcher, game and shader precompiler
using the original default profile. From the **original installation root**, run:

```sh
Monado_Linux/.venv/bin/python -I -B tools/update_linux.py --rollback
./Play_Monado_II.sh
```

Rollback changes the active-version pointer, including returning to the original
bootstrap after the first managed update. It does not restore or delete game
builds, input, saves or settings. This command uses the original default profile;
it is not a profile migration or save-recovery tool. The updater is Linux-only;
no Windows updater or backport is included.

If a stage fails, use the log path printed by the builder. A successful local build
and AppImage check do not establish full-game or other-GPU compatibility. The
project will not provide game files or tell you where to obtain them.
