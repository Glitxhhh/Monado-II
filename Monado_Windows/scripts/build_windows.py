#!/usr/bin/env python3
"""[XCDE] Native Windows build driver. Never launches the game or installs tools.

Fresh outputs, exact compile/link membership, strict FP/assertions, checked child
exit codes and private evidence replace the inherited stale-object shell recipe.
"""
from __future__ import annotations

import argparse
import concurrent.futures
import ctypes
from datetime import datetime, timezone
import hashlib
import importlib.metadata
import importlib.util
import json
import os
from pathlib import Path
import platform
import re
import shutil
import struct
import subprocess
import sys
import threading
import time
import uuid

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
RUNTIME = """loader dispatch kernel ipc fnprof platform/windows platform/windows_fma services/fs services/romfs_virtual
services/am services/acc services/time services/hid_set services/kbicons services/misc services/audio services/hwopus
hle/heaps hle/pl hle/vi hle/nvn_boot hle/fibers hle/input hle/party hle/bindings hle/movie hle/framepace
hle/graphics hle/foliage_distance hle/title_exit hle/desktop_exit_caption hle/system_exit hle/expanded_saves hle/expanded_save_scan
hle/quickstep_slot hle/quickstep_save hle/quickstep_native hle/quickstep_picker hle/quickstep_stats hle/quickstep_stats_ui hle/quickstep_ui_memory
hle/cheats_rewards hle/cheats_combat host/stubs host/settings_ini host/run_diagnostics_windows host/stubs_gfx host/xcde_aspect host/gamelib_static host/main_boot
diag/diag diag/sampler diag/watchdog diag/crashdump paths""".split()
NVN = """astc cmdbuf formats maxwell/translate objects output_scale smaa_data tegra texpatch
vulkan/device vulkan/exec vulkan/host_import vulkan/precompile vulkan/shaders vulkan/textures vulkan/temporal""".split()
IMGUI = ["third_party/imgui/" + n + ".cpp" for n in ("imgui", "imgui_draw", "imgui_tables", "imgui_widgets")]
CPP = IMGUI + ["third_party/imgui/backends/imgui_impl_vulkan.cpp", "runtime/nvn/vulkan/imgui_glue.cpp",
               "runtime/ui/menu_vk.cpp", "runtime/nvn/vulkan/dlss.cpp", "runtime/nvn/vulkan/vma.cpp"]
LAUNCHER = IMGUI + ["third_party/imgui/backends/imgui_impl_sdl3.cpp",
                    "third_party/imgui/backends/imgui_impl_sdlrenderer3.cpp", "tools/launcher/launcher.cpp"]
LIBRARIES = ["SDL3", "shaderc_shared", "vulkan-1", "opus", "avcodec", "avformat", "avutil", "swscale", "swresample", "z"]
FLAGS = ["-O1", "-UNDEBUG", "-march=x86_64_v2", "-ffp-contract=off", "-fno-fast-math", "-fno-strict-aliasing", "-fvisibility=hidden"]


def validate_windows_baseline(value):
    """Source identity only; public artifact approval is a separate boundary."""
    legacy = {"schema", "accepted_baseline", "implementation", "public_release"}
    if (not isinstance(value, dict) or type(value.get("schema")) is not int or value["schema"] != 1
            or value.get("accepted_baseline") != "dev104 R5" or value.get("public_release") is not False):
        raise RuntimeError("Unrecognized native Windows source baseline")
    if set(value) == legacy and value["implementation"] == "Windows development":
        return value
    version = value.get("version")
    if (set(value) == legacy | {"version", "release_channel"}
            and value["implementation"] == "Windows Experimental"
            and value["release_channel"] == "experimental"
            and isinstance(version, str) and len(version) <= 64
            and re.fullmatch(r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)", version)):
        return value
    raise RuntimeError("Unrecognized native Windows source baseline")


def read_windows_baseline(path):
    def unique(pairs):
        value = {}
        for key, item in pairs:
            if key in value:
                raise ValueError("Duplicate native Windows baseline key")
            value[key] = item
        return value
    try:
        if not ordinary(path) or path.stat().st_size > 4096:
            raise ValueError("Nonordinary or oversized native Windows baseline")
        raw = path.read_bytes()
        if len(raw) > 4096:
            raise ValueError("Oversized native Windows baseline")
        value = json.loads(raw, object_pairs_hook=unique)
    except (OSError, ValueError) as error:
        raise RuntimeError("Unreadable native Windows source baseline") from error
    return validate_windows_baseline(value), hashlib.sha256(raw).hexdigest()


def windows_baseline_label(value):
    value = validate_windows_baseline(value)
    implementation = value["implementation"] + (" " + value["version"] if "version" in value else "")
    return value["accepted_baseline"] + " · " + implementation


def digest(path):
    with path.open("rb") as f:
        return hashlib.file_digest(f, "sha256").hexdigest()


def ordinary(path):
    return path.is_file() and not path.is_symlink() and not path.is_junction()


def process_options(command, cwd, *, cwd_independent=False):
    """Spell the application explicitly and avoid CreateProcess's cwd limit.

    Extended executable names are supported by lpApplicationName; extended
    lpCurrentDirectory names still fail on supported Windows hosts. Only callers
    using absolute input/output arguments may opt into a neutral short cwd. This
    changes no path identity, process-global cwd, or profile-relative state.
    """
    if os.name != "nt":
        return {"cwd": cwd}
    executable = Path(command[0])
    directory = Path(cwd)
    if not executable.is_absolute() or not directory.is_absolute():
        raise ValueError("Native child executable and working directory must be absolute")
    application = str(executable)
    if not application.startswith("\\\\?\\") and len(application.encode("utf-16-le")) // 2 + 1 >= 260:
        application = "\\\\?\\UNC\\" + application[2:] if application.startswith("\\\\") else "\\\\?\\" + application
    # Count native UTF-16 units, including the terminating NUL. Prefixing a
    # too-long cwd does not remove this particular CreateProcess limitation.
    if len(str(directory).encode("utf-16-le")) // 2 + 1 >= 260:
        if not cwd_independent:
            raise ValueError("This profile-relative native child needs a working directory shorter than 260 characters; choose a shorter profile location")
        if not directory.is_dir():
            raise ValueError("Native child working directory is unavailable")
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.GetSystemDirectoryW.argtypes = [ctypes.c_wchar_p, ctypes.c_uint]
        kernel.GetSystemDirectoryW.restype = ctypes.c_uint
        buffer = ctypes.create_unicode_buffer(32768)
        length = kernel.GetSystemDirectoryW(buffer, len(buffer))
        if not length or length >= 259:
            raise OSError("No short native system directory is available for compiler startup")
        directory = Path(buffer.value)
    return {"executable": application, "cwd": str(directory)}


def git_identity():
    # A source-only export can be nested below an unrelated checkout. Never
    # search upward and attribute that enclosing repository to this build.
    repository = ROOT.parent
    marker = repository / ".git"
    if not marker.exists() or marker.is_symlink() or marker.is_junction():
        return {"git_commit": None, "git_branch": None}
    try:
        return {"git_commit": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=repository, stderr=subprocess.DEVNULL, text=True).strip(),
                "git_branch": subprocess.check_output(["git", "branch", "--show-current"], cwd=repository, stderr=subprocess.DEVNULL, text=True).strip()}
    except (OSError, subprocess.CalledProcessError):
        return {"git_commit": None, "git_branch": None}


