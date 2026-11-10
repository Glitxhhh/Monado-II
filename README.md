# Monado II

**Monado II** is a community fork of **Monado**, the Xenoblade Chronicles:
Definitive Edition static recompilation project by **TheMonadoGuy / Monado
contributors**, built on **Peiikai's LASRecomp**. The name acknowledges both the
original project and the game. This fork adds native Linux work and continues
maintenance (no plans to add my improvements like 99 save slots, high fps stability, etc to Windows at this point); it is independent of Nintendo and Monolith Soft. This was developed entirely using AI,
do with that information what you will.

<img width="1259" height="795" alt="image" src="https://github.com/user-attachments/assets/69fabc08-665d-41bc-8900-f707138d630f" />

Dedicated Quickstep Gem Slot! (Optional)
<img width="960" height="504" alt="quickstep" src="https://github.com/user-attachments/assets/c90305c4-7b92-4f11-a0b0-0086d4f6659f" />

99 Save Slots!
<img width="960" height="504" alt="99-save-slots" src="https://github.com/user-attachments/assets/76e5b163-2a8d-41a4-82a2-060e61174bab" />

Linux 0.1.11 stabilizes the optional dedicated Quick Step gem slot, including
save/Continue restoration, equipped indicators and native menu feedback, and
corrects the reported Bionis Interior crash. **Quick Step remains off by default**
for players who prefer the original equipment setup. See the
[changelog](CHANGELOG.md), [Quick Step guide](docs/QUICKSTEP.md) and
[Linux requirements](docs/BUILD_LINUX.md).

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
| Linux x86-64 | Native SDL/Vulkan port, high-refresh timing fixes, menu-speed preference, native DLSS/DLAA, optional shader preparation, expanded saves and an optional stable Quick Step slot | Opening cutscenes/battles through first character control tested by the maintainer; expanded saves, scan responsiveness, Quick Step, trading and the Bionis Interior crash fix received positive manual feedback; full game, Future Connected and save/recovery coverage remain incomplete |
| Windows x86-64 | Original Monado implementation retained | Original author reported full-game playability on NVIDIA RTX; this fork has not independently built or validated Windows, and Linux backports are not currently planned |

**Linux frame generation is experimental. Performance varies: it may run worse,
stutter or show visual glitches; leaving it off is recommended.** FSR 3.1 Super Resolution remains
unavailable on Linux. Windows retains its own vendor integrations. Other GPUs,
drivers and long sessions need more validation; component tests are not full-game
qualification. See [platform status](docs/PLATFORM_STATUS.md).

Linux includes **Exit to desktop** on the title screen and in the in-game System
menu, with native confirmation and Cancel/No selected by default. Existing menu
actions remain available. Save your progress first: exiting does not save it.

Linux provides **99 manual save slots plus a separate autosave for each
story**, keeping main-game and Future Connected saves separate. Four native cards
remain visible as you scroll; unused slots show **No data** and create no files.
Existing saves keep their native format and filenames. Initial Save/Load scanning
groups ready native read steps within a small budget, yielding for pending I/O
and callbacks. Opening these menus and scanning Continue can still take time;
99 populated slots and every save/recovery case have not been qualified.

The strict public source snapshot uses plain TTF/OTF/TTC font input. It does not
ship Nintendo fonts or include proprietary-font deobfuscation. The public launcher
uses the Monado II wordmark/theme and the original generated landscape artwork
on the left. Its exact asset and origin are documented in the
[artwork provenance](Monado_Linux/tools/launcher/assets/ARTWORK_PROVENANCE.md).
The separate local sword icon remains excluded. Broader gameplay/UI validation
remains incomplete.

## Setup downloads and local builds

Download the [Linux setup/build kit](https://github.com/brendonbusker/Monado-II/releases/download/v0.1.11/monado-ii-0.1.11-linux-setup.tar.gz)
or [Windows setup/build kit](https://github.com/brendonbusker/Monado-II/releases/download/v0.1.11/monado-ii-0.1.11-windows-setup.zip)
from the release, and verify its checksum. These contain source and setup tools;
you provide your own input and build locally. For Linux app launchers such as
Quiver, select **`App_Monado_II.sh`**: it shows first-build progress in an installed
terminal, then opens the launcher. See [Quiver setup and limitations](docs/QUIVER.md).

On Linux, put your own extracted input in `game_files/exefs` and `game_files/romfs`
beside the downloaded kit, then run **`./Build_Monado_II.sh`**. It builds locally and
generates **`Monado-II-x86_64.AppImage`** for the launcher. Open it with
**`./Play_Monado_II.sh`**, which also works without FUSE. Keep the workspace beside
the image; game code, libraries and state remain outside its launcher payload.
See the [Linux instructions](docs/BUILD_LINUX.md) for prerequisites and an input
symlink option that leaves existing game files in place.

The normal Linux build includes native **DLSS Super Resolution / DLAA** support
for compatible NVIDIA hardware. It fetches the pinned official NVIDIA SDK locally;
SDK binaries/models are not included in source kits or the launcher image.
Use `./Build_Monado_II.sh --without-dlss` to explicitly omit that provider.
Starting with 0.1.6, normal Linux builds also include **frame-generation support**.
It remains **experimental and disabled in settings by default**: select DLSS/DLAA
and enable the checkbox if you want to try it. No extra build flag is needed.
Explicit `--without-dlss` excludes both providers because FG currently depends on
DLSS/DLAA. See the [build guide](docs/BUILD_LINUX.md#experimental-frame-generation).

Starting with **0.1.3**, the Linux launcher quietly checks for newer stable
releases on startup. A newer release offers **Update** or **Later**. Update
downloads the source kit, builds it locally with progress and cancellation, then
restarts the launcher; it never presses Play. Compatible launcher-only updates
retain the verified game build. Other updates rebuild the game locally. Saves,
settings and the shared input stay in the original installation; failed or
cancelled preparation keeps the previous version selected.

**Updating to 0.1.11 requires a full local game rebuild** for the Quick Step and
queued-rendering fixes. The updater performs that rebuild and keeps the original
save/settings profile in place; it does not convert, replace or delete your saves.
Keep optional Quick Step metadata with the complete save folder, including hidden
files; native `.sav` formats and filenames remain unchanged.

From 0.1.6, the original folder's `update-version.json` shows the active version.
Use `./Rollback_Monado_II.sh` for a version-aware rollback; see the Linux guide.

**0.1.2 has no updater:** first apply the verified 0.1.3 or newer Linux source kit to the
same original installation folder and rebuild once, preserving its input and
profile directories. Follow the safe manual-upgrade steps below. A fresh kit in
a new folder is a separate installation and does not import an older profile.
See [Linux updating and rollback](docs/BUILD_LINUX.md#4-update-and-roll-back).
Windows has no updater in this release, and backport work is not currently planned.

Releases provide separate **Linux setup `.tar.gz`** and **Windows setup `.zip`**
kits containing reviewed source. They are **not precompiled playable game downloads**.
You build the game locally from your own verified dump. No compiled game binary,
private vendor SDK binary, game-derived cache or user data is distributed.
Version 0.1.11 uses the regular stable release channel. Quick Step is stable and
optional; frame generation remains experimental and off by default. A legacy
Linux `-source` download remains solely for compatibility with installed updaters. New downloads use the `-setup` kit.

- [Linux build instructions](docs/BUILD_LINUX.md)
- [Windows build instructions](docs/BUILD_WINDOWS.md)
- [Release contents and verification](docs/RELEASING.md)
- [Quiver entrypoint and re-review details](docs/QUIVER.md)
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
