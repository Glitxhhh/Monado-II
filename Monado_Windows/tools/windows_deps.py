"""Resolve and fetch ordinary MSYS2 UCRT64 build dependencies into this project.

No pacman database/system installation or vendor SDK agreement is modified.  The
checked lock records exact official package URLs, hashes and licences; downloaded
archives are verified before a fresh private dependency tree is selected.
"""
from __future__ import annotations

import argparse
import concurrent.futures
import hashlib
import io
import json
from pathlib import Path, PurePosixPath
import re
import shutil
import tarfile
import time
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
ORIGIN = "https://repo.msys2.org/mingw/ucrt64/"
PREFIX = "mingw-w64-ucrt-x86_64-"
REQUESTED = [PREFIX + n for n in (
    "sdl3", "ffmpeg", "vulkan-headers", "vulkan-loader", "shaderc",
    "vulkan-memory-allocator", "opus")]


def digest(path):
    with path.open("rb") as f:
        return hashlib.file_digest(f, "sha256").hexdigest()


def fetch(url, path, expected=None):
    if path.is_file() and (expected is None or digest(path) == expected):
        return path
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(path.suffix + ".download")
    with urllib.request.urlopen(url, timeout=120) as src, tmp.open("wb") as dst:
        shutil.copyfileobj(src, dst)
    if expected is not None and digest(tmp) != expected:
        raise RuntimeError(f"SHA256 mismatch: {url}")
    tmp.replace(path)
    return path


def content_inventory(prefix):
    result = {}
    for path in sorted(prefix.rglob("*")):
        if path.is_symlink() or path.is_junction():
            raise RuntimeError("Dependency cache contains a filesystem link: " + str(path))
        if path.is_file() and path.name not in ("content-inventory.json", "receipt.json"):
            result[path.relative_to(prefix).as_posix()] = {"bytes": path.stat().st_size, "sha256": digest(path)}
    return result


def verify(lock):
    config = json.loads((ROOT / ".deps/windows-deps.json").read_text(encoding="utf-8"))
    prefix = Path(config["prefix"])
    inventory = prefix / "content-inventory.json"
    if config["lock_sha256"] != digest(lock) or not inventory.is_file() or digest(inventory) != config.get("inventory_sha256"):
        raise RuntimeError("Dependency cache lock/inventory identity mismatch")
    if content_inventory(prefix) != json.loads(inventory.read_text(encoding="utf-8")):
        raise RuntimeError("Dependency cache content changed")
    print("Verified project-local dependency content inventory: " + str(prefix))


def resolve(lock):
    db = fetch(ORIGIN + "ucrt64.db", ROOT / ".deps/downloads/ucrt64.db")
    packages, providers = {}, {}
    raw_db = db.read_bytes()
    if raw_db.startswith(b"\x28\xb5\x2f\xfd"):
        import zstandard
        raw_db = zstandard.ZstdDecompressor().stream_reader(io.BytesIO(raw_db)).read()
    with tarfile.open(fileobj=io.BytesIO(raw_db), mode="r:*") as tf:
        for member in tf:
            if not member.name.endswith("/desc"):
                continue
            raw = tf.extractfile(member).read().decode("utf-8")
            fields = {}
            for block in raw.strip().split("\n\n"):
                lines = block.splitlines()
                fields[lines[0].strip("%")] = lines[1:]
            name = fields["NAME"][0]
            packages[name] = fields
            for alias in fields.get("PROVIDES", []):
                providers[re.split(r"[<>=]", alias)[0]] = name
    selected, pending = {}, REQUESTED.copy()
    while pending:
        name = re.split(r"[<>=]", pending.pop())[0]
        name = name if name in packages else providers.get(name, name)
        if name in selected:
            continue
        if name not in packages:
            raise RuntimeError(f"Missing official UCRT64 package: {name}")
        p = packages[name]
        selected[name] = {"name": name, "version": p["VERSION"][0],
                          "url": ORIGIN + p["FILENAME"][0],
                          "sha256": p["SHA256SUM"][0],
                          "bytes": int(p["CSIZE"][0]), "licenses": p.get("LICENSE", [])}
        pending.extend(p.get("DEPENDS", []))
    record = {"schema": 1, "origin": ORIGIN, "database_sha256": digest(db),
              "requested": REQUESTED, "packages": sorted(selected.values(), key=lambda p: p["name"])}
    lock.write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
    print(f"Locked {len(selected)} packages, {sum(p['bytes'] for p in selected.values()) / 1048576:.1f} MiB")


