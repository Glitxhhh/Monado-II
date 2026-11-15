# Native Windows run diagnostics

`runtime/host/run_diagnostics_windows.c` adapts the accepted Linux LINUX-099
runtime diagnostics and `scripts/run_settings_log.py` retention/privacy policy.
It is passive: it does not change settings, timing, provider eligibility or saves.

After game-code and selected-input validation, a real game start generates its
own random run ID. Known requested INI settings, parsed settings, effective
renderer policy and the observed applied limiter are reported under that ID.
Unknown keys, bindings, mod paths, GPU UUIDs, arbitrary environment values and
file paths are excluded. Invalid requested values are omitted. Timing/INI/hidden
overrides are reported as booleans rather than their private values. NVIDIA
Smooth Motion preference is explicitly unavailable as a native activation claim;
the Linux driver environment variable is not consulted.

The render thread samples Game and SDK Output rates about every two seconds when
generation is selected, including when the overlay is hidden. It retains warmup,
unavailable and input-readiness states. SDK accepted totals have no generated-only
identity, so Generated remains unavailable. Neither rate means physical scanout.
Game events count successfully acquired native frames; failed/headless acquisitions
and repeated output images contribute no Game events.
Configured/effective target FPS and the nominal applied limiter are identified
separately from measured rates.

Optional local JSONL records live in the stable profile's `logs/game-runs/`.
Native Unicode operations hold ordinary ancestor directories through creation,
retention and recording, require current-user ownership and reject reparse points
and hardlinked files. An exclusive owned lock serializes record access. Closed
records carry a SHA256 footer over all earlier bytes. Retention removes only an
unchanged, owned, valid record through its already-checked native handle, keeping
at most 16 recognized records. Unknown, modified, sharing-locked, foreign or
unfinalized state is preserved and disables optional recording rather than
interrupting the game. The digest detects modification; it is not a publisher
signature or protection against a malicious process running as the same user.

Each record is bounded to 32 KiB, with at most 32 changed rows per settings,
renderer, pacing and applied-limiter category. After the record fills, sampled
diagnostics continue on stderr. Shader preparation emits its runtime-start kind
only on stderr and creates no run record. Invalid/dry startup checks return
before recording begins. `atexit` seals a normal exit; interrupted records remain
unfinalized and are preserved.

`tests/run_windows_diagnostics.py` compiles the real native helper and settings
parser against authored fixture state. It checks private-value exclusion,
effective fields, hidden-overlay sampling cadence, bounds, 16-record retention,
unknown/modified/locked/hardlinked/reparse preservation and no shader-preparation
records. Test-only probes verify that Windows blocks parent and record-directory
renames during retention and active recording. These are component fixtures and
do not establish gameplay or performance coverage.
