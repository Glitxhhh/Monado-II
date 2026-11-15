#!/usr/bin/env python3
"""Build a private, game-free portable Windows setup EXE from reviewed exports.

No network, global installation or game generation occurs here. Every source
member comes from an explicit checked export manifest. Python/tool files come
from the pinned original archives, and native files from the checked dependency
inventory plus a successful native build's actual dependency closure.
"""
from __future__ import annotations
import argparse
import ctypes
from ctypes import wintypes
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import stat
import struct
import subprocess
import sys
import zipfile

PORT = Path(__file__).resolve().parents[1]
MAX_FILE = 512 * 1024 * 1024
HEADER = struct.Struct("<8sIIQ")
RECORD = struct.Struct("<IIQQ32s")
FOOTER = struct.Struct("<8sQQ32s")


def digest(data):
    return hashlib.sha256(data).hexdigest()


def encoded(value):
    return (json.dumps(value, sort_keys=True, indent=2, ensure_ascii=False) + "\n").encode("utf-8")


def name(value):
    if (not isinstance(value, str) or not value or len(value) > 4096 or
            not value.isascii() or value != str(PurePosixPath(value)) or
            any(c in value for c in '\\:<>"|?*') or value.startswith("/") or
            any(ord(c) < 32 or ord(c) == 127 for c in value)):
        raise ValueError("Unsafe portable payload member: " + str(value))
    for part in value.split("/"):
        stem = part.split(".", 1)[0].upper()
        if (part in ("", ".", "..") or part.endswith((".", " ")) or
                stem in {"CON", "PRN", "AUX", "NUL"} or
                re.fullmatch(r"(?:COM|LPT)[1-9]", stem)):
            raise ValueError("Unsafe Windows payload component: " + part)
    return value


def ordinary(path, limit=MAX_FILE):
    path = Path(path).absolute()
    if path.resolve() != path:
        raise ValueError("Payload source and parents must be ordinary: " + str(path))
    for parent in (*reversed(path.parents), path):
        info = parent.lstat()
        if info.st_file_attributes & stat.FILE_ATTRIBUTE_REPARSE_POINT:
            raise ValueError("Reparse payload source: " + str(parent))
    before = path.stat()
    if not stat.S_ISREG(before.st_mode) or before.st_nlink != 1 or before.st_size > limit:
        raise ValueError("Expected a bounded ordinary single-link file: " + str(path))
    data = path.read_bytes()
    after = path.stat()
    snap = lambda x: (x.st_ino, x.st_dev, x.st_size, x.st_mtime_ns, x.st_ctime_ns)
    if snap(before) != snap(after) or len(data) != before.st_size:
        raise ValueError("Payload source changed while reading: " + str(path))
    return data


def document(path, limit=16 * 1024 * 1024):
    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError("Duplicate manifest key: " + key)
            result[key] = value
        return result
    value = json.loads(ordinary(path, limit), object_pairs_hook=unique)
    if not isinstance(value, dict):
        raise ValueError("Expected manifest object")
    return value


class Member:
    def __init__(self, source=None, data=None, archive=None, member=None, identity=None):
        self.source, self.data, self.archive, self.member = source, data, archive, member
        self.identity = identity

    def read(self):
        data = (ordinary(self.source) if self.source is not None else
                self.archive.read(self.member) if self.archive is not None else self.data)
        if len(data) > MAX_FILE:
            raise ValueError("Payload member exceeds limit")
        identity = {"bytes": len(data), "sha256": digest(data)}
        if self.identity is not None and identity != self.identity:
            raise ValueError("Checked payload member changed: " + str(self.source or self.member))
        self.identity = identity
        return data


class Members(dict):
    def __init__(self):
        super().__init__()
        self.folded = set()