def install(lock):
    import zstandard
    record = json.loads(lock.read_text(encoding="utf-8"))
    identity = digest(lock)[:16]
    final = ROOT / ".deps" / ("ucrt64-" + identity)
    if (final / "receipt.json").is_file():
        inventory = final / "content-inventory.json"
        if not inventory.exists():
            inventory.write_text(json.dumps(content_inventory(final), indent=2) + "\n", encoding="utf-8")
            config = ROOT / ".deps/windows-deps.json"
            record = json.loads(config.read_text(encoding="utf-8"))
            record["inventory_sha256"] = digest(inventory)
            tmp = config.with_suffix(".tmp")
            tmp.write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
            tmp.replace(config)
        verify(lock)
        print(final)
        return
    stage = ROOT / ".deps" / ("ucrt64-" + identity + ".staging-" + str(time.time_ns()))
    def download(p):
        print("Fetching " + p["name"], flush=True)
        return p, fetch(p["url"], ROOT / ".deps/downloads" / p["url"].rsplit("/", 1)[-1], p["sha256"])
    with concurrent.futures.ThreadPoolExecutor(max_workers=6) as pool:
        archives = list(pool.map(download, record["packages"]))
    stage.mkdir(parents=True)
    links = []
    for p, archive in archives:
        print("Extracting " + p["name"], flush=True)
        with archive.open("rb") as f, zstandard.ZstdDecompressor().stream_reader(f) as reader:
            with tarfile.open(fileobj=reader, mode="r|") as tf:
                for m in tf:
                    path = PurePosixPath(m.name)
                    if path.is_absolute() or ".." in path.parts or ":" in m.name or "\\" in m.name:
                        raise RuntimeError(f"Unsafe package member: {m.name}")
                    if not path.parts or path.parts[0] != "ucrt64":
                        continue  # package-manager metadata stays in the archive
                    dest = stage.joinpath(*path.parts[1:])
                    if m.islnk() or m.issym():
                        target = PurePosixPath(m.linkname)
                        if target.is_absolute() or ":" in m.linkname or "\\" in m.linkname:
                            raise RuntimeError(f"Unsafe package link: {m.name}")
                        if m.islnk():
                            if not target.parts or target.parts[0] != "ucrt64":
                                raise RuntimeError(f"External package hardlink: {m.name}")
                            source = stage.joinpath(*target.parts[1:])
                        else:
                            source = dest.parent.joinpath(*target.parts)
                        source = source.resolve()
                        if not source.is_relative_to(stage.resolve()):
                            raise RuntimeError(f"Escaping package link: {m.name}")
                        links.append((dest, source))
                        continue
                    if not (m.isfile() or m.isdir()):
                        raise RuntimeError(f"Nonordinary package member: {m.name}")
                    if m.isdir():
                        dest.mkdir(parents=True, exist_ok=True)
                    else:
                        dest.parent.mkdir(parents=True, exist_ok=True)
                        with tf.extractfile(m) as src, dest.open("wb") as dst:
                            shutil.copyfileobj(src, dst)
    # Materialize reviewed archive-internal aliases as ordinary files, never
    # filesystem links or reparse points in the selected dependency tree.
    while links:
        remaining = []
        for dest, source in links:
            if source.is_file() and not source.is_symlink():
                dest.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(source, dest)
            else:
                remaining.append((dest, source))
        if len(remaining) == len(links):
            raise RuntimeError("Unresolved package aliases: " + str(remaining[:3]))
        links = remaining
    (stage / "receipt.json").write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
    inventory = stage / "content-inventory.json"
    inventory.write_text(json.dumps(content_inventory(stage), indent=2) + "\n", encoding="utf-8")
    inventory_sha256 = digest(inventory)
    stage.replace(final)
    config = ROOT / ".deps/windows-deps.json"
    tmp = config.with_suffix(".tmp")
    tmp.write_text(json.dumps({"prefix": str(final), "lock_sha256": digest(lock), "inventory_sha256": inventory_sha256}, indent=2) + "\n", encoding="utf-8")
    tmp.replace(config)
    print(final)


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("action", choices=("resolve", "install", "verify"))
    ap.add_argument("--lock", type=Path, default=ROOT / "tools/windows-deps.lock.json")
    a = ap.parse_args()
    {"resolve": resolve, "install": install, "verify": verify}[a.action](a.lock)
