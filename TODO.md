# Monado II TODO

Unchecked items are planned work, not implemented or validated features.

## Windows backports

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
- [ ] Regenerate/build on Windows and run focused regression tests in isolated
  state, then manual gameplay. Keep original Windows fiber/vendor implementations.

Windows already has DLSS/DLAA, AMD frame generation and shader precompilation.
Do not replace those with Linux-specific providers or transplant experimental
Linux FG presentation. No Windows backport is implied by publication preparation.

## Linux correctness and coverage

- [ ] Improve experimental frame-generation pacing/performance and visual quality
  after the current publication work; keep its warning and off-by-default setting.
- [ ] Validate more gameplay, demanding scenes, long sessions and Future Connected.
- [ ] Validate disposable save/reload/journal recovery and failure durability.
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
  builds, progress/cancellation, stable profiles and explicit pointer rollback.
- [ ] Expand real-system update/restart/interruption and rollback validation.
- [ ] Support automated full rebuilds of custom frame-generation or
  footstep-diagnostic builds without dropping their selected features.
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
