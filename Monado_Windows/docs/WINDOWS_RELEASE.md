# Windows Experimental release materials

The main user download is **MonadoII-Windows-Setup.exe**. It is a portable
graphical launcher/builder, with the original launcher artwork, fonts and theme.
On first use it asks for the user's own extracted DE 2.0.0 folder and an install
location, then builds the game locally. Reopening it selects the checked existing
settings launcher. It never starts gameplay without the user's Play action.

**Windows 0.1.0 is experimental.** DLSS/DLAA still has reported purple flashes
and flickering. Performance, all areas/stories, all optional providers and other
hardware remain incompletely qualified. Packaging/component tests do not remove
these limits. The accepted Linux implementation is unchanged.

## Files to attach alongside the Linux release

- `MonadoII-Windows-Setup.exe`: the standalone Windows setup, with verified
  Python/compiler/native tools and the reviewed Windows source embedded.
- `monado-ii-windows-0.1.0-setup.zip`: the source-only Windows updater archive.
- `monado-ii-windows-update.json`: independent Windows update identity/hashes.
- Native and Python/tool corresponding-source companion archives, including
  package recipes, patches and component licenses. The native companion also
  retains all frontend objects/resources/static libraries and a relative relink
  recipe so libiconv can be replaced and the setup relinked.
- `SHA256SUMS` and the release notes/third-party notice index.

Attach the complete prepared release directory's public asset set. The setup
EXE is the file users run; the source ZIP is not a prebuilt playable game. No
game input, generated game code/library, game shaders/caches, saves, keys,
proprietary graphics SDK DLLs or prepared model files belong in these assets.

The Windows version/channel is independent of the Linux release number. Both
sets of assets may be attached to the same ordinary GitHub release. The current
Windows updater checks the ordinary latest release, not GitHub prereleases.
Marking Windows experimental in its identity and release notes does not require
marking the combined Linux release as a GitHub prerelease.

## Reproduce the setup package

Build on native Windows with the hash-pinned tool archives and dependency prefix.
Create a clean publication export and the versioned Windows source archive;
the EXE must embed that same versioned source. Run
`tools/windows_release_sources.py` and the root
`tools/windows_python_release_sources.py` to collect and verify the exact source,
recipe and license closure. The native collector additionally binds the final
frontend's retained relink inputs. No package recipe or downloaded source code
is executed during collection.

Pass `--release-version 0.1.0`, `--native-sources` and `--python-sources` to
`tools/build_windows_builder.py`, along with its reviewed source export, pinned
Python archives, native prefix and successful dependency-selection receipt.
Public mode verifies the actual redistributed bytes, source companions, static
libraries, frontend relink inputs and required notices before emitting the
stable setup EXE. Missing, changed or incomplete evidence fails closed. Omitting
release mode produces a private `-Dev.exe` and does not approve redistribution.

Native setup compilation, hidden first-run/render/bootstrap checks, authored
state/cancellation/updater tests and the dependency/source audits are separate
qualification records. Manual game/image-quality/performance testing remains
separate. Preserve existing profiles, saves and unknown settings during testing.
