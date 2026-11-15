# Start the Monado II Windows parity implementation

This is the maintainer-approved task for Codex on the maintainer's native Windows
installation. Read this file and carry out the work below. You do not need the
previous conversation or another feature-selection discussion.

Read root [AGENTS.md](AGENTS.md), then the COMPLETE
[Windows handoff](docs/WINDOWS_PORT_HANDOFF.md), the exact
[reference inventory](docs/WINDOWS_PARITY_INVENTORY.json) and
[deferred notes](docs/windows-parity/DEFERRED_NOTES.md). Read applicable contribution,
license and source contracts. Inspect the checkout and actual host before work.

## Do the Windows implementation

Build the first genuinely updated native Windows release of Monado II with
**EVERY applicable improvement in the accepted Linux dev104 R5 snapshot** already
present in this branch. This is the current large Linux dev build, not just the
older released Linux 0.1.16/0.1.20 feature set or the original Windows version.
Windows source is still original at this handoff; it needs real implementation.

Use Monado_Windows as the working target and Monado_Linux plus its authored tests
as the reference. Retain native Windows platform/fiber/TLS/memory behavior and
supported Streamline/FidelityFX providers. Transfer portable behavior in coherent
buildable bundles with native Windows adapters. Do not convert the entire Linux
directory, copy Linux binaries/syscalls, or require an upfront architecture rewrite.

The complete checklist includes gameplay/high-FPS and fractional timing fixes,
trading/Bionis correctness, 99 saves and safe save transactions, stable optional
Quickstep, all cheats, Exit, L2/party/controller behavior, current launcher/options,
aspects, FPS overlay and honest generated/output measurements, experimental 2x
generation/refresh matching, native temporal FSR3/optional FSR4 Lite, shared temporal
and renderer fixes, graphics controls, rumble, GPU selection, support reports,
shader preparation, local GameLib/runtime reuse and source fallback, graphical
building/input recovery, platform-aware updates and current-only cleanup. The
handoff supplies all source maps, defaults, native pitfalls and acceptance checks.
Do not declare parity after implementing only the UI or a subset of those bundles.

Prioritize a **portable graphical Windows executable**: select the user's own
extracted DE 2.0.0 game folder, choose destination/settings, show real progress,
optionally prepare shaders, and reopen the same executable into the settings
launcher/Play flow. No installer is required. Keep root Build_Monado_II.bat and
Play_Monado_II.bat as a source/fallback route. Do not auto-start the game after builds.

The maintainer accepts the current Linux dev AS-IS. Preserve it. Small quality-panel
layout changes, the reported 5–10 FPS performance recovery investigation and other
rendering/polish notes stay deferred until Windows parity is completed. Do not turn
them into prerequisites or promise those known issues already fixed.

## Work autonomously and validate honestly

You are authorized to plan each coherent bundle, edit source, perform local builds,
run authored/native component tests and debug routine failures on this Windows
installation. Continue through the complete implementation instead of stopping at
another high-level plan. Ask only for genuinely missing user-owned input/access or
a real external/system decision. Respect actual tool/OS approvals; audit inherited
setup scripts before execution. No silent global installs, broad upgrades, vendor
agreement acceptance, unrelated process kills or driver changes.

Reconcile local changes and running jobs. Use codex/windows-parity or a clearly
named codex/ child implementation branch without discarding existing work. Keep one
private project-root PROJECT_MEMORY.md and private detailed evidence under
Monado_Windows/runs. Make small traceable source commits on the development branch;
do not publish a release or change main/tags without the maintainer's approval.

Keep selected game input immutable. Preserve all human saves, settings, cache
ownership and accepted builds. Never reseed a used profile or publish Nintendo
input/generated code, lasgame.dll, translated shaders, saves, keys or private logs.
Use independent authored fixture state. If the approved private development-save
seed is unavailable on Windows, preserve that fact and progress with components;
do not download a replacement or modify existing saves.

Test the real production startup path: the matching local GameLib must load and
validate before workers/module loader/kernel/dispatch. Linux R4 missed this call
and crashed despite direct library unit tests. Cover missing/mismatched/bad-library
failures, static fallback and precompile bypass without running Nintendo functions.

Record actual Windows compiler/dependency/source/build/input identities, commands,
test outcomes/skips and missing coverage. Linux passes and source CI are references,
not Windows native-build/gameplay proof. Do not weaken strict checks or remove
effects to claim success. Preserve all license/attribution/source obligations.

## Finish with a native dev build for manual testing

Once all applicable parity is implemented and native build/component checks pass,
provide exact executable/build/profile paths and a concise manual acceptance list.
Keep gameplay and visible-launcher tests manual unless specifically requested;
do not press Play, automate controls or close a human game session.

Clearly label implemented work with its ACTUAL native-build/component/gameplay
status. Windows remains original/unimplemented until implementation changes it;
do not borrow a Linux release number as evidence of Windows updates. The final
release needs the maintainer's Windows manual test and approval. Full DE/Future
Connected, long sessions, all hardware and unresolved rendering scenarios must
remain explicit where not tested.

**Start now:** reconcile the Windows host/checkout/input/state, audit and harden
the inherited native build, then implement and validate the handoff milestones.
