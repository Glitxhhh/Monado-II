# Native Windows development builds

This branch is bringing the accepted Linux dev104 R5 behavior into the native
Windows port. It has produced native game/settings builds and a physically
confirmed title screen on Windows 11, RTX 4070 SUPER, driver 595.97. Gameplay,
save/reload, long sessions, all providers, other GPUs and full DE/Future Connected
coverage still require maintainer testing. See [platform status](PLATFORM_STATUS.md)
and the acceptance matrix in the [handoff](WINDOWS_PORT_HANDOFF.md).

There is no new public Windows release. The existing
`monado-ii-windows-original-implementation-setup.zip` download remains the
historical implementation; Linux version numbers never identify Windows updates.
Local receipts use `windows-parity-development`, without borrowing a Linux release
number. The accepted Linux source and deferred notes remain unchanged.

## Portable graphical setup

The authored native setup frontend and backend support selecting already-extracted
input, a fresh installation directory, worker limits 1–6, real build progress,
cancellation/retry and optional shader preparation. A private development setup
EXE can be built with `Monado_Windows/tools/build_windows_builder.py` from a clean
reviewed Windows source export, the original pinned Python/wheel archives and a
successful native dependency receipt. It embeds a checked persistent local tool
cache; reopening uses that cache and the installation registry. It opens settings
only when requested and never presses Play.

This development payload is not approved for public redistribution. Dependency,
GPL/corresponding-source and optional vendor/model obligations require a separate
review. No extracted game data, generated game C, game-code DLL, shader cache or
save belongs in a setup payload. Users generate their own game library locally.

Adopt checks a completed native build without importing or resetting its profile.
Locate requires the same original executable and RomFS inventories before changing
only the two recorded input junctions. Unknown/edited destinations and original
receipts remain intact. Updates use a dedicated Windows channel and verified source;
a Linux release is never offered as a Windows update. Failed/cancelled preparation
keeps the current selection. Activation changes the checked runtime selection while
the original profile remains stable. Obsolete owned update transactions are eligible
for cleanup only after success; busy, changed or unsafe trees are deferred.

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
