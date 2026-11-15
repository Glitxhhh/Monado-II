# Optional local Windows providers

The portable manager accepts an explicitly selected local FidelityFX or
Streamline SDK folder. It does not download these SDKs, install a driver or
accept an agreement. Defaults remain unchanged. Selecting a provider does not
enable an upscaler or frame generation.

FidelityFX uses `amd_fidelityfx_vk.dll` with the five Vulkan API entry points
used by this runtime. The accepted native API is the SDK 1.1.4 Vulkan API;
newer DirectX-only SDK DLLs are not substitutes. Streamline uses
`sl.interposer.dll`, `sl.common.dll`, `sl.dlss.dll` and `nvngx_dlss.dll` together.
Choose the SDK root or one flat folder containing these DLLs and their licenses.
Recognized SDK binary subfolders are `bin`, `bin/x64` and `bin/x64/production`.
Ambiguous duplicate copies are refused.

An SDK `LICENSE`, `LICENSE.txt`, `LICENSE.md` or `license.txt` is required.
Streamline additionally requires `nvngx_dlss.license.txt`. Read that NVIDIA
license before choosing Build or Apply: use of the DLSS SDK can accept its
agreement. Monado does not accept it on your behalf. Streamline's own license
does not replace the separate DLSS license.

The manager checks ordinary current-user-owned files and every ancestor, AMD64
PE DLL format, required exports, normal and delayed imports and exact content
hashes without loading the DLLs. Only the named providers and a finite list of
Microsoft VC runtime dependencies are eligible for copying. A missing named VC
runtime may use the actual protected `GetSystemDirectoryW` installation after
an ordinary-file, PE and Windows/TrustedInstaller/Administrators owner check.
Its path and content identity are recorded as an installed system dependency;
it is not copied. A missing runtime produces manual installation guidance.
Neither `PATH` nor an environment-provided Windows directory selects it.

Applying a selection builds a fresh runtime and attempts checked whole-GameLib
reuse. An authenticated compatibility change can require local regeneration.
Provider DLLs, copied license files and `windows-providers.json` are bound by the
new build receipt before activation. The original runtime, receipt, settings,
saves and stable profile are preserved. A changed retained SDK source must be
restored or explicitly selected again. Cancellation/failure preserves the prior
selection and retains the failed fresh attempt for inspection.

The manual source workflow accepts the same explicit selections:

```powershell
python -B scripts/build_windows.py --stage all --split-game --input C:\MyGame `
  --output build\fresh-provider-build --prefix C:\MyTools\ucrt64 `
  --fidelityfx C:\MySDKs\FidelityFX --streamline C:\MySDKs\Streamline
```

Both provider flags are optional. Existing runtime files are never overwritten.
The portable backend's `select-providers` action uses its authenticated local
source kit to build a fresh candidate; it does not invent a public release or
require an online update offer. Build and Update retain the last known source
identities unless the user explicitly selects a replacement. The durable
installation registry records those selections only after successful activation.

For a build launched by the portable manager, the backend can also pass these
four driver arguments together:

```text
--preparation-manager ABSOLUTE_BUILDER_EXE
--preparation-manager-sha256 FULL_EXE_SHA256
--preparation-manager-payload-sha256 EMBEDDED_PAYLOAD_SHA256
--preparation-manager-manifest-sha256 PAYLOAD_MANIFEST_SHA256
```

They bind an optional management executable in the immutable launcher context.
The frontend and backend verify its complete file and payload/footer identity;
the context's SHA is compiled into the settings launcher. A missing or moved
manager does not disable ordinary settings or FSR4 preparation. Source builds
without that checked manager retain the manual provider flags above.

Keep SDK binaries and licenses private unless their redistribution terms have
been reviewed. Source exports and the portable manager payload exclude them;
the user stages them locally. Receipt provenance and local integrity checks do
not establish a compatible GPU, actual provider dispatch, image correctness,
performance or physical scanout. Those require native runtime qualification.

The expected Streamline DLL grouping is described by NVIDIA's
[programming guide](https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuide.md).
AMD documents the accepted SDK generation in its
[FidelityFX 1.1.4 manual](https://gpuopen.com/manuals/fidelityfx_sdk/).
