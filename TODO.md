# Monado II TODO

Unchecked items are potential or deferred work, not implemented or validated
features. Windows parity remains deferred; 0.1.12 does not add Windows backports.

## Deferred Windows parity references

This list records known Linux/Windows differences for future Windows development;
none of these backports is implemented or qualified by this release.

- [ ] Bring over the themed/responsive launcher, public-safe resources, Monado II
  wordmark/icon integration and larger default opening size.
- [ ] Port engine-time GUI animation and held-direction repeat corrections,
  including title/menu/battle-arts navigation.
- [ ] Port the Normal (30 FPS style) / Fast (60 FPS style) menu-speed setting,
  parser/save support and checked game query hooks.
- [ ] Port animation phase rebase and complementary walk/run threshold correction
  that resolved reported high-FPS footstep chatter on Linux.
- [ ] Port exact30FPS cutscene limiter behavior at fractional/high output rates.
- [ ] Transfer save flush/Commit error propagation and journal-retention repairs,
  graceful guest exit/exit locks, IPC bounds/lifecycle and scheduler atomics.
- [ ] Review DLAA native-resolution, temporal-history/resource retirement,
  GPU host-memory import and shader/pipeline-cache integrity corrections.
- [ ] Adapt honest precompile complete/partial/cancel/fatal states, safe cache
  commits and explicit retry to Windows process/filesystem APIs.
- [ ] Preserve custom/unknown launcher settings, atomically save configuration,
  and review post-game reload/process behavior.
- [ ] Design and validate a Windows updater; the 0.1.3 updater is Linux-only.
- [ ] Revisit Linux-only Exit to desktop, expanded save slots, bounded initial
  save scanning, the dedicated Quick Step slot and native trading/runtime fixes
  if Windows parity is explicitly scheduled later.
- [ ] Regenerate/build on Windows and run focused regression tests in isolated
  state, then manual gameplay. Keep original Windows fiber/vendor implementations.

Windows already has DLSS/DLAA, AMD frame generation and shader precompilation.
Do not replace those with Linux-specific providers or transplant experimental
Linux FG presentation. Publishing a Windows source kit does not establish parity.

## Linux correctness and coverage

- [x] Stabilize the dedicated Quick Step slot: restore saved bonuses through
  Load Game/Continue, native row/portrait E markers, normal chooser restoration
  and native menu feedback. Keep it off by default for the original-game preference.
- [ ] Expand Quick Step character, equipment, save/recovery and Future Connected
  coverage beyond the accepted manual scenarios.
- [x] Preserve recorded shader programs and compiler constants for queued draws;
  the Bionis Interior crash correction received positive manual feedback.
- [x] Restore held-L2 camera input and native control-help prompts with optional
  XC3-style party switching; provide Linux party discovery and keep optional
  D-pad walking from competing with held-ZL controls. The maintainer accepted
  the revised controls manually.
- [ ] Expand field/battle, Future Connected and controller coverage for those
  camera and party-switching controls.
- [x] Default newly selected Linux DLSS to DLAA / Native (100%) while preserving
  explicit saved quality choices.
- [ ] Diagnose and fix reported DLSS Quality (67%) shaking. Native (100%) is the
  current workaround; the 0.1.12 selection default is not a rendering fix.
- [ ] Investigate reported shadow boxes in Valak/Makna. They are not fixed in
  0.1.12; exact causes and affected scenes remain to be established.
- [ ] Confirm actual CachyOS/AMD/Wayland gameplay and additional distribution
  builds. Isolated Arch userspace loading/media passes do not cover native GPU,
  audio/session or optional plugin behavior.
- [ ] Reconcile current-version Bazzite diagnostics and qualify host/container,
  Python and GUI/CLI environment handling. The historical missing-codec report
  used the older dependency set; no new Bazzite fix is included in 0.1.12.
- [ ] Improve experimental frame-generation pacing/performance and visual quality
  after the current publication work; keep its warning and off-by-default setting.
- [ ] Validate more gameplay, demanding scenes, long sessions and Future Connected.
- [ ] Validate disposable save/reload/journal recovery and failure durability.
- [ ] Expand extended-save coverage across main-game and Future Connected,
  including 99 populated manual slots, thumbnails, Continue and recovery.
- [ ] Expand high-FPS follower/damping/cutscene coverage and FP exception semantics.
- [ ] Test AMD/Intel/other hardware and remaining diagnostics/input behavior.
- [ ] Validate the public plain-font, restored-artwork and DLSS paths across more
  gameplay scenarios, drivers and hardware.

## Releases and onboarding

- [x] Add a one-command Linux local build and generated launcher AppImage that
  keeps game data outside its payload and preserves checked workspace state.
- [x] Restore the exact generated launcher illustration with provenance and
  default DLSS/DLAA support, with an explicit `--without-dlss` build option.
- [x] Add Linux stable-release offers, verified source downloads, local update
  builds, progress/cancellation and stable profiles.
- [x] Retain only the current launcher and its required game payload after checked
  activation; clean accumulated updater-owned history without changing saves,
  hidden Quick Step metadata, settings, mods or useful caches. Remove local
  rollback history; manual downgrades remain available through GitHub releases.
- [x] Rehearse the installed 0.1.11 upgrade and first-start cleanup using an
  independent copy, checking protected profile contents and legacy bootstrap
  selection. This did not change the original installation or test gameplay.
- [ ] Expand real-system update/restart/interruption and manual-downgrade
  validation across supported environments.
- [ ] Improve first-start cleanup responsiveness and measure longer update
  histories; the isolated long-history rehearsal took about 75 seconds for
  maintenance and post-checks, with duration depending on files and storage.
- [x] Include FG capability in normal Linux builds and support ordinary FG full
  rebuilds with the 0.1.6+ updater, retaining the off-by-default runtime setting.
- [ ] Support automated full rebuilds of footstep-diagnostic builds without
  dropping their diagnostic features. Pre-0.1.6 custom FG clients need one manual upgrade.
- [ ] Validate the workspace launcher AppImage on more distributions/desktops;
  it currently requires the complete local build folder and its absolute receipts.

- [ ] Run real clean-machine Linux and Windows local-build checks.
- [ ] Test public source/setup kits on both platforms; Windows remains unvalidated
  by this fork until those checks are recorded.
- [ ] Review future authored-launcher/installer binaries and dependency notices
  separately. Never publish a game-derived executable or cache.
- [ ] Review further proposed artwork/icons before adding them to source/releases;
  only the exact illustration recorded in the provenance is currently admitted.
- [x] Provide a safe, user-friendly local build flow without bundling game content.
