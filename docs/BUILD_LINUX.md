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
unresolved. Version 0.1.2 is a regular source/setup release requiring local game
generation. See [platform status](PLATFORM_STATUS.md) and
[remaining work](../TODO.md) for features and validation limits.

If a stage fails, use the log path printed by the builder. A successful local build
and AppImage check do not establish full-game or other-GPU compatibility. The
project will not provide game files or tell you where to obtain them.
