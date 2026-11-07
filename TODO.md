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
- [ ] Validate the public-only plain-font and artwork-free launcher changes in game.

## Releases and onboarding

- [ ] Run real clean-machine Linux and Windows local-build checks.
- [ ] Test public source/setup kits on both platforms; Windows remains unvalidated
  by this fork until those checks are recorded.
- [ ] Review future authored-launcher/installer binaries and dependency notices
  separately. Never publish a game-derived executable or cache.
- [ ] Review any proposed artwork/icon rights before adding it to source/releases.
- [ ] Provide a safe, user-friendly local build flow without bundling game content.
