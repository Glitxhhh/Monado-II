# Current Windows development target

**Preparation only: implement the accepted Linux dev104 R5 feature set natively
on Windows.** Read [AGENTS](../AGENTS.md) and the complete
[Windows handoff](WINDOWS_PORT_HANDOFF.md) before executing inherited setup/build
scripts. The first updated Windows release must include the portable graphical
.exe builder/launcher and all applicable current Linux functionality. Root
Build_Monado_II.bat and Play_Monado_II.bat are fallback requirements to implement;
they are not supplied or validated by this preparation. No installer is required.

Use Monado_Windows as the implementation target, retain native Win32/provider
behavior, validate owned extracted DE2.0.0 input, generate game code locally and
keep all Windows state/output separate. Audit and fix compiler-failure propagation,
process-name-wide taskkill, hardcoded/unquoted paths and broad system setup before
running the historical recipe below. Source CI does not establish a native build.
Existing original-Windows release instructions remain historical; they do not
describe a completed parity build. No Linux distro/sysroot/glibc prerequisites apply
to Windows. The detailed handoff supplies phased implementation and acceptance gates.

# Local Windows build

Download **`monado-ii-windows-original-implementation-setup.zip`** from the GitHub
Release. **This is the original Windows implementation, without the Linux
additions.** Linux release numbers do not indicate a Windows update. The filename
and implementation identity remain unchanged until Windows itself is updated.

The kit contains reviewed source and inherited build tools, not a ready-to-run
Windows app. Quiver recognizing the archive does not establish Windows setup or
gameplay compatibility. The original author reported playability; this fork has
not independently built or validated Windows.

The new Linux launcher, high-refresh corrections, expanded saves, dedicated Quick
Step slot, Cheats tab, updater, controller/RADV corrections and other Linux work
have **not** been backported. [TODO](../TODO.md) records deferred parity references.
The containing source snapshot is recorded separately as provenance, not as a new
Windows implementation version. Windows has no updater in this kit.

You need your own legally dumped DE2.0.0 game files from your own legally obtained
Switch and game. No game files, keys, acquisition links or dumping instructions are
provided. Use root `game_files/exefs` and `game_files/romfs` as the shared read-only
input. Keep outputs and runtime state under `Monado_Windows`, separate from Linux.

The inherited route uses Windows10/11 x86-64, Git Bash, Python3.12+, MSYS2 UCRT64
at its expected path and the SDL3/Vulkan/shaderc/VMA/FFmpeg/Opus development
libraries. See the inherited per-port README/NOTICE for dependency descriptions.
Review `tools/setup_windows.ps1` before any explicit invocation: it installs and
updates system tools; publication preparation did not run it. No system/toolchain
installation is implied by unpacking the source kit.

After installing the prerequisites yourself, open Git Bash at the Monado II root:

```sh
cd Monado_Windows
python -m venv .venv
.venv/Scripts/python -m pip install capstone unicorn lz4 ziglang==0.14.1
# Select the already-extracted shared input using the inherited local-folder UI.
powershell.exe -NoProfile -File tools/setup_game_files.ps1
.venv/Scripts/python -X utf8 tools/extract/check_dump.py targets
# Explicitly setting XCDE_EXE bypasses inherited taskkill while retaining the
# filename expected by the Windows launcher. Close your game before rebuilding.
XCDE_EXE=build/xcde.exe bash scripts/build_all.sh
```

Select root `game_files` in the folder UI. It links input rather than bundling or
copying it. When the local build succeeds, open `build/xcde_launcher.exe` manually.
This inspected recipe has not been executed on Windows; clean-machine validation
remains TODO. The inherited `Build Monado.bat` route uses the expected filename,
offers setup, and may terminate a running process named xcde.exe during rebuilding;
close your own game before choosing that route. It was not executed here.

No vendor DLLs, system/game fonts, private SDKs, generated game executable or shader
caches are shipped. The public source supports plain TTF/OTF/TTC input only. Never
run inherited development packaging scripts to create public releases; the unsafe
game-copying packager is excluded. Use only [source release tooling](RELEASING.md).