def atomic_json(path, value):
    tmp = path.with_name(path.name + "." + uuid.uuid4().hex + ".tmp")
    with tmp.open("x", encoding="utf-8", newline="\n") as stream:
        stream.write(json.dumps(value, indent=2, ensure_ascii=False) + "\n")
        stream.flush()
        os.fsync(stream.fileno())
    tmp.replace(path)


def generator_closure():
    files = list((ROOT / "tools/lifter").glob("*.py"))
    files += [ROOT / n for n in ("tools/gen_all.py", "games/xcde/profile.json", "games/xcde/patches.py", "games/xcde/hle_symbols.txt")]
    files.append(ROOT / "runtime/platform/windows_fma.c")
    # Generated chunks include cpu.h and portable SIMD. Follow every quoted
    # include recursively, including conditional branches, instead of treating
    # unrelated launcher/save/renderer headers as guest ABI dependencies.
    pending, seen = [ROOT / "runtime/cpu.h", ROOT / "runtime/simd.h", ROOT / "runtime/gamelib.h"], set()
    while pending:
        header = pending.pop().resolve()
        if header in seen:
            continue
        if not header.is_relative_to(ROOT / "runtime") or not ordinary(header):
            raise RuntimeError("Invalid guest header dependency: " + str(header))
        seen.add(header)
        for name in re.findall(r'^\s*#\s*include\s*"([^"]+)"', header.read_text(encoding="utf-8"), re.MULTILINE):
            pending.append(header.parent / name)
    files += list(seen)
    return {p.relative_to(ROOT).as_posix(): digest(p) for p in sorted(files)}


def compatible_generator_closure(old, current):
    # Legacy development receipts conservatively included every runtime header.
    # Migration may drop only unrelated header entries; every actual recursive
    # guest include and every generator/profile/patch/HLE file must still match.
    return (isinstance(old, dict) and all(old.get(n) == sha for n, sha in current.items()) and
            all(n in current or n.startswith("runtime/") and n.endswith(".h") for n in old))


