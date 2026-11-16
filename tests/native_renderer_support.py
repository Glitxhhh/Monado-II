"""Port-selectable authored renderer fixtures; native Windows uses pinned local Zig."""
from pathlib import Path
import os
import shutil
import subprocess
import unittest
from functools import lru_cache
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
    if WINDOWS:
        zig = pinned_zig()
        # Native Windows fixtures use GNU import archives, Zig's resource
        # compiler and MinGW headers. Ambient MSVC-targeting Clang is not that
        # qualified toolchain; source-only CI does not install private tools.
        if zig is None and _configured_native_setup():
            raise AssertionError("Configured native setup is missing its pinned Windows Zig compiler")
        return [str(zig), "cc"] if zig else None
    local = PORT / ".deps/bin/clang-cc"
    compiler = str(local) if local.is_file() else shutil.which("clang")
    return [compiler] if compiler else None

def _zig_candidates():
    return (PORT / ".venv/Lib/site-packages/ziglang/zig.exe",
            Path(sys.executable).parent / "Lib/site-packages/ziglang/zig.exe",
            PORT / ".deps/python314/Lib/site-packages/ziglang/zig.exe")

def _configured_native_setup():
    # A partial/explicit installation is a failing fixture precondition, not
    # evidence that a native component is unavailable on a clean source runner.
    if any(key in os.environ for key in ("MONADO_TEST_CC", "MONADO_NATIVE_TEST_COMPILER",
                                         "MONADO_TEST_GRAPHICS_PREFIX")):
        return True
    paths = (PORT / ".deps/windows-deps.json", *_zig_candidates())
    return any(path.exists() or path.is_symlink() for path in paths)

def pinned_zig():
    explicit = os.environ.get("MONADO_NATIVE_TEST_COMPILER")
    if explicit:
        path = Path(explicit)
        if not path.is_file():
            raise FileNotFoundError("explicit native test compiler is missing: " + str(path))
        return path
    for path in _zig_candidates():
        if path.exists() or path.is_symlink():
            if not path.is_file():
                raise AssertionError("Configured pinned compiler is not an ordinary file: " + str(path))
            return path
    return None

def require_pinned_zig():
    compiler = pinned_zig()
    if compiler is None:
        if _configured_native_setup():
            raise AssertionError("Configured native setup is missing its pinned Windows Zig compiler")
        raise unittest.SkipTest("Pinned native Windows Zig toolchain is not installed")
    return compiler

@lru_cache(maxsize=8)
def _zig_command(command):
    result = subprocess.run([*command, '--version'], capture_output=True, text=True, timeout=30)
    if result.returncode:
        raise AssertionError(result.stdout + result.stderr)
    return 'ziglang' in result.stdout or Path(command[0]).name.lower() in ('zig', 'zig.exe')

def compiler_is_zig():
    command = compiler_command()
    return bool(command and _zig_command(tuple(command)))

def require_native_graphics(*, production_defaults=False):
    # Inspect graphics metadata even when the compiler is absent, so a corrupt
    # or partial configured setup cannot be mistaken for a clean CI absence.
    prefix = graphics_prefix()
    compiler = compiler_command()
    if prefix is None and compiler is None and not _configured_native_setup():
        raise unittest.SkipTest("Native Windows compiler/graphics setup is not installed")
    if compiler is None:
        raise AssertionError("Configured native graphics setup is missing its compiler")
    if prefix is None:
        raise AssertionError("Configured native compiler setup is missing graphics dependency metadata")
    if not prefix.is_absolute():
        raise AssertionError("Configured native graphics prefix must be absolute: " + str(prefix))
    if not prefix.is_dir():
        raise AssertionError("Configured native graphics prefix is missing: " + str(prefix))
    if production_defaults:
        for path in (PORT / '.venv/Lib/site-packages/ziglang/zig.exe',
                     PORT / '.deps/windows-deps.json'):
            if not path.is_file():
                raise AssertionError("Production native FSR4 dependency setup is incomplete: " + str(path))
    return prefix

def compiler_environment():
    environment = os.environ.copy()
    if WINDOWS and compiler_is_zig():
        for key, subdir in (("ZIG_LOCAL_CACHE_DIR", "zig-cache"), ("ZIG_GLOBAL_CACHE_DIR", "zig-global-cache")):
            directory = PORT / "build" / subdir
            directory.mkdir(parents=True, exist_ok=True)
            environment[key] = str(directory)
    return environment

def binary_name(name):
    return name + (".exe" if WINDOWS else "")

def native_flags():
    # Only the spelling changes between Zig and stock Clang. Both select the
    # same x86-64-v2 ISA; production compiler flags are not modified here.
    cpu = "-march=x86_64_v2" if compiler_is_zig() else "-march=x86-64-v2"
    flags = ["-std=gnu11", "-O1", "-UNDEBUG", cpu, "-ffp-contract=off", "-fno-strict-aliasing",
             "-ffunction-sections", "-fdata-sections", "-Wall", "-Wextra", "-Werror",
             "-Wno-format", "-Wno-unused-parameter", "-Wno-unused-function", "-pthread"]
    if not WINDOWS:
        flags += ["-Wl,--gc-sections"]
    if os.environ.get('MONADO_TEST_UBSAN') == '1':
        flags += ['-fsanitize=undefined', '-fno-sanitize-recover=all']
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