class Compressor:
    """Use the same built-in Win32 codec as the standalone native reader."""
    def __init__(self):
        self.dll = ctypes.WinDLL("cabinet", use_last_error=True)
        self.dll.CreateCompressor.argtypes = [wintypes.DWORD, ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p)]
        self.dll.CreateCompressor.restype = wintypes.BOOL
        self.dll.Compress.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
                                      ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
        self.dll.Compress.restype = wintypes.BOOL
        self.dll.CloseCompressor.argtypes = [ctypes.c_void_p]
        self.handle = ctypes.c_void_p()
        if not self.dll.CreateCompressor(4, None, ctypes.byref(self.handle)):
            raise ctypes.WinError(ctypes.get_last_error())

    def compress(self, data):
        if len(data) < 128:
            return 0, data
        required = ctypes.c_size_t()
        source = ctypes.create_string_buffer(data)
        self.dll.Compress(self.handle, source, len(data), None, 0, ctypes.byref(required))
        if not 0 < required.value <= MAX_FILE:
            raise ValueError("Invalid Windows compressor size")
        target = ctypes.create_string_buffer(required.value)
        if not self.dll.Compress(self.handle, source, len(data), target, len(target), ctypes.byref(required)):
            raise ctypes.WinError(ctypes.get_last_error())
        result = target.raw[:required.value]
        return (1, result) if len(result) < len(data) else (0, data)

    def close(self):
        if self.handle:
            self.dll.CloseCompressor(self.handle)
            self.handle = None


def add(members, path, member):
    name(path)
    if path.lower() in members.folded:
        raise ValueError("Case-insensitive payload collision: " + path)
    if path.startswith("state/") or path.startswith("."):
        raise ValueError("Payload cannot contain mutable/private state")
    members[path] = member
    members.folded.add(path.lower())


def reviewed_source(root, manifest_path, members):
    manifest = document(manifest_path)
    if manifest.get("schema") != 1 or manifest.get("platform") != "windows-amd64":
        raise ValueError("A reviewed Windows-only source export manifest is required")
    files = manifest.get("files")
    if not isinstance(files, dict) or not 1 <= len(files) <= 10000:
        raise ValueError("Invalid reviewed source inventory")
    # An exported tree is a concrete review boundary: no undeclared extras.
    actual = {p.relative_to(root).as_posix() for p in root.rglob("*") if p.is_file()}
    allowed_extra = {manifest_path.relative_to(root).as_posix()} if manifest_path.is_relative_to(root) else set()
    if actual != set(files) | allowed_extra:
        raise ValueError("Reviewed source export has missing or undeclared files")
    for path, identity in files.items():
        name(path)
        if ("Monado_Linux" in path.split("/") or any(p in {"game_files", "generated", "runs", "saves", "build", ".deps", ".git", "updates"} for p in path.split("/")) or
                Path(path).suffix.lower() in {".exe", ".dll", ".o", ".obj", ".spv", ".sav", ".nso", ".nca", ".nsp", ".xci", ".keys"}):
            raise ValueError("Source export contains a private/compiled/game member: " + path)
        add(members, "source/" + path, Member(source=root / path, identity=identity))
    add(members, "source-manifest.json", Member(data=encoded(manifest)))
    for source, target in (("Monado_Windows/tools/bootstrap_windows.py", "setup/bootstrap_windows.py"),
                           ("tools/update_windows_release.py", "setup/update_windows_release.py"),
                           ("tools/update_windows_retention.py", "setup/update_windows_retention.py"),
                           ("tools/update_release.py", "setup/update_release.py"),
                           ("tools/publication_guard.py", "setup/publication_guard.py"),
                           ("tools/windows_file_guard.py", "setup/windows_file_guard.py")):
        if source not in files:
            raise ValueError("Missing reviewed setup helper: " + source)
        add(members, target, Member(source=root / source, identity=files[source]))


