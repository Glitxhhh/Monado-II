# FSR 4 Lite capture tooling (bbport)

[XCDE] `tools/fsr4cap` of https://github.com/deadinside28/bloodborne_pc at commit
`0fc2921c45f8ff3900f6bfa43a299e9a4f532af4` (GPL-2.0-or-later, `LICENSE`), unmodified. The FidelityFX API headers
under `ffx/` are AMD's (MIT, see their headers).

Monado uses it to prepare FSR 4.1.2 Lite's model files **on the player's own PC**, the way bbport does: nothing of
AMD's is shipped. `tools/fsr4_prepare.py` (frozen as `monado-fsr4prep`, run by the launcher's "Prepare FSR 4 Lite"):

1. downloads AMD's `amd_fidelityfx_upscaler_dx12.dll` and its licence from AMD's FidelityFX SDK 2.4.0 on GitHub
   (pinned commit, SHA-256 checked);
2. `driver_model.py force ... lite` restricts it to the Lite INT8 model;
3. records its passes: `build_assets.sh` under the player's Proton on Linux, `build_assets_win.py` on Windows
   (vkd3d-proton's `d3d12.dll` / `d3d12core.dll` and DXVK's `dxgi.dll` beside `fsr4cap.exe`);
4. `extract.py` translates them to SPIR-V with `dxil-spirv` (pinned, `dxil-spirv-class-bindings.patch`) and checks
   the replay rules; the result goes to `<data folder>/fsr4/lite`.

The packages bring the prebuilt tools (`fsr4cap.exe`, `amdxc64.dll`, `dxil-spirv`, `spirv-as`, `spirv-dis`, and on
Windows vkd3d-proton / DXVK): `.github/workflows/release.yml`.
