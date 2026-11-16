# NOTICE

Monado is an independent project, not affiliated with or endorsed by Nintendo or Monolith Soft. *Xenoblade
Chronicles* is a trademark of Nintendo. This repository contains no game code, assets, keys, firmware or system
fonts (see `CONTRIBUTING.md`, Article I). Players must supply files extracted from their own copy.

## Derived work

| Component | Origin | License | Where |
|---|---|---|---|
| AArch64→C generator (`gen.py`, `lifter.py`, `simd_c.py`, `analyze.py`, `nso.py`, `sym.py`) | [LASRecomp](https://github.com/peiikai/LASRecomp) by Peiikai, commit `136f679` | MIT | `tools/lifter/` |
| Runtime (loader, dispatch, kernel, IPC, services, NVN→Vulkan, Maxwell shader translator) | LASRecomp, commit `136f679` | MIT | `runtime/` |
| Native correctly rounded `fma` / `fmaf` compatibility | musl math sources bundled by pinned Zig 0.14.1; exact per-file hashes retained | MIT / FreeBSD two-clause | `runtime/platform/windows_fma.c`, `third_party/musl_math/` |

Every edit to a copied file is marked `[XCDE]` (the project's working name) and listed in `docs/UPSTREAM.md`.

## Reference only (no code copied)

| Project | License | Use |
|---|---|---|
| ssbu-recomp (Denkishi), commit `938e66b` | GPL-3.0 | design reference; copying code requires an ADR accepting GPL-3.0 for the whole project |
| B2recomp (K0NL) | GPL-3.0 | design reference |
| Other Switch recomps and PC ports reviewed for ideas (commits and issues) | various | listed in `docs/RECOMP_KNOWLEDGE.md`; no code copied |

## Third-party components in this repository (`third_party/`)

| Component | License |
|---|---|
| Dear ImGui | MIT (`third_party/imgui/LICENSE.txt`) |
| SMAA (reference shader and lookup textures) | MIT-style (`third_party/smaa/LICENSE.txt`) |
| AMD FidelityFX API headers (FSR 3.1 / frame generation) | MIT |
| NVIDIA Streamline headers (DLSS) | MIT (`third_party/streamline/license.txt`) |
| bcdec (BC texture decoding) | MIT or public domain (Unlicense), at the user's choice |
| Native baseline FMA compatibility (`runtime/platform/windows_fma.c`) | musl MIT / FreeBSD two-clause; complete notices and exact source provenance in `third_party/musl_math/` |

## Linked at build time (not in this repository)

| Component | License |
|---|---|
| SDL3 | zlib |
| Vulkan headers / loader, Vulkan Memory Allocator | Apache-2.0 / MIT |
| shaderc (glslang, SPIRV-Tools) | Apache-2.0 |
| FFmpeg (selected local build) | LGPL-2.1+ or GPL, according to enabled components |
| libopus | BSD-3-Clause |
| Capstone, Unicorn, lz4 (build-time Python tools, not linked into the game) | BSD-3-Clause / GPL-2.0 / BSD-2-Clause |

The native Windows development dependency lock records the exact official MSYS2
packages, hashes and declared licenses. Its FFmpeg closure includes GPL-enabled
components; the historical LGPL-only description does not describe this local
development build. Complete package notices remain in the private dependency
tree. These local dependencies are not an approved public binary bundle; any
distribution must satisfy the actual selected components' licenses and applicable
corresponding-source obligations.

## Optional vendor binaries (not redistributed; each user downloads them)

The optional native Windows FSR4 Lite component retains GPL-2.0-or-later and the
complete AMD SDK notice in `third_party/fsr4_native`. Its MIT API header does not
relicense the backend. Combined distribution requires applicable GPL terms and
corresponding combined source/build scripts; dynamic linking is no exemption.
The local preparation recorder/translator sources under `third_party/fsr4cap`
retain GPL-2.0-or-later and their exact upstream notices; AMD API headers and the
AMD extension shim retain their MIT notices. Native process/I/O adapters and the
authored Vulkan validator retain MIT attribution. No model, vendor SDK DLL,
capture or translated shader is included in source.
See `docs/WINDOWS_FSR4.md` for exact provenance and current preparation limits.

| Component | Use |
|---|---|
| AMD FidelityFX SDK 1.1.4 signed DLL (`amd_fidelityfx_vk.dll`) | FSR 3.1 upscaling and frame generation |
| NVIDIA Streamline 2.14.16 + DLSS DLLs | DLSS |
| Button icon packs from GameBanana (CompC: "Xbox control layout", "PS5 DualSense control layout", CC BY-NC-ND) | controller prompts; installed by the user into `mods/` |
