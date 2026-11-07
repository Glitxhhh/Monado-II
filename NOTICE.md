# Monado II notices

Monado II is a fork of Monado by TheMonadoGuy / Monado contributors, based on
LASRecomp by Peiikai. Preserve the complete inherited MIT copyright/permission
notice in each port's `LICENSE`, plus the root [LICENSE](LICENSE) and
[CREDITS](CREDITS.md). A fresh publication history does not change authorship.

Each port's detailed `NOTICE.md` records upstream code and dependency terms:
[Linux](Monado_Linux/NOTICE.md), [Windows](Monado_Windows/NOTICE.md). A platform-only
source kit includes the notice for its selected port; the other port's link applies
to the full source repository.

Reviewed source dependencies include Dear ImGui (MIT), SMAA and its upstream lookup
tables (MIT-style), AMD FidelityFX API headers (MIT), NVIDIA Streamline headers
(MIT), bcdec (MIT/Unlicense), and unmodified launcher DejaVu fonts (Bitstream Vera
license, DejaVu changes public domain). Full included notices accompany their
source. Unused third-party example fonts and examples are omitted from source kits.
SMAA.hlsl source encoding is normalized from upstream CP1252 to UTF-8; its license
and source characters are preserved. Two legacy test categories containing literal
generated-function signatures are conservatively held out; authored source guard
fixtures cover the public publication boundary instead.

Locally linked dependencies retain their own terms. In particular, the tested
Linux FFmpeg development packages enable GPL components; the optional NVIDIA NGX
SDK has proprietary terms. Source releases do not redistribute those runtime
libraries, vendor models or development sysroots. They are not approved portable
binary bundles. Selecting a future binary dependency bundle requires a separate
license/provenance review and corresponding notices/source obligations.

No game input, recompiled game code/binary, game-derived shader/cache, Nintendo
font, key, firmware, save, third-party mod or unverified local artwork/icon belongs
in this source distribution. Original authored shader source and licensed SMAA
lookup data are distinct from translated game shaders. No game-download or key
acquisition service is provided.

The optional local launcher AppImage builder downloads the exact hash-locked
official AppImage type2 runtime. Its source is MIT-licensed, with statically linked
dependencies under their respective terms. Preserved runtime/musl/libfuse/
squashfuse/zstd/zlib/mimalloc notices are in `tools/appimage/` and accompany each
locally generated image. The generic Roman numeral XPM is authored Monado II code
art under the project MIT license. Images contain the locally compiled launcher,
driver and notices; game executables and system/vendor library bundles stay out.
The generated image is a local workspace launcher, not a reviewed public binary
distribution or a substitute for the dependency notices of the external build.

Xenoblade Chronicles is a Nintendo trademark. Game content belongs to its rights
holders. Monado II is independent and unaffiliated; its source license grants no
rights to game content or other third-party intellectual property.