def python_archives(directory, members):
    runtime = document(PORT / "tools/windows-python-runtime.lock.json")
    runtime_file = directory / Path(runtime["url"]).name
    if digest(ordinary(runtime_file)) != runtime["sha256"]:
        raise ValueError("Pinned official Python archive checksum mismatch")
    archives = [(runtime_file, "python/")]
    lock = (PORT / "tools/windows-python.lock.txt").read_text(encoding="utf-8")
    for package, version, sha in re.findall(r"^([a-zA-Z0-9_-]+)==([^ ]+) --hash=sha256:([0-9a-f]{64})$", lock, re.MULTILINE):
        matches = [p for p in directory.glob("*.whl") if p.name.lower().startswith(package.replace("-", "_").lower() + "-" + version + "-")]
        if len(matches) != 1 or digest(ordinary(matches[0])) != sha:
            raise ValueError("Missing or changed pinned wheel: " + package)
        archives.append((matches[0], "python/Lib/site-packages/"))
    if len(archives) != 6:
        raise ValueError("Incomplete pinned Python tool closure")
    opened = []
    for path, prefix in archives:
        archive = zipfile.ZipFile(path)
        opened.append(archive)
        for info in archive.infolist():
            if info.is_dir():
                continue
            name(info.filename)
            if info.file_size > MAX_FILE or stat.S_ISLNK(info.external_attr >> 16):
                raise ValueError("Unsafe member in checked Python archive")
            if "/__pycache__/" in info.filename or info.filename.endswith(".pyc"):
                continue
            target = info.filename
            # CPython's isolated embed configuration excludes installed sibling
            # tools. Preserve it as documentation, then use ordinary ZIP/Lib
            # discovery with -B and a persistent executable location.
            if target == "python314._pth":
                target += ".disabled"
            add(members, prefix + target, Member(archive=archive, member=info.filename))
    return opened


def native_dependencies(prefix, build_receipt, members):
    config = document(PORT / ".deps/windows-deps.json")
    if (Path(config["prefix"]).resolve() != prefix or
            config.get("lock_sha256") != digest(ordinary(PORT / "tools/windows-deps.lock.json")) or
            config.get("inventory_sha256") != digest(ordinary(prefix / "content-inventory.json")) or
            ordinary(prefix / "receipt.json") != ordinary(PORT / "tools/windows-deps.lock.json")):
        raise ValueError("Native prefix is not the checked project-local pinned cache")
    inventory = document(prefix / "content-inventory.json")
    receipt = document(build_receipt)
    if receipt.get("status") != "success" or receipt.get("implementation") != "windows-parity-development":
        raise ValueError("A successful native Windows build receipt is required")
    selected = {p for p in inventory if p.startswith(("include/", "share/licenses/"))}
    for path, sha in receipt["import_libraries"].items():
        path = Path(path)
        relative = "lib/" + path.name
        if path.parent.name != "lib" or relative not in inventory or inventory[relative]["sha256"] != sha:
            raise ValueError("Import library does not match locked dependency inventory")
        selected.add(relative)
    for relative, sha in receipt["outputs"].items():
        leaf = Path(relative).name
        if not leaf.lower().endswith(".dll") or leaf.lower() == "lasgame.dll":
            continue
        candidate = "bin/" + leaf
        if candidate not in inventory or inventory[candidate]["sha256"] != sha:
            raise ValueError("Runtime DLL does not match checked native prefix: " + leaf)
        selected.add(candidate)
    if not any(p.startswith("bin/") for p in selected):
        raise ValueError("Native build receipt contains no checked runtime closure")
    subset = {p: inventory[p] for p in sorted(selected)}
    for path, identity in subset.items():
        add(members, "native/ucrt64/" + path, Member(source=prefix / path, identity=identity))
    add(members, "native/ucrt64/content-inventory.json", Member(data=encoded(subset)))
    add(members, "native/ucrt64/receipt.json", Member(source=prefix / "receipt.json"))


def write_payload(path, members):
    if not 1 <= len(members) <= 100000:
        raise ValueError("Invalid portable payload member count")
    identities, total = {}, 0
    for key, member in members.items():
        member.read()
        identities[key] = member.identity
        total += member.identity["bytes"]
    manifest = {"schema": 1, "platform": "windows-amd64", "files": identities}
    add(members, "payload-manifest.json", Member(data=encoded(manifest)))
    total += len(members["payload-manifest.json"].data)
    if total > 4 * 1024 * 1024 * 1024:
        raise ValueError("Portable setup content exceeds its bound")
    compressor = Compressor()
    try:
        with path.open("xb") as stream:
            stream.write(HEADER.pack(b"MONADOP1", 1, len(members), total))
            for i, key in enumerate(sorted(members, key=str.lower), 1):
                data = members[key].read()
                flag, stored = compressor.compress(data)
                path_bytes = key.encode("utf-8")
                stream.write(RECORD.pack(len(path_bytes), flag, len(data), len(stored), hashlib.sha256(data).digest()))
                stream.write(path_bytes)
                stream.write(stored)
                if i % 250 == 0 or i == len(members):
                    print(f"Packed {i}/{len(members)} checked files", flush=True)
            stream.flush()
            os.fsync(stream.fileno())
    finally:
        compressor.close()
    return manifest


