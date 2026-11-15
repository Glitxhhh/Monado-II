# Monado II: first current Windows release

For the one-file task prompt, read [START_WINDOWS_PORT.md](START_WINDOWS_PORT.md).
Read [docs/WINDOWS_PORT_HANDOFF.md](docs/WINDOWS_PORT_HANDOFF.md) in full, then
[the exact source inventory](docs/WINDOWS_PARITY_INVENTORY.json),
[deferred notes](docs/windows-parity/DEFERRED_NOTES.md), root CONTRIBUTING.md,
LICENSE, NOTICE.md, CREDITS.md and the relevant per-port contracts. This preparation
supersedes both older Windows handoffs. Do not implement against Linux 0.1.11,
0.1.16 or just the latest public release.

## Objective and current truth

The user accepts Linux dev104 R5 AS-IS as the target for the first genuinely
updated Windows release. Bring EVERY applicable Linux improvement into native
Windows, including gameplay fixes, high-FPS stability, launcher/build/update UX,
all optional features, providers and renderer correctness. The branch contains
the exact accepted Linux implementation, not an already implemented Windows port.
All Monado_Windows implementation files remain original at this preparation.

On the user's actual Windows installation, implementation, local component tests,
builds and debugging are authorized. Plan coherent bundles, then execute routine
work until the complete target is met; do not stop after another plan or a working
launcher. Do not repeatedly ask permission for ordinary source fixes/tests/builds.
On Linux, this task ends at handoff preparation. Preserve the accepted Linux tree;
do not fix its deferred polish or perform a speculative optimization campaign.

Use the current whole repository and Monado_Windows as the working target. Retain
native Win32 fibers/TLS/memory/process behavior and supported Streamline/FidelityFX
providers. Adapt portable Linux behavior in coherent, buildable bundles. No wholesale
Linux-directory conversion or prerequisite shared-core/renderer rewrite. Linux
Landlock, ELF/glibc/sysroot, namespaces, RADV flags and hidraw paths are not Windows
features; implement applicable native equivalents and label nonapplicability.

## Expected user experience

Prioritize a portable native Windows graphical launcher/builder executable, with
the existing BAT/source workflow as a fallback. No installer is required. Users
select their already-extracted game folder, choose installation location/settings,
watch real build progress and optionally prepare shaders. Reopening the same
portable executable reaches the existing settings launcher and Play flow. Preserve
adoption, relocated-input recovery, settings, saves and safe platform-aware updates.
The current handoff describes requirements; no new Windows wrappers or GUI builder
are being claimed as implemented by this branch preparation.

## Start safely and preserve state

Inspect actual branch/status, host tools, dependencies, input, existing profiles,
artifacts and running process identities before work. Do not restart an interrupted
build blindly or discard local changes. Use codex/ branches and traceable source
commits. Keep one private project-root PROJECT_MEMORY.md with detailed private
evidence under Monado_Windows/runs; update after meaningful results/failures and
before stopping. The public handoff is a reference, not a second live memory.

Use only the user's own extracted DE 2.0.0 input at root game_files/exefs and
game_files/romfs, or a checked Windows-accessible selected folder. Linux absolute
paths are not Windows paths; inspect known mounted locations and ask once only if
the user-owned input is genuinely inaccessible. Never download game/key/firmware
data or add package extraction. Keep input immutable and generated output/state
separate under the Windows port. Preserve every human/unknown profile and native
save format, story ownership and Quickstep sidecar metadata. Seed a NEW test profile
only from the approved private seed if available; NEVER reseed or reset a used
profile or download/publish a replacement save. No recovery tests on originals.

Gameplay and visible launcher testing are manual unless the user expressly asks
for an automated session. Do not automatically start the game, press Play, replay
input or close a human session. Compilation, authored synthetic/native fixtures,
non-game loader checks and hidden authored UI checks are allowed. Windows source
CI and Linux tests do not establish a native Windows build, game or performance pass.

Audit inherited setup/build scripts BEFORE executing them. Correct masked compiler
errors, hardcoded/unquoted paths, stale object globs, process-name-wide taskkill,
broad upgrades and silent agreement acceptance. Normal builds are unelevated;
prefer pinned project-local tools. Respect actual OS/tool approvals. Terminate
only owned children/process groups. Native paths/process/DLL/update handling must
work with spaces, Unicode, locked files and reparse points/junctions.

## Non-negotiable parity and evidence

Keep Quickstep, all cheats, rumble, on-screen FPS and frame generation OFF by
default; Original quality/aspect behavior and Native 100% default on new DLSS
selection. Preserve saved choices and unknown INI keys. Implement full gameplay
hooks/save transactions/providers/lifecycle, not only settings controls. Register
new helpers in actual compile AND link lists. Preserve strict CPU/FP/TLS/timing/
synchronization; no success stubs, skipped effects or game-CPU fallback shortcuts.

Split builds must load and validate the local game-code library BEFORE workers,
module/kernel/dispatch initialization. Linux R4 failed first startup because this
call was missing. Preserve the R5 correction and test actual production entry with
authored libraries, including missing/wrong ABI/bad-library failure and static/
precompile bypass. Unit tests calling the loader directly are insufficient.

Keep source/public packages free of extracted assets, game-derived generated C,
lasgame.dll or other compiled game code, translated game shaders/caches, saves,
keys, private logs and unapproved vendor/model binaries. The portable launcher may
ship only reviewed authored runtime/dependencies; users generate their game library
locally. Preserve MIT/GPL/SDK attribution and corresponding-source obligations.
Use clean reviewed publication exports, never delete human data to satisfy guards.

Record exact source/build/input identities, commands, compiler/flags/dependencies,
hardware/driver, scenario and actual outcomes/skips. Distinguish components, native
build, manual startup, gameplay, save/reload, performance and full DE/FC coverage.
Generated frames are not simulation progress; accepted presentations are not
physical scanout. Do not invent generated FPS by subtraction or multiplication.

The user accepts the reported 5–10 FPS Linux overhaul cost for now. Performance
recovery, quality-panel relocation and small rendering/polish issues are deferred
until Windows parity is completed. Preserve their notes; do not make cleanup a
prerequisite or quietly promise these known issues fixed.

Windows remains labelled original implementation until real implementation/build
work justifies a distinct Windows identity. Do not reuse a Linux version number
as evidence of Windows updates. No release, main/tag update or public parity claim
is authorized by this preparation. On Windows, provide a native dev build and
precise evidence for the maintainer's manual test before release claims.
