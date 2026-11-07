# Local Linux build

Supply only your own legally dumped DE2.0.0 input from your own legally obtained
Switch and game. No files, keys, acquisition links or dumping instructions are
provided. Place already-extracted input in root `game_files/exefs` and
`game_files/romfs`; keep it read-only. All commands start at the Monado II root.

The recorded development route requires **Linux x86-64 / CPython3.14**, bash,
`pkg-config`, `dpkg-deb`, `readelf`, Vulkan support and compatible host transitive libraries. It
uses a project-local pinned Ubuntu sysroot, not system package installation. The
wheel lock is specific to this platform/Python and intentionally rejects others.
Read the scripts and dependency notices before explicitly fetching build tools.

```sh
# Explicitly creates a port-local venv/tool wrappers and installs locked wheels.
python3.14 -B tools/setup_linux_tools.py --port Monado_Linux --install
# Read-only verification of the local interpreter/packages, Zig and GNU wrappers.
python3.14 -B tools/setup_linux_tools.py --port Monado_Linux --check
cd Monado_Linux
# Fetch/verify/extract pinned development dependencies locally; no system install.
.venv/bin/python -B scripts/setup_linux_graphics.py
.venv/bin/python -B tools/extract/check_dump.py ../game_files
.venv/bin/python -B tools/gen_all.py --exefs ../game_files/exefs --out generated/local --simd c
.venv/bin/python -B scripts/build_linux.py --generated generated/local --output build/local-game --jobs 6
.venv/bin/python -B scripts/build_launcher_linux.py --output build/local-launcher --jobs 4
.venv/bin/python -B scripts/launcher_linux.py --launcher-executable build/local-launcher/monado-launcher --game-executable build/local-game/xcde-linux --dry-run
```

Build outputs must be fresh; select a new directory when rebuilding. The dry-run
checks inputs, build receipts, dependencies and isolated runtime paths without
opening the game. When ready to test manually, run the same launcher command
without `--dry-run`. The original dated developer defaults are not release builds;
always pass your newly built paths. No automatic Play is part of this recipe.

The tool bootstrap supports this exact CPython/platform wheel baseline. Without
`--install` or `--check`, it prints a plan and writes nothing. Installation uses
hash-checked binary wheels inside `Monado_Linux/.venv` and creates compiler wrappers
and a receipt under `Monado_Linux/.deps`; it does not install system packages or
replace unrelated existing compiler wrappers. The public launcher embeds licensed
fonts and uses its solid theme without the excluded local scenic artwork/icon.

The baseline build above omits optional DLSS/FG vendor components. Their source
setup/lock scripts are present for explicit development use, but proprietary SDK
binaries/models and generated provider libraries are not shipped. A plain baseline
build exposes honest supported graphics choices. See [status](PLATFORM_STATUS.md)
before interpreting test coverage. Missing host libraries should be resolved by
the user for their distribution, without treating the Ubuntu sysroot as a portable
runtime bundle. Clean-machine installation remains a tracked validation task.