def build(args):
    if os.name != "nt":
        raise ValueError("The portable executable is built and qualified natively on Windows")
    output = args.output.absolute()
    if output.exists() or output.resolve() != output or not output.is_relative_to(PORT / "build"):
        raise ValueError("Choose a fresh ordinary output below Monado_Windows/build")
    output.mkdir(parents=True)
    members = Members()
    reviewed_source(args.source_export.resolve(), args.source_manifest.resolve(), members)
    archives = python_archives(args.python_archives.resolve(), members)
    try:
        native_dependencies(args.prefix.resolve(), args.native_receipt.resolve(), members)
        manifest = write_payload(output / "setup.payload", members)
    finally:
        for archive in archives:
            archive.close()
    env = os.environ.copy()
    env["ZIG_GLOBAL_CACHE_DIR"] = str(PORT / ".deps/zig-global-cache")
    resource = output / "builder.res"
    commands = [[sys.executable, "-m", "ziglang", "rc", "/I", str(PORT / "tools/builder"), "/fo", str(resource), str(PORT / "tools/builder/builder.rc")],
                [sys.executable, "-m", "ziglang", "c++", "-std=c++17", "-O1", "-UNDEBUG", "-march=x86_64_v2", "-ffp-contract=off", "-fno-fast-math",
                 str(PORT / "tools/builder/windows_builder.cpp"), str(resource), "-o", str(output / "builder-base.exe"), "-lbcrypt", "-lcabinet", "-lole32", "-lshell32", "-lcomctl32", "-luuid", "-lgdi32", "-luser32", "-Wl,--subsystem,windows", "-municode"]]
    for index, command in enumerate(commands):
        result = subprocess.run(command, cwd=PORT, env=env, capture_output=True, timeout=600)
        (output / f"native-{index}.out").write_bytes(result.stdout)
        (output / f"native-{index}.err").write_bytes(result.stderr)
        if result.returncode:
            raise RuntimeError("Native builder compilation failed: " + result.stderr.decode("utf-8", "replace"))
    target = output / "MonadoII-Windows-Setup-Dev.exe"
    with target.open("xb") as stream:
        stream.write(ordinary(output / "builder-base.exe"))
        offset = stream.tell()
        h = hashlib.sha256()
        with (output / "setup.payload").open("rb") as source:
            while chunk := source.read(1 << 20):
                stream.write(chunk)
                h.update(chunk)
        size = stream.tell() - offset
        stream.write(FOOTER.pack(b"M2BNDL01", offset, size, h.digest()))
        stream.flush()
        os.fsync(stream.fileno())
    evidence = {"schema": 1, "kind": "private-native-windows-portable-setup-development", "public_distribution_approved": False,
                "executable": {"bytes": target.stat().st_size, "sha256": digest(ordinary(target, 2 * 1024 * 1024 * 1024))},
                "payload_sha256": h.hexdigest(), "payload_manifest_sha256": digest(encoded(manifest)),
                "source_manifest_sha256": digest(ordinary(args.source_manifest)), "commands": commands,
                "members": len(members), "contains_game_code": False}
    (output / "builder-receipt.json").write_bytes(encoded(evidence))
    print(target)


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--source-export", required=True, type=Path)
    p.add_argument("--source-manifest", required=True, type=Path)
    p.add_argument("--python-archives", required=True, type=Path)
    p.add_argument("--prefix", required=True, type=Path)
    p.add_argument("--native-receipt", required=True, type=Path)
    p.add_argument("--output", required=True, type=Path)
    args = p.parse_args(argv)
    try:
        build(args)
    except (Exception, KeyboardInterrupt) as e:
        print(str(e), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
