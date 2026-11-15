"""Checked local PE GameLib reuse across native Windows source/version roots.

This module never loads code. A completed current-user-owned receipt, exact
canonical guest identity and an ordinary locked DLL/sidecar snapshot are needed.
Authored host definitions must still satisfy every previous import with its
original function/data kind. Reuse never changes or repairs the previous build.
"""
from __future__ import annotations
from dataclasses import dataclass
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import sys

LIMIT = 512 * 1024 * 1024
HEX = re.compile(r"[0-9a-f]{64}\Z")
SYMBOL = re.compile(r"[A-Za-z_][A-Za-z0-9_]*\Z")


class IncompatibleGameLibrary(ValueError):
    """An intact owned library needs fresh generation for the current source."""
    failure_kind = "incompatible-game-library"


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec)
    sys.modules[name] = result
    spec.loader.exec_module(result)
    return result


def canonical_id(identity):
    return hashlib.sha256(json.dumps(identity, sort_keys=True, separators=(",", ":")).encode("utf-8")).hexdigest()


def document(data):
    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError("Duplicate GameLib receipt key")
            result[key] = value
        return result
    value = json.loads(data, object_pairs_hook=unique)
    if not isinstance(value, dict):
        raise ValueError("Expected GameLib receipt object")
    return value


@dataclass
class Candidate:
    previous: Path
    receipt_sha256: str
    identity: dict
    identifier: str
    imports: dict
    files: dict


def inspect(previous, expected_identity, host_definitions, guard=None, pe=None):
    """Expected identity contains every current field EXCEPT host_imports.

    Previous roots may be outside the new checkout, but every ancestor stays
    leased, and the selected root, receipt, build directory and three reusable
    files must be ordinary and owned by the actual current TokenUser.
    """
    port = Path(__file__).resolve().parents[1]
    guard = guard or module("monado_reuse_file_guard", port.parent / "tools/windows_file_guard.py")
    pe = pe or module("monado_reuse_pe", port / "tools/pe_symbols_windows.py")
    previous = Path(guard.canonical_path(previous))
    with guard.locked_directory(previous, owner=True) as root:
        receipt_data = root.read_regular("build-receipt.json", max_bytes=16 * 1024 * 1024, owner=True)
        record = document(receipt_data)
        split = record.get("split_game", {})
        identity = split.get("identity")
        if (record.get("status") != "success" or record.get("implementation") != "windows-parity-development" or
                not record.get("installation") or not isinstance(identity, dict)):
            raise ValueError("GameLib reuse requires a completed native split installation receipt")
        imports = identity.get("host_imports")
        if (not isinstance(imports, dict) or not 1 <= len(imports) <= 200000 or
                any(not isinstance(n, str) or not SYMBOL.fullmatch(n) or k not in ("function", "data") for n, k in imports.items())):
            raise ValueError("Invalid previous GameLib host-import contract")
        if (record.get("flags") != identity["flags"] or record.get("zig") != identity["compiler"]["version"] or
                record.get("input", {}).get("exefs") != identity["input"] or
                record.get("generator_closure") != identity["generator_closure"]):
            raise ValueError("Previous GameLib receipt contradicts its canonical identity")
        identifier = canonical_id(identity)
        if not HEX.fullmatch(split.get("id", "")) or split["id"] != identifier:
            raise ValueError("Previous GameLib canonical ID is corrupt")
        files = {}
        with root.child("build", owner=True) as binary:
            for leaf in ("lasgame.dll", "lasgame.dll.id", "lasgame.dll.sha256"):
                with binary.open_regular(leaf, max_bytes=LIMIT if leaf == "lasgame.dll" else 65, owner=True) as stream:
                    data = stream.read(LIMIT + 1 if leaf == "lasgame.dll" else 66)
                    sha = hashlib.sha256(data).hexdigest()
                    if len(data) > (LIMIT if leaf == "lasgame.dll" else 65) or record.get("outputs", {}).get("build/" + leaf) != sha:
                        raise ValueError("Previous GameLib output hash/size changed: " + leaf)
                    if leaf == "lasgame.dll":
                        if split.get("dll_sha256") != sha:
                            raise ValueError("Previous DLL differs from split receipt")
                        image = pe.Image(previous / "build" / leaf)
                        imported = image.imports()
                        if image.exports() != ["lasgame_tables"]:
                            raise ValueError("Previous GameLib has an unexpected export surface")
                        own_host = [v for n, v in imported.items() if n.casefold() == "xcde.exe"]
                        if len(own_host) != 1 or sorted(own_host[0]) != sorted(imports):
                            raise ValueError("Previous GameLib PE imports contradict its authored host contract")
                        if any({"fma", "fmaf"}.intersection(values) for values in imported.values()):
                            raise ValueError("Previous GameLib imports the incorrect Windows UCRT FMA helpers")
                    elif split.get("sidecars", {}).get(leaf[len("lasgame.dll"):]) != sha:
                        raise ValueError("Previous GameLib sidecar contradicts split receipt")
                    files[leaf] = data
        for leaf, expected in (("lasgame.dll.id", identifier), ("lasgame.dll.sha256", hashlib.sha256(files["lasgame.dll"]).hexdigest())):
            data = files[leaf]
            if data not in (expected.encode("ascii"), (expected + "\n").encode("ascii")):
                raise ValueError("Previous GameLib sidecar content is corrupt: " + leaf)
        # Corruption/ownership/PE failures above are ALWAYS fatal. Only a fully
        # authenticated local library may classify as a clean source fallback.
        actual = {k: v for k, v in identity.items() if k != "host_imports"}
        if actual != expected_identity:
            raise IncompatibleGameLibrary("GameLib input, ABI, generator, guest header, compiler, flags or architecture differs; rebuild from source")
        if any(host_definitions.get(n) != kind for n, kind in imports.items()):
            raise IncompatibleGameLibrary("Current authored host does not satisfy the previous GameLib function/data imports")
        return Candidate(previous, hashlib.sha256(receipt_data).hexdigest(), identity, identifier, dict(sorted(imports.items())), files)


def copy(candidate, destination, guard=None):
    port = Path(__file__).resolve().parents[1]
    guard = guard or module("monado_reuse_copy_guard", port.parent / "tools/windows_file_guard.py")
    with guard.locked_directory(destination, owner=True) as output:
        for leaf, data in candidate.files.items():
            output.write_new(leaf, data, owner=True)
            with output.open_regular(leaf, max_bytes=LIMIT, owner=True) as stream:
                if hashlib.file_digest(stream, "sha256").hexdigest() != hashlib.sha256(data).hexdigest():
                    raise ValueError("Copied GameLib snapshot changed")
