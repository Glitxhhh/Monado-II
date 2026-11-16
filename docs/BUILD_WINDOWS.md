# Native Windows experimental setup and source builds

This branch is bringing the accepted Linux dev104 R5 behavior into the native
Windows port. It has produced native game/settings builds and a physically
confirmed title screen on Windows 11, RTX 4070 SUPER, driver 595.97. Gameplay,
save/reload, long sessions, all providers, other GPUs and full DE/Future Connected
coverage still require maintainer testing. See [platform status](PLATFORM_STATUS.md)
and the acceptance matrix in the [handoff](WINDOWS_PORT_HANDOFF.md).

The updated Windows package is **Windows Experimental 0.1.0**. Reported DLSS/DLAA
purple flashes and flickering remain unresolved. Full gameplay/provider and
performance qualification is incomplete. The historical
`monado-ii-windows-original-implementation-setup.zip` describes the old port;
use the separate `MonadoII-Windows-Setup.exe` for this experimental port. Linux
release numbers never identify Windows updates. Internal local receipts retain
the compatible `windows-parity-development` schema. The accepted Linux source
and deferred notes remain unchanged.

## Portable graphical setup

The portable EXE opens with the original launcher's artwork, fonts and theme.
First use asks for already-extracted game input and an installation directory,
with worker limits 1–6, real build progress, cancellation/retry and optional
shader preparation. A successful build opens the checked settings launcher.
Reopening the same EXE uses its verified cache and installation registry to reach
that launcher again. Play remains a manual action.

Public experimental mode in `Monado_Windows/tools/build_windows_builder.py`
requires a versioned reviewed Windows source export and complete, byte-verified
native/Python corresponding-source and frontend relink companions. Required
notices are embedded in the tool cache. Private mode still produces a `-Dev.exe`
without publication approval. See [release materials](../Monado_Windows/docs/WINDOWS_RELEASE.md)
for the assets to attach with the EXE. No extracted game data, generated game C,
game-code DLL, shader cache or save belongs in a setup payload. Users generate
their own game library locally; optional vendor/model components remain local.

Adopt checks a completed native build without importing or resetting its profile.
Locate requires the same original executable and RomFS inventories before changing
only the two recorded input junctions. Unknown/edited destinations and original
receipts remain intact. Updates use a dedicated Windows channel and verified source;
a Linux release is never offered as a Windows update. Failed/cancelled preparation
keeps the current selection. Activation changes the checked runtime selection while
the original profile remains stable. Obsolete owned update transactions are eligible
for cleanup only after success; busy, changed or unsafe trees are deferred.

## Preparing a Windows release from Linux

The current Windows source export and setup EXE packager require native Windows:
the exporter uses Win32 file guards, and the packager uses Windows compression
APIs, pinned Windows dependencies and a successful native build receipt. They
do not currently support a Linux-only build of the setup EXE.

A Linux session can maintain the source, prepare release notes and metadata, and
upload an already reviewed Windows artifact. A fresh setup EXE still needs a
Windows machine or Windows runner. The existing `source-release.yml` workflow
only packages source archives, including the historical Windows ZIP; it does
not build this portable EXE. The Windows source-check CI job is also not an EXE
release job. Do not use the historical Windows ZIP declaration to publish this
updated Windows port. The dedicated Windows source packager produces the new
updater archive/metadata; a fully automatic Windows EXE CI job remains future work.

The locally tested `MonadoII-Windows-Setup-Dev.exe`, dependency archives,
native receipts and private test evidence are not stored in Git. Preserve those
Windows-local files if reusing that development package. The latest experimental
DLSS labels require rebuilding older development packages. Public setup mode
requires the exact dependency/license, corresponding-source and relink evidence
described above, with independent Windows update metadata. Do not publish
game input, generated game code, caches or saves. These Windows requirements do
not change the accepted Linux build or release flow.

## Source and BAT fallback

Use Windows 10/11 x86-64 and a native CPython 3.14 interpreter. Run normal builds
unelevated. Your own extracted DE 2.0.0 input must contain `exefs` and `romfs`;
root `game_files` is the default. Input stays read-only. No keys, firmware,
downloaded game data or package extraction are supported.

After auditing the project setup script, explicitly prepare its hash-pinned local
Python packages and native dependencies with PowerShell:

```powershell
& .\Monado_Windows\tools\setup_windows.ps1 -PythonPath 'C:\path\to\Python314\python.exe' -DownloadDependencies
```

The script uses a project-local environment and pinned dependency lock. It does
not install or upgrade system packages, alter system PATH or accept vendor terms.
An existing environment with the wrong interpreter is preserved and refused.

Build from the repository root:

```powershell
.\Build_Monado_II.bat -InputFolder 'C:\My extracted game' -Jobs 4 --split-game
```

The explicit Python equivalent is:

```powershell
& .\Monado_Windows\.venv\Scripts\python.exe -B .\Monado_Windows\scripts\build_windows.py --stage all --split-game --input 'C:\My extracted game' --jobs 4
```

If present, the BAT wrapper prefers the checked project-local embedded interpreter
at `Monado_Windows/.deps/python314/python.exe`. Each fresh build has a separate
output/profile under `Monado_Windows/build`; its receipt records exact source,
input, compiler, flags, dependency and binary identities. Compiler/link failures
are fatal. Cancellation terminates only the build's owned job, retains diagnostics
and does not replace the active game/settings pair.

Open the checked settings launcher manually with `Play_Monado_II.bat`, then choose
Play yourself. Build never starts the game. For runtime/UI-only rebuilds,
`--reuse-game-library 'C:\absolute\checked\build-root' --split-game` can reuse a
compatible locally generated DLL after complete identity/ABI/PE/receipt checks.
Input, generator, guest-header, compiler or ABI changes require regeneration;
corrupt receipts/libraries are failures. Omitting `--split-game` retains the
full static-game source fallback.

## Runtime and qualification boundaries

New profiles keep Quick Step, cheats, rumble, on-screen FPS and frame generation
OFF. Original quality/aspect choices remain default; first DLSS selection is
Native 100%. Unknown INI keys and saved choices survive settings updates. Native
Win32 fibers, FP/TLS, memory, Unicode processes and provider loading remain native.

Windows presents once per real frame by default while retaining FIFO V-Sync and
the game limiter. A focused mixed-display defect in the inherited repeat policy
was reproduced at about 10 real FPS on a nominal 179.88-Hz monitor; the corrected
policy reached about 30 real FPS at the same size/display. Accepted presentation
calls are not a physical scanout measurement. No effects or simulation steps were
removed. Explicit `XCDE_REPEAT_PRESENT=1` retains legacy diagnostic repetition.

Optional SDK/model components require their own valid local preparation and real
provider capability. Missing components remain unavailable. Authored component
fixtures, native compilation and title-screen startup are separate evidence;
none proves full gameplay or provider performance. Preserve every existing
profile/save and use disposable authored fixtures for recovery tests.
