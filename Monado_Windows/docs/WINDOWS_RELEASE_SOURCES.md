# Windows dependency source companions

The Windows setup contains reviewed open-source tools and native dependencies.
It does not contain the game library, extracted game data, proprietary provider
SDKs, prepared models, translated game shaders, saves or private run logs. Users
generate their game library from their own extracted input.

Ship the setup, exact Monado Windows source archive, native source companion and
Python/tool source companion together. Keep their checksums in the release
metadata. An experimental renderer label does not waive dependency licenses.

## Native dependency evidence

`tools/windows_release_sources.py` reads the setup's authenticated payload
manifest. Every selected native file and the two statically linked frontend
libraries is matched to its original hash-pinned MSYS2 binary package. The
original `.PKGINFO` and `.BUILDINFO` are retained; the latter pins the exact
`PKGBUILD` used for that binary. The matching official all-source archive
contains the recipe, patches, upstream source archives or Git object stores.
The collector executes none of those recipes or upstream programs.

`tools/windows-release-native-source-review.json` pins the complete reviewed
package ownership, source disposition, source inputs, archive identities,
notices and companion file inventory. The verifier compares the actual files
and final native payload to that independent inventory. Deleting a package,
setting `source_required` false or adding an unrelated file fails verification.

The current reviewed dependency closure has 126 binary package owners, 115
retained all-source archives, 77 exact shipped header source files and 524
notices. Required GPL/LGPL sources include the matching GCC runtime, FFmpeg,
codec libraries, libiconv and their applicable patches/build recipes. Seven
required Git inputs are checked by resolving the pinned revision and hashing
the same `git archive --format tar` bytes used by makepkg 6.1. Source Git config,
hooks, alternates, replacements, network operations and inherited Git overrides
are excluded from that verification.

The source review also records nested archive member names and byte identities.
Compiler/codec test inputs, dummy DLL-loader fixtures and authored SPIR-V test
vectors remain part of their upstream source distributions. They are not game
caches or trained models. Four permissive packages contribute only headers:
Whisper, llama.cpp, Tcl and Vulkan Memory Allocator. Their exact shipped headers
and notices are provided instead of optional full upstream examples, platform
executables or model-associated test containers. The llama GGUF files inspected
in the omitted archive contain zero tensors; they are vocabulary/metadata, not
trained weights. GGML's retained source archive has no model containers.

Graphite's source archive is repacked only to remove an unrelated website font
demo ZIP containing an installer. `SOURCE-REPACK.json` records the original and
provided archive/recipe hashes, removed member identity and unchanged source
member inventory. Original recipes are preserved. The provided recipe changes
only the local source location and its checksum. All library/test source and
patches remain byte-identical.

To rebuild a native dependency, unpack its retained all-source archive in the
matching MSYS2 UCRT64 build environment and use its `PKGBUILD` and patches. Read
the retained `.BUILDINFO` for the original environment/tool versions. Git
object stores are included for the packages that use them. This collection is
source delivery and verification; it does not automatically install MSYS2 or
execute package build scripts on the user's computer. MSYS2 describes its
[package recipes and build process](https://www.msys2.org/wiki/Creating-Packages/)
and notes that its packages have [independent licenses](https://www.msys2.org/license/).

## Static frontend relinking

The branded portable frontend statically links SDL3 and libiconv. The final
native companion additionally contains its seven authored frontend objects,
Windows resource object, both exact static libraries, relative Zig 0.14.1
link recipe and setup-payload repacker. It contains no guest/game objects.
The release gate binds those inputs to the actual newly compiled frontend.

The included `relink/README.txt` explains how to replace/rebuild libiconv, relink
`builder-base.exe`, and combine that new base with the unchanged authenticated
payload from the published setup. The output footer's payload offset is updated
without executing the setup or modifying its payload. This makes the modified
library usable in the complete portable program, rather than producing only an
unusable frontend base. See [LGPL 2.1 section 6](https://www.gnu.org/licenses/old-licenses/lgpl-2.1.en.html)
for the relinking source/object option.

`seal_relink(report, frontend_root, fresh_output)` creates a new immutable
companion. `verify_report(report, payload_files=..., release_version=...,
static_files=..., relink_root=...)` must pass against the final setup and
frontend. A previous dependency-only component is not the final sealed frontend
companion.