def pe_imports(path):
    """Read ordinary PE imports without executing a DLL to check its existence."""
    data = path.read_bytes()
    if data[:2] != b"MZ":
        raise ValueError(f"Not PE: {path}")
    pe = struct.unpack_from("<I", data, 0x3c)[0]
    if data[pe:pe+4] != b"PE\0\0":
        raise ValueError(f"Invalid PE signature: {path}")
    machine, count = struct.unpack_from("<HH", data, pe+4)
    if machine != 0x8664:
        raise ValueError(f"Not x86-64 PE: {path}")
    opt_size = struct.unpack_from("<H", data, pe+20)[0]
    opt = pe + 24
    if struct.unpack_from("<H", data, opt)[0] != 0x20b:
        raise ValueError(f"Not PE32+: {path}")
    sections = []
    for i in range(count):
        s = opt + opt_size + i * 40
        virtual_size, address, size, offset = struct.unpack_from("<IIII", data, s+8)
        sections.append((address, max(virtual_size, size), offset, size))
    def off(rva):
        for address, size, offset, rawsize in sections:
            if address <= rva < address + size and rva - address < rawsize:
                return offset + rva - address
        raise ValueError(f"Invalid PE RVA in {path}")
    rva, size = struct.unpack_from("<II", data, opt+112+8)
    if not rva:
        return []
    base, result = off(rva), []
    for i in range(min(size // 20 + 1, 4096)):
        entry = struct.unpack_from("<IIIII", data, base+i*20)
        if not any(entry):
            break
        start = off(entry[3])
        end = data.find(b"\0", start, start+512)
        if end == -1:
            raise ValueError(f"Unbounded PE import in {path}")
        name = data[start:end].decode("ascii")
        if Path(name).name != name or "/" in name or "\\" in name or ":" in name:
            raise ValueError("Unsafe PE import name")
        result.append(name)
    return result


def dll_closure(paths, prefix, flat=False, own_images=None):
    available = {p.name.lower(): p for p in (prefix if flat else prefix / "bin").glob("*.dll")}
    images = {p.name.lower(): p for p in (own_images or [])}
    system = Path(os.environ.get("SystemRoot", "C:/Windows")) / "System32"
    found, pending = {}, list(paths)
    while pending:
        source = pending.pop()
        for name in pe_imports(source):
            key = name.lower()
            if key in found:
                continue
            if key in images:
                if not ordinary(images[key]):
                    raise RuntimeError("Missing/nonordinary owned PE host image: " + name)
                continue
            if key in available:
                file = available[key]
                if not ordinary(file):
                    raise RuntimeError(f"Nonordinary runtime DLL: {file}")
                found[key] = file
                pending.append(file)
            elif name.lower().startswith(("api-ms-win-", "ext-ms-win-")) or (system / name).is_file():
                continue
            else:
                raise RuntimeError(f"Missing dependency {name} imported by {source}")
    return sorted(found.values())


class OwnedProcesses:
    """A native kill-on-close job contains only this build's compiler descendants."""
    def __init__(self):
        self.job = None
        if os.name != "nt":
            return
        from ctypes import wintypes as w
        class BASIC(ctypes.Structure):
            _fields_ = [("ProcessTime", ctypes.c_int64), ("JobTime", ctypes.c_int64), ("LimitFlags", w.DWORD),
                        ("Min", ctypes.c_size_t), ("Max", ctypes.c_size_t), ("Count", w.DWORD),
                        ("Affinity", ctypes.c_size_t), ("Priority", w.DWORD), ("Scheduling", w.DWORD)]
        class IO(ctypes.Structure):
            _fields_ = [(n, ctypes.c_uint64) for n in ("r", "w", "o", "rb", "wb", "ob")]
        class LIMIT(ctypes.Structure):
            _fields_ = [("Basic", BASIC), ("Io", IO), ("ProcMem", ctypes.c_size_t), ("JobMem", ctypes.c_size_t),
                        ("PeakProc", ctypes.c_size_t), ("PeakJob", ctypes.c_size_t)]
        self.k32 = ctypes.WinDLL("kernel32", use_last_error=True)
        self.k32.CreateJobObjectW.restype = w.HANDLE
        self.k32.CreateJobObjectW.argtypes = [ctypes.c_void_p, w.LPCWSTR]
        self.k32.SetInformationJobObject.argtypes = [w.HANDLE, ctypes.c_int, ctypes.c_void_p, w.DWORD]
        self.k32.AssignProcessToJobObject.argtypes = [w.HANDLE, w.HANDLE]
        self.k32.CloseHandle.argtypes = [w.HANDLE]
        self.job = self.k32.CreateJobObjectW(None, None)
        limits = LIMIT()
        limits.Basic.LimitFlags = 0x2000  # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
        if not self.job or not self.k32.SetInformationJobObject(self.job, 9, ctypes.byref(limits), ctypes.sizeof(limits)):
            raise ctypes.WinError(ctypes.get_last_error())

    def assign(self, process):
        if self.job and not self.k32.AssignProcessToJobObject(self.job, int(process._handle)):
            process.terminate()
            process.wait()
            raise ctypes.WinError(ctypes.get_last_error())

    def close(self):
        if self.job:
            self.k32.CloseHandle(self.job)
            self.job = None


class Builder:
    def __init__(self, args):
        self.args = args
        self.output = args.output.absolute()
        if self.output.exists():
            raise RuntimeError(f"Output must be new; existing build preserved: {self.output}")
        if not self.output.is_relative_to(ROOT / "build"):
            raise RuntimeError("Build output must be below Monado_Windows/build")
        if self.output.resolve() != self.output:
            raise RuntimeError("Build output/ancestors must be ordinary, canonical Windows paths")
        baseline, baseline_sha256 = read_windows_baseline(ROOT / "windows-baseline.json")
        self.output.mkdir(parents=True)
        (self.output / "logs").mkdir()
        self.receipt = {"schema": 1, "implementation": "windows-parity-development", "status": "started",
                        "python": sys.version, "host": platform.platform(), "flags": FLAGS, "commands": [], "sources": {}}
        self.guard = OwnedProcesses()
        self.lock = threading.Lock()
        self.prefix = args.prefix
        if self.prefix is None:
            config = ROOT / ".deps/windows-deps.json"
            self.prefix = Path(json.loads(config.read_text(encoding="utf-8"))["prefix"]) if config.exists() else Path("C:/msys64/ucrt64")
        self.prefix = self.prefix.resolve()
        self.cc = [sys.executable, "-m", "ziglang", "cc"]
        self.cxx = [sys.executable, "-m", "ziglang", "c++"]
        self.gamelib_id = "windows-dev"
        self.includes = ["-I" + str(ROOT / p) for p in ("runtime", "runtime/nvn", "runtime/nvn/vulkan", "third_party", "third_party/imgui", "third_party/imgui/backends", "third_party/streamline/include")]
        self.includes += ["-I" + str(self.prefix / "include"), "-I" + str(self.output / "inc"), "-I" + str(ROOT / "third_party/smaa/Textures")]
        self.receipt["prefix"] = str(self.prefix)
        self.receipt["zig"] = importlib.metadata.version("ziglang")
        self.receipt.update(git_identity())
        self.receipt["baseline"] = baseline
        self.receipt["sources"]["windows-baseline.json"] = baseline_sha256
        self.receipt["native_build_identity"] = (self.receipt["git_commit"] or "source kit") + " / " + self.output.name
        if self.receipt["zig"] != "0.14.1":
            raise RuntimeError("This Windows recipe requires pinned ziglang==0.14.1")

    def run(self, name, command, timeout=900):
        logname = re.sub(r"[^a-zA-Z0-9_.-]", "_", name)
        out, err = (self.output / "logs" / (logname + s) for s in (".out", ".err"))
        record = {"name": name, "argv": list(map(str, command)), "stdout": str(out), "stderr": str(err)}
        start = time.monotonic()
        with out.open("wb") as stdout, err.open("wb") as stderr:
            env = os.environ.copy()
            env["PYTHONDONTWRITEBYTECODE"] = "1"
            env["ZIG_GLOBAL_CACHE_DIR"] = str(ROOT / ".deps/zig-cache")
            startup = process_options(record["argv"], ROOT, cwd_independent=True)
            record["cwd"] = startup["cwd"]
            record["executable"] = startup.get("executable", record["argv"][0])
            process = subprocess.Popen(record["argv"], **startup, stdout=stdout, stderr=stderr, env=env)
            self.guard.assign(process)
            try:
                record["exit"] = process.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                process.terminate()
                process.wait()
                record["exit"] = -1
                record["timeout"] = timeout
        record["seconds"] = round(time.monotonic() - start, 3)
        with self.lock:
            self.receipt["commands"].append(record)
            atomic_json(self.output / "build-receipt.json", self.receipt)
        if record["exit"]:
            message = err.read_text(encoding="utf-8", errors="replace")[-10000:]
            raise RuntimeError(f"{name} failed with exit {record['exit']}\n{message}\nLogs: {self.output / 'logs'}")
        return out

    def preflight(self):
        libraries = []
        for name in LIBRARIES:
            library = self.prefix / "lib" / ("lib" + name + ".dll.a")
            if name == "z" and not library.is_file():
                library = self.prefix / "lib/libz.a"
            if not ordinary(library):
                raise RuntimeError(f"Missing native import library: {library}; run tools/windows_deps.py install")
            libraries.append(str(library))
        self.libs = libraries
        self.receipt["import_libraries"] = {str(p): digest(Path(p)) for p in libraries}
        probe = self.output / "preflight.c"
        probe.write_text("#include <SDL3/SDL.h>\n#include <libavcodec/avcodec.h>\n#include <shaderc/shaderc.h>\n#include <vulkan/vulkan.h>\n#ifdef NDEBUG\n#error assertions disabled\n#endif\n#ifdef __FAST_MATH__\n#error fast math enabled\n#endif\nint main(void) { return (int)(SDL_GetVersion()+avcodec_version()+vkEnumerateInstanceVersion(0)); }\n", encoding="utf-8")
        exe = self.output / "dependency-probe.exe"
        self.run("dependency-preflight", self.cc + FLAGS + self.includes + [str(probe), "-o", str(exe)] + libraries)
        closure = dll_closure([exe] + [self.prefix / "bin" / p for p in ("SDL3.dll", "libshaderc_shared.dll") if (self.prefix / "bin" / p).exists()], self.prefix)
        self.receipt["preflight_dlls"] = {p.name: digest(p) for p in closure}
        self.receipt["dependency_receipt_sha256"] = digest(self.prefix / "receipt.json") if (self.prefix / "receipt.json").exists() else None
        print(f"Native dependency preflight passed; {len(closure)} runtime DLLs checked", flush=True)

    def compile(self, sources, tag=""):
        def authored_headers():
            return sorted([p for p in (ROOT / "runtime").rglob("*") if p.suffix in (".h", ".inc")] +
                          list((ROOT / "tools/launcher").glob("*.h")) + list((ROOT / "tools/builder").glob("*.h")))
        header_paths = authored_headers()
        headers = {str(p): digest(p) for p in header_paths}
        def one(source):
            relative = source.relative_to(ROOT).as_posix() if source.is_relative_to(ROOT) else str(source)
            if not ordinary(source):
                raise RuntimeError(f"Missing/nonordinary source: {source}")
            before = digest(source)
            obj = self.output / "obj" / (hashlib.sha256(relative.encode()).hexdigest()[:12] + "_" + source.stem + tag + ".o")
            obj.parent.mkdir(exist_ok=True)
            cmd = (self.cxx + ["-std=c++17"] if source.suffix == ".cpp" else self.cc + ["-std=gnu11"])
            cmd += FLAGS + self.includes + ['-DGAMELIB_ID="' + self.gamelib_id + '"', "-DXCDE_GFX"]
            if source.is_relative_to(ROOT / "runtime") and self.args.stage != "boot":
                cmd.append("-DXCDE_WINDOWS_FRAMEGEN=1")
            if self.args.stage == "boot" and source == ROOT / "runtime/host/main_boot.c":
                cmd.remove("-DXCDE_GFX")
            if source == ROOT / "tools/launcher/launcher.cpp":
                cmd += ["-I" + str(self.output / "inc/launcher-theme"), "-DMONADO_LAUNCHER_RESOURCES", "-DMONADO_LAUNCHER_ARTWORK"]
                cmd += ["-DMONADO_WINDOWS_BASELINE_LABEL=" + json.dumps(windows_baseline_label(self.receipt["baseline"])),
                        "-DMONADO_WINDOWS_BUILD_IDENTITY=" + json.dumps(self.receipt["native_build_identity"])]
                cmd += ["-DMONADO_FSR4_CONTEXT_SHA256=" + json.dumps(self.receipt.get("preparation_context_sha256", ""))]
                selected = self.receipt.get("providers", {}).get("selection", {})
                provider_status = "Local optional SDKs staged: " + ", ".join(sorted(selected)) + ". Runtime checks GPU/provider activation." if selected else "No local DLSS/FSR3 SDKs staged. Optional choices use the available fallback."
                cmd += ["-DMONADO_WINDOWS_PROVIDER_STATUS=" + json.dumps(provider_status)]
            self.run("compile-" + relative, cmd + ["-c", str(source), "-o", str(obj)])
            if digest(source) != before:
                raise RuntimeError(f"Source changed during compile; retry in a new output: {source}")
            with self.lock:
                self.receipt["sources"][relative] = before
            print("Compiled " + relative, flush=True)
            return obj
        results, errors = [], []
        with concurrent.futures.ThreadPoolExecutor(max_workers=self.args.jobs) as pool:
            futures = [pool.submit(one, source) for source in sources]
            for future in concurrent.futures.as_completed(futures):
                try:
                    results.append(future.result())
                except Exception as error:
                    errors.append(error)
                    if not self.args.keep_going:
                        for pending in futures:
                            pending.cancel()
                        break
        if errors:
            raise RuntimeError("\n".join(map(str, errors)))
        if {str(p): digest(p) for p in authored_headers()} != headers:
            raise RuntimeError("Runtime headers changed during compilation; retry in a fresh build output")
        self.receipt["sources"].update({Path(p).relative_to(ROOT).as_posix(): sha for p, sha in headers.items()})
        return sorted(results)

    def link(self, name, objects, launcher=False, extra=None):
        exe = self.output / "build" / name
        exe.parent.mkdir(exist_ok=True)
        resource = self.output / "windows_utf8.res"
        if not resource.exists():
            self.run("compile-native-manifest", self.cc[:-1] + ["rc", "/I", str(ROOT / "tools"), "/fo", str(resource), str(ROOT / "tools/windows_utf8.rc")])
            self.receipt["sources"]["tools/windows_utf8.rc"] = digest(ROOT / "tools/windows_utf8.rc")
            self.receipt["sources"]["tools/windows_utf8.manifest"] = digest(ROOT / "tools/windows_utf8.manifest")
        response = self.output / (name + ".rsp")
        args = [str(p) for p in objects] + [str(resource), "-o", str(exe), "-march=x86_64_v2"]
        args += [self.libs[0]] if launcher else self.libs + ["-lwinmm", "-ldbghelp", "-lbcrypt", "-ladvapi32", "-Wl,--stack,268435456"]
        if launcher:
            args += ["-Wl,--subsystem,windows", "-ladvapi32", "-lbcrypt", "-lole32", "-lshell32", "-luuid"]
        args += extra or []
        response.write_text("\n".join('"' + a.replace("\\", "/").replace('"', '\\"') + '"' for a in args), encoding="utf-8")
        self.run("link-" + name, self.cxx + ["@" + str(response)], timeout=1800)
        for dll in dll_closure([exe], self.prefix):
            dest = exe.parent / dll.name
            if dest.exists() and digest(dest) != digest(dll):
                raise RuntimeError("Conflicting runtime DLL: " + dll.name)
            shutil.copy2(dll, dest)
        return exe

    def game_identity(self, imports=None):
        entry = ROOT / "runtime/host/gamelib_entry_windows.c"
        if not ordinary(entry):
            raise RuntimeError("Missing authored Windows GameLib entry")
        compiler = Path(importlib.util.find_spec("ziglang").submodule_search_locations[0]) / "zig.exe"
        abi = re.search(r"^#define GAMELIB_ABI\s+(\d+)\s*$", (ROOT / "runtime/gamelib.h").read_text(encoding="utf-8"), re.MULTILINE)
        if not abi:
            raise RuntimeError("GameLib ABI declaration is not explicit")
        identity = {"schema": 1, "format": "windows-amd64-pe", "abi": int(abi.group(1)),
                    "compiler": {"version": self.receipt["zig"], "sha256": digest(compiler)},
                    "flags": FLAGS, "input": self.receipt["input"]["exefs"],
                    "generator_closure": generator_closure(), "entry_sha256": digest(entry),
                    "native_game_helpers": {"runtime/platform/windows_fma.c": digest(ROOT / "runtime/platform/windows_fma.c")}}
        if imports is not None:
            identity["host_imports"] = imports
        return identity


    def split_link(self, host_objects, game_objects):
        spec = importlib.util.spec_from_file_location("monado_coff", ROOT / "tools/coff_symbols.py")
        coff = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(coff)
        dll_helpers = [o for o in host_objects if o.name.endswith("_windows_fma.o")]
        if len(dll_helpers) != 1:
            raise RuntimeError("Both native images require one explicit baseline Windows fma/fmaf helper")
        candidate, reuse = None, None
        if self.args.reuse_game_library:
            reuse = importlib.util.spec_from_file_location("monado_game_reuse", ROOT / "tools/game_library_reuse_windows.py")
            helper = importlib.util.module_from_spec(reuse)
            sys.modules[reuse.name] = helper
            reuse.loader.exec_module(helper)
            host_definitions = {}
            for object_path in host_objects:
                host_definitions.update(coff.symbols(object_path)[0])
            candidate = helper.inspect(self.args.reuse_game_library, self.game_identity(), host_definitions)
            imports, identity = candidate.imports, candidate.identity
            self.receipt["reused_game_library"] = {"previous": str(candidate.previous), "receipt_sha256": candidate.receipt_sha256,
                                                   "id": candidate.identifier, "dll_sha256": hashlib.sha256(candidate.files["lasgame.dll"]).hexdigest()}
            self.receipt["generator_closure"] = identity["generator_closure"]
            for file in (ROOT / "tools/game_library_reuse_windows.py", ROOT / "tools/pe_symbols_windows.py", ROOT.parent / "tools/windows_file_guard.py"):
                self.receipt["sources"][os.path.relpath(file, ROOT).replace("\\", "/")] = digest(file)
        else:
            imports = coff.host_imports(game_objects + dll_helpers, host_objects)
            identity = self.game_identity(imports)
        if not imports or any(not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", n) for n in imports):
            raise RuntimeError("Unrepresentable/empty native authored host export list")
        entry = ROOT / "runtime/host/gamelib_entry_windows.c"
        self.receipt["sources"].update(identity["generator_closure"])
        self.receipt["sources"]["runtime/host/gamelib_entry_windows.c"] = identity["entry_sha256"]
        self.gamelib_id = hashlib.sha256(json.dumps(identity, sort_keys=True, separators=(",", ":")).encode("utf-8")).hexdigest()
        definition = self.output / "host-game-imports.def"
        definition.write_text("LIBRARY xcde.exe\nEXPORTS\n" + "".join("  " + n + (" DATA" if k == "data" else "") + "\n" for n, k in imports.items()), encoding="ascii", newline="\n")
        # The first pass inventories actual authored definitions. Only this TU
        # consumes GAMELIB_ID; recompile it under the final canonical identity.
        original_loader = next(o for o in host_objects if o.name.endswith("_gamelib.o"))
        final_loader = self.compile([ROOT / "runtime/gamelib.c"], tag="_" + self.gamelib_id[:12])[0]
        host_objects = [o for o in host_objects if o != original_loader] + [final_loader]
        implib = self.output / "xcde-host.dll.a"
        host = self.link("xcde.exe", host_objects, extra=[str(definition), "-Wl,--out-implib," + str(implib)])
        dll = host.parent / "lasgame.dll"
        if candidate:
            helper.copy(candidate, host.parent)
        else:
            glue = self.compile([entry], tag="_" + self.gamelib_id[:12])
            arguments = [str(p) for p in game_objects + glue + dll_helpers] + [str(implib), "-shared", "-o", str(dll), "-march=x86_64_v2", "-Wl,--out-implib," + str(host.parent / "lasgame-local.lib")]
            response = self.output / "lasgame.dll.rsp"
            response.write_text("\n".join('"' + a.replace("\\", "/").replace('"', '\\"') + '"' for a in arguments), encoding="utf-8", newline="\n")
            self.run("link-local-gamecode-dll", self.cxx + ["@" + str(response)], timeout=1800)
        for dependency in dll_closure([dll], self.prefix, own_images=[host]):
            dest = host.parent / dependency.name
            if dest.exists() and digest(dest) != digest(dependency):
                raise RuntimeError("GameLib dependency conflicts with the authored runtime")
            shutil.copy2(dependency, dest)
        if not candidate:
            for suffix, value in ((".id", self.gamelib_id), (".sha256", digest(dll))):
                with Path(str(dll) + suffix).open("x", encoding="ascii", newline="\n") as stream:
                    stream.write(value + "\n")
                    stream.flush()
                    os.fsync(stream.fileno())
        self.receipt["split_game"] = {"id": self.gamelib_id, "identity": identity,
                                      "dll_sha256": digest(dll), "host_import_library_sha256": digest(implib),
                                      "sidecars": {suffix: digest(Path(str(dll) + suffix)) for suffix in (".id", ".sha256")}}
        self.receipt["sources"]["tools/coff_symbols.py"] = digest(ROOT / "tools/coff_symbols.py")
        atomic_json(self.output / "gamecode-identity.json", self.receipt["split_game"])
        print(f"Split authored host and local GameLib linked; {len(imports)} explicit host imports", flush=True)

    def validate_input(self):
        if self.args.input is None:
            selected = ROOT / ".deps/selected-input.json"
            self.args.input = Path(json.loads(selected.read_text(encoding="utf-8"))["path"]) if ordinary(selected) else ROOT.parent / "game_files"
        self.args.input = self.args.input.resolve()
        if self.args.input.is_relative_to(self.output) or self.output.is_relative_to(self.args.input):
            raise RuntimeError("Input and build output must not overlap")
        self.run("check-input", [sys.executable, "-B", str(ROOT / "tools/extract/check_dump.py"), str(self.args.input)], timeout=600)
        identities = {}
        for name in ("main", "sdk"):
            file = self.args.input / "exefs" / name
            identities[name] = {"sha256": digest(file), "bytes": file.stat().st_size}
        expected = {"main": "76ffa21b99c0385cfb14d1f90bbeddcdc98c17350b8edd79dfdbbb7341e70c48",
                    "sdk": "31ce67e8701584ba812dceeae9b590d7a6ad642bcb5aa74d2fe3ea5c5474250d"}
        if any(identities[n]["sha256"] != expected[n] for n in expected):
            raise RuntimeError("Selected DE2.0.0 main/SDK hashes differ from the accepted input")
        self.receipt["input"] = {"path": str(self.args.input), "exefs": identities}

    def installation(self):
        if os.name != "nt":
            raise RuntimeError("Native Windows installation requires Win32")
        import _winapi
        target = self.output / "targets"
        target.mkdir()
        for name in ("exefs", "romfs"):
            _winapi.CreateJunction(str(self.args.input / name), str(target / name))
        for name in ("LICENSE", "NOTICE.md"):
            shutil.copy2(ROOT / name, self.output / name)
        self.receipt["installation"] = {"input_links": {n: str(self.args.input / n) for n in ("exefs", "romfs")},
                                         "state_root": str(self.output), "new_profile": True}

    def reuse_game_objects(self, previous):
        """Reuse only an explicitly selected local completed compile manifest.

        A failed later runtime link does not invalidate successful game compiles;
        every input/header/source/flag and successful command is checked first.
        This is a private static-object optimization, never GameLib ABI reuse.
        """
        previous = previous.absolute()
        if previous.resolve() != previous or not previous.is_relative_to(ROOT / "build"):
            raise RuntimeError("Game object reuse requires an ordinary owned build root")
        receipt_path = previous / "build-receipt.json"
        if not ordinary(receipt_path):
            raise RuntimeError("Previous build has no ordinary compile receipt")
        record = json.loads(receipt_path.read_text(encoding="utf-8"))
        identity = generator_closure()
        if (record.get("flags") != FLAGS or record.get("zig") != self.receipt["zig"] or
                record.get("input", {}).get("exefs") != self.receipt["input"]["exefs"] or
                not compatible_generator_closure(record.get("generator_closure"), identity)):
            raise RuntimeError("Previous game object input/compiler/flags/header closure differs")
        stamp_path = previous / "generated/generated-receipt.json"
        if not ordinary(stamp_path) or digest(stamp_path) != record.get("generated_receipt_sha256"):
            raise RuntimeError("Previous generated identity receipt changed")
        stamp = json.loads(stamp_path.read_text(encoding="utf-8"))
        for name, sha in stamp["sources"].items():
            source = previous / "generated" / name
            if not ordinary(source) or digest(source) != sha or record["sources"].get(source.relative_to(ROOT).as_posix()) != sha:
                raise RuntimeError("Previous generated source changed: " + name)
        objects = []
        adopted = {}
        values = record.get("generated_objects", [])
        if not values:
            # An unrelated runtime header edit can fail a whole-stage guard
            # after every guest compile succeeded. Recover only the exact
            # successful command manifest, never discover objects with globs.
            by_source = {}
            for command in record["commands"]:
                argv = command.get("argv", [])
                if command.get("exit") != 0 or len(argv) < 4 or argv[-4] != "-c" or argv[-2] != "-o":
                    continue
                c_source = Path(argv[-3])
                if c_source.is_relative_to(previous / "generated"):
                    name = c_source.relative_to(previous / "generated").as_posix()
                    if name in by_source:
                        raise RuntimeError("Duplicate successful guest compile in previous manifest")
                    by_source[name] = argv[-1]
            if set(by_source) != set(stamp["sources"]):
                raise RuntimeError("Previous guest compile manifest is incomplete")
            values = [by_source[n] for n in sorted(by_source)]
        for value in values:
            source = Path(value)
            if not source.is_relative_to(previous / "obj") or source.resolve() != source or not ordinary(source):
                raise RuntimeError("Previous game object ownership mismatch")
            commands = [c for c in record["commands"] if c.get("exit") == 0 and "-c" in c["argv"] and c["argv"][-1] == str(source)]
            if len(commands) != 1:
                raise RuntimeError("Previous game object lacks one successful compile command")
            c_source = Path(commands[0]["argv"][-3])
            if not c_source.is_relative_to(previous / "generated") or c_source.relative_to(previous / "generated").as_posix() not in stamp["sources"]:
                raise RuntimeError("Previous object compiled another source")
            target = self.output / "obj/reused-game" / source.name
            target.parent.mkdir(parents=True, exist_ok=True)
            sha = digest(source)
            shutil.copy2(source, target)
            if digest(target) != sha:
                raise RuntimeError("Copied game object changed")
            objects.append(target)
            adopted[str(target)] = {"sha256": sha, "from": str(source)}
        if len(objects) != len(stamp["sources"]) or not objects:
            raise RuntimeError("Previous game object manifest is incomplete")
        self.receipt["reused_game_objects"] = {"receipt": str(receipt_path), "receipt_sha256": digest(receipt_path), "objects": adopted}
        self.receipt["generator_closure"] = identity
        print(f"Reused {len(objects)} checked local static game objects", flush=True)
        return objects

    def output_inventory(self):
        # Exact linked images and their checked native closure, never ambient
        # *.dll/*.exe globs which could bless a human's unrelated extra file.
        names = []
        if self.args.stage in ("all", "boot"):
            names.append("xcde_boot.exe" if self.args.stage == "boot" else "xcde.exe")
        if self.args.stage in ("all", "launcher"):
            names.append("xcde_launcher.exe")
        if self.args.split_game and self.args.stage == "all":
            names.append("lasgame.dll")
        images = [self.output / "build" / name for name in names]
        if any(not ordinary(p) for p in images):
            raise RuntimeError("A recipe-linked output image is missing or nonordinary")
        own = [p for p in images if p.name == "xcde.exe"]
        paths = set(images)
        for dependency in dll_closure(images, self.prefix, own_images=own):
            path = self.output / "build" / dependency.name
            if not ordinary(path) or digest(path) != digest(dependency):
                raise RuntimeError("Copied native dependency differs from selected prefix: " + dependency.name)
            paths.add(path)
        if "lasgame.dll" in names:
            paths.update(self.output / "build" / ("lasgame.dll" + suffix) for suffix in (".id", ".sha256"))
        if self.receipt.get("preparation_context_sha256"):
            paths.add(self.output / "fsr4-preparation-context.json")
        for relative, expected in self.receipt.get("providers", {}).get("files", {}).items():
            name = Path(relative)
            if name.is_absolute() or ".." in name.parts or name.as_posix() != relative:
                raise RuntimeError("Provider receipt has an unsafe output path")
            path = self.output / name
            if not ordinary(path) or path.stat().st_size != expected["bytes"] or digest(path) != expected["sha256"]:
                raise RuntimeError("Staged local provider file changed or is missing: " + relative)
            paths.add(path)
        return {p.relative_to(self.output).as_posix(): digest(p) for p in sorted(paths)}

    def stage_providers(self):
        fidelityfx, streamline = getattr(self.args, "fidelityfx", None), getattr(self.args, "streamline", None)
        if fidelityfx is None and streamline is None:
            return
        dependencies = (ROOT / "tools/windows_provider_selection.py", ROOT / "tools/pe_symbols_windows.py",
                        ROOT.parent / "tools/windows_file_guard.py")
        identities = {path: digest(path) for path in dependencies}
        spec = importlib.util.spec_from_file_location("monado_build_providers", dependencies[0])
        helper = importlib.util.module_from_spec(spec)
        sys.modules[spec.name] = helper
        spec.loader.exec_module(helper)
        self.receipt["providers"] = helper.stage(self.output, fidelityfx=fidelityfx, streamline=streamline,
            progress=lambda phase, done, total, message: print(f"{message} ({done}/{total})", flush=True))
        for path, identity in identities.items():
            if not ordinary(path) or digest(path) != identity:
                raise RuntimeError("Local provider verifier changed during staging")
            self.receipt["sources"][os.path.relpath(path, ROOT).replace("\\", "/")] = identity

    def retention_inventory(self):
        # The finalized build-receipt.json is deliberately excluded: its own
        # embedded digest would recurse. The checked supervisor adds that one
        # finalized file identity before authorizing an obsolete transaction.
        paths = {self.output / name for name in self.receipt["outputs"]}
        for name in ("preflight.c", "dependency-probe.exe", "dependency-probe.pdb", "windows_utf8.res",
                     "xcde.exe.rsp", "xcde_boot.exe.rsp", "xcde_launcher.exe.rsp", "lasgame.dll.rsp",
                     "host-game-imports.def", "xcde-host.dll.a", "gamecode-identity.json", "LICENSE", "NOTICE.md",
                     "inc/smaa_hlsl.inc", "build/xcde.pdb", "build/xcde_boot.pdb", "build/xcde_launcher.pdb",
                     "build/lasgame.pdb", "build/lasgame-local.lib"):
            path = self.output / name
            if path.exists():
                paths.add(path)
        for command in self.receipt["commands"]:
            if command.get("exit") != 0:
                raise RuntimeError("A failed command cannot acquire cleanup authority")
            paths.update(Path(command[key]) for key in ("stdout", "stderr"))
            argv = command["argv"]
            if len(argv) >= 4 and argv[-4] == "-c" and argv[-2] == "-o":
                paths.add(Path(argv[-1]))
            if command["name"] == "launcher-resources":
                manifest = json.loads(Path(command["stdout"]).read_text(encoding="utf-8"))
                allowed = {"launcher_fonts.inc", "launcher_art.inc", "DejaVu-font-license.txt"}
                if not set(manifest["generated"]).issubset(allowed):
                    raise RuntimeError("Unexpected launcher resource artifact")
                for name, identity in manifest["generated"].items():
                    path = self.output / "inc/launcher-theme" / name
                    if path.stat().st_size != identity["bytes"] or digest(path) != identity["sha256"]:
                        raise RuntimeError("Generated launcher resource changed")
                    paths.add(path)
        for value in self.receipt.get("reused_game_objects", {}).get("objects", {}):
            paths.add(Path(value))
        stamp = self.output / "generated/generated-receipt.json"
        if stamp.exists():
            if digest(stamp) != self.receipt.get("generated_receipt_sha256"):
                raise RuntimeError("Generated cleanup receipt changed")
            paths.add(stamp)
            generated = json.loads(stamp.read_text(encoding="utf-8"))
            for name, sha in generated["sources"].items():
                if not re.fullmatch(r"(main|sdk)/(main|sdk)_(?:[0-9]{3}|table)\.c", name) or name.split("/")[0] != name.split("/")[1].split("_")[0]:
                    raise RuntimeError("Unknown generated recipe output")
                path = stamp.parent / name
                if digest(path) != sha:
                    raise RuntimeError("Generated cleanup source changed")
                paths.add(path)
            for name in ("main.log", "sdk.log"):
                path = stamp.parent / name
                if path.exists():
                    paths.add(path)
        inventory = {}
        for path in sorted(paths):
            if path.absolute() != path or not path.is_relative_to(self.output) or path.resolve() != path or not ordinary(path) or path.stat().st_nlink != 1:
                raise RuntimeError("Nonordinary or escaping recipe cleanup artifact: " + str(path))
            inventory[path.relative_to(self.output).as_posix()] = {"bytes": path.stat().st_size, "sha256": digest(path)}
        return inventory

    def build(self):
        self.preflight()
        if self.args.stage == "preflight":
            return
        stage = self.args.stage
        if stage in ("generated", "boot", "all"):
            self.validate_input()
        if stage in ("runtime", "boot", "all"):
            (self.output / "inc").mkdir()
            self.run("smaa-source", [sys.executable, "-B", str(ROOT / "tools/gen_smaa_inc.py"), str(ROOT / "third_party/smaa/SMAA.hlsl"), str(self.output / "inc/smaa_hlsl.inc")])
            units = [n if n != "host/gamelib_static" or not self.args.split_game else "gamelib" for n in RUNTIME]
            sources = [ROOT / ("runtime/" + n + ".c") for n in units]
            sources += [ROOT / ("runtime/nvn/" + n + ".c") for n in NVN] + [ROOT / n for n in CPP]
            objects = self.compile(sources)
            self.receipt["authored_objects"] = [str(o) for o in objects]
        if stage in ("generated", "boot", "all"):
            if self.args.reuse_game_library:
                game_objects = []  # compatible checked local DLL: no generated C/objects required
            elif self.args.reuse_game_build:
                game_objects = self.reuse_game_objects(self.args.reuse_game_build)
            else:
                generated = self.args.generated
                identity = generator_closure()
                if generated is None:
                    generated = self.output / "generated"
                    self.run("generate-game", [sys.executable, "-B", str(ROOT / "tools/gen_all.py"), "--exefs", str(self.args.input / "exefs"), "--out", str(generated)], timeout=1800)
                    if generator_closure() != identity:
                        raise RuntimeError("Generator/guest-header source changed during generation; retry in a fresh build")
                    stamp = {"schema": 1, "input": self.receipt["input"]["exefs"], "generator_closure": identity,
                             "sources": {p.relative_to(generated).as_posix(): digest(p) for p in sorted(generated.rglob("*.c"))}}
                    atomic_json(generated / "generated-receipt.json", stamp)
                else:
                    generated = generated.absolute()
                    if generated.resolve() != generated or not any(generated.is_relative_to(ROOT / n) for n in ("build", "generated", "runs")):
                        raise RuntimeError("Generated reuse requires an ordinary owned Windows output root")
                    stamp_path = generated / "generated-receipt.json"
                    if not ordinary(stamp_path):
                        raise RuntimeError("Generated sources have no owned identity receipt; use the full source build to regenerate")
                    stamp = json.loads(stamp_path.read_text(encoding="utf-8"))
                    if stamp.get("input") != self.receipt["input"]["exefs"] or not compatible_generator_closure(stamp.get("generator_closure"), identity):
                        raise RuntimeError("Generated input/generator/header identity changed; regenerate from source")
                    for name, sha in stamp["sources"].items():
                        path = generated / name
                        if not path.resolve().is_relative_to(generated) or not ordinary(path) or digest(path) != sha:
                            raise RuntimeError("Generated source hash/ownership mismatch: " + name)
                self.receipt["generator_closure"] = identity
                self.receipt["generated_receipt_sha256"] = digest(generated / "generated-receipt.json")
                game_sources = []
                for name in ("main", "sdk"):
                    entries = sorted((generated / name).glob("*.c"))
                    if len(entries) < 2 or not (generated / name / (name + "_table.c")).is_file():
                        raise RuntimeError(f"Generated {name} chunks and table missing")
                    game_sources += entries
                game_objects = self.compile(game_sources)
                self.receipt["generated_objects"] = [str(o) for o in game_objects]
            if stage in ("all", "boot"):
                if self.args.split_game:
                    self.split_link(objects, game_objects)
                else:
                    self.link("xcde_boot.exe" if stage == "boot" else "xcde.exe", objects + game_objects)
        self.stage_providers()
        if stage in ("launcher", "all"):
            helper_path = ROOT / "tools/preparation_context_windows.py"
            spec = importlib.util.spec_from_file_location("monado_preparation_context", helper_path)
            context_helper = importlib.util.module_from_spec(spec); spec.loader.exec_module(context_helper)
            context = context_helper.create(ROOT, self.output,
                installation=getattr(self.args, "preparation_installation", None) or self.output,
                manifest=getattr(self.args, "preparation_python_manifest", None),
                manifest_sha256=getattr(self.args, "preparation_python_manifest_sha256", None),
                manager=getattr(self.args, "preparation_manager", None),
                manager_sha256=getattr(self.args, "preparation_manager_sha256", None),
                manager_payload_sha256=getattr(self.args, "preparation_manager_payload_sha256", None),
                manager_manifest_sha256=getattr(self.args, "preparation_manager_manifest_sha256", None))
            context_path = self.output / "fsr4-preparation-context.json"
            atomic_json(context_path, context)
            self.receipt.update(preparation_context=str(context_path), preparation_context_sha256=digest(context_path),
                                preparation_available=context["status"] == "available", preparation_reason=context["reason"])
            self.receipt["sources"]["tools/preparation_context_windows.py"] = digest(helper_path)
            (self.output / "inc").mkdir(exist_ok=True)
            self.run("launcher-resources", [sys.executable, "-B", str(ROOT / "tools/launcher/theme_resource.py"), "--assets", str(ROOT / "tools/launcher/assets"), "--output", str(self.output / "inc/launcher-theme")])
            self.link("xcde_launcher.exe", self.compile([ROOT / n for n in LAUNCHER]), launcher=True)
        for name, sha in self.receipt["sources"].items():
            path = ROOT / name
            if not ordinary(path) or digest(path) != sha:
                raise RuntimeError("Compiled source changed before completion; use a fresh build: " + name)
        if stage == "all":
            self.installation()


def checked_current(launch=False):
    marker_path = ROOT / "build/active-windows-build.json"
    if not ordinary(marker_path):
        raise RuntimeError("No checked native game/launcher installation has been selected")
    marker = json.loads(marker_path.read_text(encoding="utf-8"))
    install = Path(marker["root"])
    if install.resolve() != install or not install.is_relative_to(ROOT / "build"):
        raise RuntimeError("Unsafe active installation root")
    receipt_path = install / "build-receipt.json"
    if not ordinary(receipt_path) or digest(receipt_path) != marker["receipt_sha256"]:
        raise RuntimeError("Active build receipt changed")
    receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
    if receipt.get("status") != "success" or not receipt.get("installation"):
        raise RuntimeError("Active build is not a completed native installation")
    for name, sha in receipt["outputs"].items():
        path = install / name
        if not path.resolve().is_relative_to(install) or not ordinary(path) or digest(path) != sha:
            raise RuntimeError("Active build companion changed/missing: " + name)
    game, launcher = install / "build/xcde.exe", install / "build/xcde_launcher.exe"
    for exe in (game, launcher):
        if exe.relative_to(install).as_posix() not in receipt["outputs"]:
            raise RuntimeError("Active receipt lacks the complete game/launcher pair")
    images = [game, launcher]
    if receipt.get("split_game"):
        images.append(game.parent / "lasgame.dll")
    dll_closure(images, game.parent, flat=True, own_images=[game])
    if launch:
        arguments = [str(launcher), "--profile-root", str(install)]
        subprocess.Popen(arguments, **process_options(arguments, install, cwd_independent=True), close_fds=True)
    print(str(launcher))
    return install


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--stage", choices=("preflight", "runtime", "generated", "boot", "launcher", "all"), default="all")
    ap.add_argument("--input", type=Path)
    ap.add_argument("--output", type=Path, default=ROOT / "build" / ("windows-dev-" + datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S") + f"-{os.getpid()}"))
    ap.add_argument("--generated", type=Path)
    ap.add_argument("--reuse-game-library", type=Path, help="reuse a compatible checked owned local lasgame.dll across source/version roots; never execute while checking")
    ap.add_argument("--reuse-game-build", type=Path, help="verify a selected private static compile receipt and reuse its unchanged game objects")
    ap.add_argument("--split-game", action="store_true", help="link an authored runtime plus locally generated lasgame.dll with strict identity/hash sidecars")
    ap.add_argument("--prefix", type=Path)
    ap.add_argument("--preparation-python-manifest", type=Path)
    ap.add_argument("--preparation-python-manifest-sha256")
    ap.add_argument("--preparation-installation", type=Path)
    ap.add_argument("--preparation-manager", type=Path)
    ap.add_argument("--preparation-manager-sha256")
    ap.add_argument("--preparation-manager-payload-sha256")
    ap.add_argument("--preparation-manager-manifest-sha256")
    ap.add_argument("--fidelityfx", type=Path, help="explicit local FidelityFX SDK/DLL folder with its license; no download")
    ap.add_argument("--streamline", type=Path, help="explicit local Streamline SDK/DLL folder with its licenses; no download")
    ap.add_argument("--jobs", type=int, choices=range(1, 7), default=4)
    ap.add_argument("--keep-going", action="store_true", help="retain all runtime compile diagnostics; never link failed objects")
    ap.add_argument("--launch-current", action="store_true", help="verify active pair/DLLs, then open only its settings launcher")
    ap.add_argument("--check-current", action="store_true", help="verify active pair/DLLs without launching anything")
    args = ap.parse_args(argv)
    if (args.fidelityfx is not None or args.streamline is not None) and args.stage != "all":
        ap.error("Local optional providers require the complete fresh --stage all recipe")
    if args.reuse_game_library is not None:
        # Normalize CLI spelling without resolving any reparse-point ancestry.
        args.reuse_game_library = args.reuse_game_library.absolute()
    if args.split_game and args.stage != "all":
        ap.error("--split-game currently requires the complete graphics --stage all; static/headless fallback remains available")
    if args.reuse_game_library and (not args.split_game or args.generated or args.reuse_game_build):
        ap.error("--reuse-game-library requires --split-game and cannot be combined with generated/object reuse")
    builder = None
    try:
        if args.launch_current or args.check_current:
            checked_current(launch=args.launch_current)
            return 0
        builder = Builder(args)
        builder.build()
        builder.receipt["status"] = "success"
        builder.receipt["outputs"] = builder.output_inventory()
        builder.receipt["retention_files"] = builder.retention_inventory()
        atomic_json(builder.output / "build-receipt.json", builder.receipt)
        if args.stage == "all":
            atomic_json(ROOT / "build/active-windows-build.json", {"root": str(builder.output), "receipt_sha256": digest(builder.output / "build-receipt.json")})
        print("Build stage complete: " + str(builder.output))
        return 0
    except (Exception, KeyboardInterrupt) as error:
        if builder:
            builder.receipt.update(status="failed", error=str(error))
            if getattr(error, "failure_kind", None) == "incompatible-game-library":
                builder.receipt["failure_kind"] = "incompatible-game-library"
        print(str(error), file=sys.stderr)
        return 3 if getattr(error, "failure_kind", None) == "incompatible-game-library" else 1
    finally:
        if builder:
            builder.guard.close()
            atomic_json(builder.output / "build-receipt.json", builder.receipt)


if __name__ == "__main__":
    sys.exit(main())
