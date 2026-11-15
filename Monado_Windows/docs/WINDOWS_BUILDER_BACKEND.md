# Native portable builder backend

`tools/bootstrap_windows.py` is the native setup executable's trusted Python
backend. It runs from the persistent authenticated cache, never from an EXE
extraction temporary directory. It never starts the game or settings launcher.

The frontend invokes the cached `python/python.exe` with `-I -S -B` and
`setup/bootstrap_windows.py` with these arguments:

```
--action build|resume|adopt|locate-input|check-update|update|prepare-fsr4|select-fsr4|build-fsr4-tools|select-providers|download-dlss
--cache ABSOLUTE_CACHE
--payload-sha256 OUTER_PAYLOAD_SHA256
--manifest-sha256 PAYLOAD_MANIFEST_SHA256
--progress CACHE/state/FRESH.jsonl
--cancel-file CACHE/state/FRESH_CANCEL
--jobs 1..6
[--input EXTRACTED_GAME_FOLDER] [--destination INSTALLATION_FOLDER] [--precompile]
[--sdk LOCAL_SDK_FILE] [--tools LOCAL_CHECKED_TOOLS] [--component LOCAL_NATIVE_DLL]
[--assets EXISTING_VERIFIED_MODELS]
[--archives LOCAL_PINNED_ARCHIVES] [--support LOCAL_D3D_SUPPORT]
[--cmake LOCAL_EXE] [--ninja LOCAL_EXE] [--compiler LOCAL_EXE]
[--vulkan-prefix LOCAL_HEADERS_AND_IMPORT_LIBRARY]
[--fidelityfx LOCAL_SDK_FOLDER] [--streamline LOCAL_SDK_FOLDER]
[--manager-executable PORTABLE_BUILDER_EXE --manager-sha256 FULL_EXE_SHA256]
```

The native frontend authenticates the embedded payload and extracted cache.
The backend independently checks the manifest trust anchor, every payload member,
exact membership and the source manifest before staging or importing build tools.
The native frontend retains immutable file and ancestor leases through the child
lifetime. Only after full verification does Python append the checked bundled
site-packages directory; it never evaluates `.pth` or user customization.
Only `state/` and the native extractor's bounded root `.lock` and `.complete`
markers are outside the immutable payload inventory. The manifest does not grant
permission to execute a file from an installation that a user may have edited.

Progress consists of UTF-8 JSONL records of at most 64 KiB. Each record has
`phase`, integer `done` and `total`, `message`, `status`, `installation` and `log`.
The explicit `download-dlss` action ends with `status=download-ready` and
`downloaded_sdk` pointing to a private unselected review directory. It never
starts a compiler, changes the registry or selects a provider. The native UI
must review the supplied NVIDIA agreements and receive Use before passing that
source to the ordinary fresh-build/provider action; Cancel preserves the prior
selection and the downloaded review files. The fixed official URL, archive size
and SHA256 are authored in `windows_dlss_download.py`, never caller supplied.
Successful checked results may additionally return `launcher`, `profile` and
`launcher_args`, the exact pair `['--profile-root', ABSOLUTE_PROFILE]`, optionally
followed by `['--fsr4-assets', VERIFIED_ASSETS, '--fsr4-component', VERIFIED_DLL]`.
Recognized durable optional source choices are flat `fsr4_sdk`, `fsr4_tools`,
`fsr4_component`, `provider_fidelityfx` and `provider_streamline` fields. The native
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

Optional FSR4 preparation requires explicitly selected local SDK, native tools
and component; it downloads none of them. `fsr4_prepare_worker_windows.py` shares
fresh-attempt orchestration with the settings launcher. The fixed preparation
pipeline runs in the authenticated worker, and each native tool starts suspended
inside its own kill-on-close job. Real bounded stage events stream to the GUI.
Cancellation/failure leaves the prior selection and all human state intact;
retry uses a new `fsr4-preparations/prepare-TIMESTAMP-RANDOM` output.

Only the installed all-resource/component verifier can authorize activation.
The single durable `PROFILE/fsr4-preparation.json` changes atomically after final
validation, cancellation and game-session checks. Model files and settings are
not copied or overwritten; selection does not change the chosen upscaler. Reopen
revalidates resources/component before supplying launch arguments. An unavailable
or changed optional selection preserves its metadata and permits ordinary settings.

Source-built settings use an immutable `fsr4-preparation-context.json`, whose SHA
is embedded in the launcher and bound by its completed build receipt. The context
binds a pinned interpreter, every interpreter member, pipeline/import helpers and
locks. The native launcher leases these before isolated Python startup; the worker
rechecks owner, ordinary single-link identities, exact hashes, import membership
and receipt binding before imports. A source build lacking this pinned closure
reports optional preparation unavailable with portable-builder guidance.

The standalone worker accepts `--context ABS --context-sha256 SHA --profile ABS
--sdk ABS --tools ABS --component ABS --output FRESH --progress ABS --cancel-file
ABS`. `--verify-selection` instead needs only context, profile and progress/cancel
arguments. State files are fresh `PROFILE/logs/fsr4-preparation/progress-UUID.jsonl`
and `cancel-UUID`. No model, game, GPU or performance pass is inferred from the
worker's authored metadata fixtures; native synthetic GPU qualification is reported
only when the real pipeline's validator provides it.

Missing preparation tools can be built graphically from explicitly selected local
pinned source archives, provenance-receipted D3D support, CMake, Ninja and compiler.
The portable builder supplies its checked Vulkan headers/import library; the source
launcher selects a local Vulkan prefix explicitly. `build-fsr4-tools` and standalone
`--build-tools` share real stage progress, owned cancellation and fresh retry output
under `fsr4-preparation-tools/tools-TIMESTAMP-RANDOM`. Full receipt/PE/import closure
verification and a fresh build of the reviewed native Vulkan component using the
same compiler/prefix precede atomic publication of `PROFILE/fsr4-tools.json`.
Component outputs use `fsr4-components/component-TIMESTAMP-RANDOM`; failed component
builds preserve the previous tools and model. Tool selection
is separate from model selection. `--verify-tools` verifies it on reopen; combined
`--verify-selection` can report verified tools while no model exists. Failed, locked,
unknown or changed selections retain their files. No compiler/source boundary mock
in the worker fixtures constitutes an actual native tool build.

Optional shader preparation supplies a fresh session and the checked runtime PE
hash. Exit zero is insufficient: the guarded final work record must match both
tokens, report phase `done`/state `complete`, zero failures/errors/skips and all
work totals complete. Cancellation publishes only the exclusive new current-session
cancel file, allows a bounded graceful exit and then closes only the owned job.

`tests/unit/test_bootstrap_windows.py` provides authored native component fixtures.
It replaces download and expensive game compilation boundaries explicitly; its
passing results do not establish an actual game build, gameplay, save/reload or
whole portable-executable pass.
