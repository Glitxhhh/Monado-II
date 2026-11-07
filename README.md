# Monado II

**Monado II** is a community fork of **Monado**, the Xenoblade Chronicles:
Definitive Edition static recompilation project by **TheMonadoGuy / Monado
contributors**, built on **Peiikai's LASRecomp**. The name acknowledges both the
original project and the game. This fork adds native Linux work and continues
maintenance; it is independent of Nintendo and Monolith Soft.

## You must supply your own legally dumped game

**No game files are included in this repository or its releases.** To play, you
must own a legally obtained copy of Xenoblade Chronicles: Definitive Edition and
supply your own legally dumped game files from your own legally obtained Nintendo
Switch console, including the supported version 2.0.0 update.

**I will not provide game files, keys, firmware, download links, or tell anyone
where to get them. I will not provide instructions for obtaining or dumping them.**
Requests for those files, links or instructions will not be answered. Do not post
game files, keys, saves, generated game code, shaders or caches in issues or pull
requests. See [the contribution rules](CONTRIBUTING.md).

Only already-extracted `exefs/` and `romfs/` folders are accepted. Package/key
extraction is not supported. Game-derived C, executables, shaders and caches are
generated locally on each player's machine and must stay out of Git and releases.

## Current status

| Platform | Source status | Validation |
|---|---|---|
| Linux x86-64 | Native SDL/Vulkan port, high-refresh timing fixes, menu-speed preference, native DLSS/DLAA and optional shader preparation | Opening cutscenes/battles through first character control tested by the maintainer; full game, Future Connected and save/recovery coverage remain incomplete |
| Windows x86-64 | Original Monado implementation retained | Original author reported full-game playability on NVIDIA RTX; this fork has not independently built or validated Windows, and Linux improvements are queued in [TODO](TODO.md) |

**Linux frame generation is experimental. It may run slowly and show visual
glitches; leaving it off is recommended.** FSR 3.1 Super Resolution remains
unavailable on Linux. Windows retains its own vendor integrations. Other GPUs,
drivers and long sessions need more validation; component tests are not full-game
qualification. See [platform status](docs/PLATFORM_STATUS.md).

The strict public source snapshot uses plain TTF/OTF/TTC font input. It does not
ship Nintendo fonts or include proprietary-font deobfuscation. The public launcher
uses a neutral Monado II wordmark/theme; the development artwork and local sword
icon are excluded pending redistribution review. These public-only changes need
further gameplay/UI validation.

## Source releases and local builds

On Linux, put your own extracted input in `game_files/exefs` and `game_files/romfs`
beside the downloaded kit, then run **`./Build_Monado_II.sh`**. It builds locally and
generates **`Monado-II-x86_64.AppImage`** for the launcher. Open it with
**`./Play_Monado_II.sh`**, which also works without FUSE. Keep the workspace beside
the image; game code, libraries and state remain outside its launcher payload.
See the [Linux instructions](docs/BUILD_LINUX.md) for prerequisites and an input
symlink option that leaves existing game files in place.

Initial releases are separate **Linux source/setup `.tar.gz`** and **Windows
source/setup `.zip`** kits. They are **not precompiled playable game downloads**.
You build the game locally from your own verified dump. No compiled game binary,
private vendor SDK binary, game-derived cache or user data is distributed.

- [Linux build instructions](docs/BUILD_LINUX.md)
- [Windows build instructions](docs/BUILD_WINDOWS.md)
- [Release contents and verification](docs/RELEASING.md)
- [Changelog and differences from upstream](CHANGELOG.md)

The source tree keeps `Monado_Linux/` and `Monado_Windows/` separate. Use one
read-only game-input location, with independent per-port generated files, builds,
configuration, caches, logs and saves. Never direct runtime output into game input.

This project uses extensive AI-assisted development. The original Monado author
also documented substantial AI assistance. Human review, tests and reproducible
results matter; existing known limitations are tracked rather than hidden.

## Credits and license

Full attribution is in [CREDITS](CREDITS.md), with preserved copyright notices in
[LICENSE](LICENSE) and third-party terms in [NOTICE](NOTICE.md). Monado II's authored
code is MIT-licensed; dependencies retain their respective licenses. The source
license grants no rights to Nintendo/Monolith Soft game content, trademarks, fonts,
artwork or other third-party assets.
