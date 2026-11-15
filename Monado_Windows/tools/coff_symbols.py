"""Bounded COFF symbol inventory for explicit native host/game import lists.

Reads ordinary compiler objects without executing code or invoking a host-global
tool. Standard AMD64 COFF and LLVM's big-object variant are supported.
"""
from __future__ import annotations

from pathlib import Path
import struct


def symbols(path: Path):
    if path.is_symlink() or path.is_junction() or not path.is_file():
        raise ValueError("COFF input must be an ordinary owned object")
    if not 20 <= path.stat().st_size <= 512 * 1024 * 1024:
        raise ValueError("COFF object size outside bounds")
    data = path.read_bytes()
    if data[:4] == b"\0\0\xff\xff":
        if len(data) < 56 or struct.unpack_from("<H", data, 4)[0] < 2:
            raise ValueError("Unsupported COFF import/big-object header")
        machine = struct.unpack_from("<H", data, 6)[0]
        pointer, count = struct.unpack_from("<II", data, 48)
        width, section_fmt, type_offset, class_offset = 20, "<i", 16, 18
    else:
        machine = struct.unpack_from("<H", data, 0)[0]
        pointer, count = struct.unpack_from("<II", data, 8)
        width, section_fmt, type_offset, class_offset = 18, "<h", 14, 16
    if machine != 0x8664:
        raise ValueError("COFF object is not AMD64")
    end = pointer + count * width
    if not pointer or count > 10_000_000 or end + 4 > len(data):
        raise ValueError("COFF symbol table outside object")
    strings_size = struct.unpack_from("<I", data, end)[0]
    if strings_size < 4 or end + strings_size > len(data):
        raise ValueError("COFF string table outside object")
    defined, undefined = {}, set()
    i = 0
    while i < count:
        base = pointer + i * width
        raw = data[base:base + 8]
        if raw[:4] == b"\0\0\0\0":
            offset = struct.unpack_from("<I", raw, 4)[0]
            if not 4 <= offset < strings_size:
                raise ValueError("COFF symbol name outside string table")
            start = end + offset
            stop = data.find(b"\0", start, min(end + strings_size, start + 65536))
            if stop < 0:
                raise ValueError("Unbounded COFF symbol name")
            raw = data[start:stop]
        name = raw.split(b"\0", 1)[0].decode("ascii")
        value = struct.unpack_from("<I", data, base + 8)[0]
        section = struct.unpack_from(section_fmt, data, base + 12)[0]
        kind = struct.unpack_from("<H", data, base + type_offset)[0]
        storage, auxiliary = data[base + class_offset:base + class_offset + 2]
        if i + auxiliary >= count:
            raise ValueError("COFF auxiliary records outside symbol table")
        if storage == 2 and name:
            if section > 0:
                defined[name] = "function" if kind & 0x20 else "data"
            elif section == 0 and value == 0:
                undefined.add(name)
        i += 1 + auxiliary
    return defined, undefined


def host_imports(game_objects, host_objects):
    game_defined, game_undefined, host_defined = {}, set(), {}
    for object_path in game_objects:
        definitions, references = symbols(object_path)
        game_defined.update(definitions)
        game_undefined.update(references)
    for object_path in host_objects:
        definitions, _ = symbols(object_path)
        host_defined.update(definitions)
    external = game_undefined - game_defined.keys()
    # CRT/Win32/compiler support remains the DLL's ordinary native link closure.
    # Only functions/data actually defined by this authored runtime are exported.
    return {name: host_defined[name] for name in sorted(external & host_defined.keys())}
