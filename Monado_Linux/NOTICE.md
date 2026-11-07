# NOTICE

Monado is an independent project, not affiliated with or endorsed by Nintendo or Monolith Soft. *Xenoblade
Chronicles* is a trademark of Nintendo. This repository contains no game code, assets, keys, firmware or system
fonts (see `CONTRIBUTING.md`, Article I). Players must supply files extracted from their own copy.

## Derived work

| Component | Origin | License | Where |
|---|---|---|---|
| AArch64→C generator (`gen.py`, `lifter.py`, `simd_c.py`, `analyze.py`, `nso.py`, `sym.py`) | [LASRecomp](https://github.com/peiikai/LASRecomp) by Peiikai, commit `136f679` | MIT | `tools/lifter/` |
| Runtime (loader, dispatch, kernel, IPC, services, NVN→Vulkan, Maxwell shader translator) | LASRecomp, commit `136f679` | MIT | `runtime/` |

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
| DejaVu Sans / Serif launcher fonts, unmodified copies | Bitstream Vera font licence; DejaVu changes public domain (`tools/launcher/assets/fonts/LICENSE-DejaVu.txt`) |

## Linked at build time (not in this repository)

| Component | License |
|---|---|
| SDL3 | zlib |
| zlib (Linux graphics cache and keyboard-image compression) | zlib; Jean-loup Gailly and Mark Adler, package notice retained in the local sysroot |
| Vulkan headers / loader, Vulkan Memory Allocator | Apache-2.0 / MIT |
| shaderc (glslang, SPIRV-Tools) | Apache-2.0 |
| LLVM libc++, libc++abi and compiler runtime supplied by the pinned Zig C++ toolchain | Apache-2.0 with LLVM Exceptions; original toolchain notices remain in the ignored local compiler installation |
| SPIRV-Reflect (Google Inc., SDK shader-build helper only; not linked into the runtime companion) | Apache-2.0; original copyright and license references retained in the pinned SDK source |
| FFmpeg (original Windows LGPL build from MSYS2) | LGPL-2.1+ |
| FFmpeg (Linux development packages pinned in `graphics-deps-linux.lock.json`) | Runtime reports GPL version 2 or later; Ubuntu build enables GPL components |
| libopus | BSD-3-Clause |
| Capstone, Unicorn, lz4 (build-time Python tools, not linked into the game) | BSD-3-Clause / GPL-2.0 / BSD-2-Clause |

### Linux development dependency receipt [XCDE] LINUX-002

The locally extracted Ubuntu development sysroot is ignored and is not a release
bundle. `scripts/setup_linux_graphics.py` verifies the pinned package archives and
retains their copyright/license files under the sysroot's `usr/share/doc/`.
The tested FFmpeg 8.0.1 binaries report **GPL version 2 or later**, so the original
Windows LGPL-only dependency description does not apply to these Linux binaries.
No Linux distribution package is approved by this development test. Selection of
release dependencies and their notices remains a packaging gate; this record does
not change the MIT attribution of the authored runtime/generator source.

## Optional runtime dependencies

### Native Linux FidelityFX source dependency

The optional Linux frame-generation companion is built locally from AMD's MIT-licensed
[FidelityFX SDK 1.1.4 source](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/tree/c6efa6bf7f2027b3ec94f28578bb5965eabb9e55).
Copyright (C) 2024 Advanced Micro Devices, Inc. The pinned source, build working copy and compiled shaders/library
remain in ignored `.deps/fsr/`. `fsr-linux.lock.json` and `scripts/setup_linux_fsr.py` record the source identities,
Linux portability changes and native build recipe; the complete MIT notice accompanies development builds as
`FidelityFX-MIT.txt`. This uses source-built interpolation with Monado's ordinary Vulkan presentation, without
the Windows SDK presenter, Windows DLLs or game-derived shader data. No release bundle is approved by this setup.

### Other optional components

| Component | Use |
|---|---|
| AMD FidelityFX SDK 1.1.4 signed DLL (`amd_fidelityfx_vk.dll`) | FSR 3.1 upscaling and frame generation |
| NVIDIA Streamline 2.14.16 + DLSS DLLs | DLSS |
| NVIDIA NGX / DLSS SDK 310.9.1, commit `374959484e79a640feaba44c93ac8cfb0a03f5b5` | Native Linux Vulkan Super Resolution / DLAA; separate NVIDIA proprietary [license](https://github.com/NVIDIA/DLSS/blob/374959484e79a640feaba44c93ac8cfb0a03f5b5/LICENSE.txt), retained with ignored local SDK and development build. No SDK headers, archive, SR binary or models are tracked. `dlss-linux.lock.json` pins the official files; the authored bridge loads only after offline policy. This is local development, not an approved redistribution bundle. |
| Button icon packs from GameBanana (CompC: "Xbox control layout", "PS5 DualSense control layout", CC BY-NC-ND) | controller prompts; installed by the user into `mods/` |
