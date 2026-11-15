# Native portable builder backend

`tools/bootstrap_windows.py` is the native setup executable's trusted Python
backend. It runs from the persistent authenticated cache, never from an EXE
extraction temporary directory. It never starts the game or settings launcher.

The frontend invokes the cached `python/python.exe` with `-E -s -B` and
`setup/bootstrap_windows.py` with these arguments:

```
--action build|resume|adopt|locate-input|check-update|update
--cache ABSOLUTE_CACHE
--payload-sha256 OUTER_PAYLOAD_SHA256
--manifest-sha256 PAYLOAD_MANIFEST_SHA256
--progress CACHE/state/FRESH.jsonl
--cancel-file CACHE/state/FRESH_CANCEL
--jobs 1..6
[--input EXTRACTED_GAME_FOLDER] [--destination INSTALLATION_FOLDER] [--precompile]
```

The native frontend authenticates the embedded payload and extracted cache.
The backend independently checks the manifest trust anchor, every payload member,
exact membership and the source manifest before staging or importing build tools.
Only `state/` and the native extractor's bounded root `.lock` and `.complete`
markers are outside the immutable payload inventory. The manifest does not grant
permission to execute a file from an installation that a user may have edited.

Progress consists of UTF-8 JSONL records of at most 64 KiB. Each record has
`phase`, integer `done` and `total`, `message`, `status`, `installation` and `log`.
Successful checked results may additionally return `launcher`, `profile` and
`launcher_args`, the exact pair `['--profile-root', ABSOLUTE_PROFILE]`. The native
frontend opens that checked settings executable outside the backend's owned job.
Terminal statuses are `success`, `error`, `cancelled`, `needs-setup`, `needs-input`,
`up-to-date` and `update-available`. No public Windows update metadata means
`up-to-date`; Linux release assets are never substituted.

Builds use a new output for every attempt. Cancellation closes only the owned
Win32 process job, preserving the prior selected installation and failed output.
Owned build children discard inherited `PYTHON*` overrides, disable user site
packages and retain the checked interpreter's ordinary tool environment.
The durable checked-installation registry lives in `cache/state/`, outside source
and profiles. A failed atomic registry write leaves both the durable and in-memory
selection unchanged. Native file guards retain ordinary ancestors, require owned
single-link metadata and use expected hashes under a cooperating-writer mutex.
This is not a claim of protection against a malicious process running as the same
Windows user.

Adoption verifies a completed local game/settings pair and its original receipt.
For legacy main/SDK-only receipts, the original input must still be accessible to
establish the full exefs inventory before relocation. Locate validates that same
full inventory and the accepted RomFS inventory, then transactionally changes only
the two original receipt-declared input junctions. Original receipts, input files,
settings, saves and unknown human files are preserved. There is no save seeding.

Updates verify an independent Windows source release using the installed trusted
verifier before executing its builder. A checked compatible local game library
may be reused; only the driver's authenticated, typed compatibility mismatch
permits a second fresh build that regenerates it. Corrupt, missing, ownership,
hash, PE, compiler and unclassified failures never trigger this fallback. The
failed reuse attempt is retained separately under `installation/failed-builds/`.

The selected runtime changes by an atomic registry update while the original
profile remains stable. A completed update's exact source and derived artifact
inventory is snapshotted before activation. Current-only cleanup runs afterward,
using snapshot hashes bound to the durable registry. It preserves current/profile
roots and unknown, changed, locked or busy candidates. Deferred cleanup cannot
turn a successful activation into a failed update. No rollback selector or process
termination of human sessions is provided.

`tests/unit/test_bootstrap_windows.py` provides authored native component fixtures.
It replaces download and expensive game compilation boundaries explicitly; its
passing results do not establish an actual game build, gameplay, save/reload or
whole portable-executable pass.
