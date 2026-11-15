"""Port-selectable authored renderer fixtures; native Windows uses pinned local Zig."""
from pathlib import Path
import os
import shutil
import sys

ROOT = Path(__file__).resolve().parents[1]
PORT_NAME = os.environ.get("MONADO_TEST_PORT", "Monado_Windows" if sys.platform == "win32" else "Monado_Linux")
if PORT_NAME not in ("Monado_Windows", "Monado_Linux"):
    raise ValueError("MONADO_TEST_PORT must be Monado_Windows or Monado_Linux")
PORT = ROOT / PORT_NAME
WINDOWS = sys.platform == "win32"

def compiler_command():
    explicit = os.environ.get("MONADO_TEST_CC")
    if explicit:
        return [explicit]
    zig = PORT / ".venv/Lib/site-packages/ziglang/zig.exe"
    if WINDOWS and zig.is_file():
        return [str(zig), "cc"]
    local = PORT / ".deps/bin/clang-cc"
    compiler = str(local) if local.is_file() else shutil.which("clang")
    return [compiler] if compiler else None

def compiler_environment():
    environment = os.environ.copy()
    if WINDOWS:
        for key, subdir in (("ZIG_LOCAL_CACHE_DIR", "zig-cache"), ("ZIG_GLOBAL_CACHE_DIR", "zig-global-cache")):
            directory = PORT / "build" / subdir
            directory.mkdir(parents=True, exist_ok=True)
            environment[key] = str(directory)
    return environment

def binary_name(name):
    return name + (".exe" if WINDOWS else "")

def native_flags():
    flags = ["-std=gnu11", "-O1", "-UNDEBUG", "-march=x86_64_v2", "-ffp-contract=off", "-fno-strict-aliasing",
             "-ffunction-sections", "-fdata-sections", "-Wall", "-Wextra", "-Werror",
             "-Wno-format", "-Wno-unused-parameter", "-Wno-unused-function", "-pthread"]
    if not WINDOWS:
        flags += ["-Wl,--gc-sections"]
    return flags

def graphics_prefix():
    import json
    explicit = os.environ.get("MONADO_TEST_GRAPHICS_PREFIX")
    if explicit:
        return Path(explicit)
    config = PORT / ".deps/windows-deps.json"
    if WINDOWS and config.is_file():
        return Path(json.loads(config.read_text(encoding="utf8"))["prefix"])
    return None
