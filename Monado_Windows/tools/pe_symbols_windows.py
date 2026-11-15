#!/usr/bin/env python3
"""Inspect bounded AMD64 PE import/export names without loading executable code."""
from __future__ import annotations
import argparse
import json
from pathlib import Path
import struct


class Image:
    def __init__(self, path):
        path = Path(path)
        if path.is_symlink() or path.is_junction() or not path.is_file() or not 64 <= path.stat().st_size <= 512 * 1024 * 1024:
            raise ValueError("Expected a bounded ordinary PE image")
        self.data = path.read_bytes()
        if self.data[:2] != b"MZ":
            raise ValueError("Missing DOS header")
        pe = self.unpack("<I", 0x3c)[0]
        if self.slice(pe, 4) != b"PE\0\0":
            raise ValueError("Missing PE signature")
        machine, count = self.unpack("<HH", pe + 4)
        size = self.unpack("<H", pe + 20)[0]
        opt = pe + 24
        if machine != 0x8664 or not 240 <= size <= 4096 or self.unpack("<H", opt)[0] != 0x20b or self.unpack("<I", opt + 108)[0] < 2 or count > 96:
            raise ValueError("Expected ordinary AMD64 PE32+ image")
        self.slice(opt, size)
        self.header_size = self.unpack("<I", opt + 60)[0]
        self.directories = [self.unpack("<II", opt + 112 + i * 8) for i in range(2)]
        self.sections = []
        for i in range(count):
            s = opt + size + i * 40
            self.slice(s, 40)
            virtual, address, raw, offset = self.unpack("<IIII", s + 8)
            self.slice(offset, raw)
            self.sections.append((address, max(virtual, raw), offset, raw))

    def slice(self, start, size):
        if start < 0 or size < 0 or start + size > len(self.data):
            raise ValueError("PE field is outside image")
        return self.data[start:start + size]

    def unpack(self, fmt, offset):
        return struct.unpack(fmt, self.slice(offset, struct.calcsize(fmt)))

    def offset(self, rva, size=1):
        if rva < self.header_size:
            self.slice(rva, size)
            return rva
        for address, virtual, offset, raw in self.sections:
            if address <= rva < address + virtual and rva - address + size <= raw:
                self.slice(offset + rva - address, size)
                return offset + rva - address
        raise ValueError("PE RVA is outside ordinary raw sections")

    def string(self, rva):
        start = self.offset(rva)
        end = self.data.find(b"\0", start, min(len(self.data), start + 4096))
        if end < 0:
            raise ValueError("Unbounded PE name")
        return self.data[start:end].decode("ascii")

    def imports(self):
        rva, size = self.directories[1]
        if not rva:
            return {}
        if size < 20 or size > 20 * 4096:
            raise ValueError("Invalid import directory bounds")
        result = {}
        for index in range(size // 20):
            entry = self.unpack("<IIIII", self.offset(rva + index * 20, 20))
            if not any(entry):
                return result
            library = self.string(entry[3])
            if not library or library in result or any(c in library for c in "\\/:"):
                raise ValueError("Unsafe/duplicate PE import library")
            names = []
            thunk = entry[0] or entry[4]
            for i in range(200000):
                value = self.unpack("<Q", self.offset(thunk + i * 8, 8))[0]
                if not value:
                    break
                names.append("#" + str(value & 0xffff) if value >> 63 else self.string(value + 2))
            else:
                raise ValueError("Unbounded PE import thunk")
            result[library] = names
        raise ValueError("Missing import directory terminator")

    def exports(self):
        rva, size = self.directories[0]
        if not rva:
            return []
        entry = self.unpack("<IIHHIIIIIII", self.offset(rva, 40))
        count, names, table = entry[7], entry[9], entry[10]
        if count > 200000:
            raise ValueError("Unbounded PE export names")
        result = []
        for i in range(count):
            name_rva = self.unpack("<I", self.offset(names + i * 4, 4))[0]
            ordinal = self.unpack("<H", self.offset(table + i * 2, 2))[0]
            if ordinal >= entry[6]:
                raise ValueError("Invalid PE export ordinal")
            result.append(self.string(name_rva))
        if len(set(result)) != len(result):
            raise ValueError("Duplicate PE export name")
        return sorted(result)


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("image", type=Path)
    p.add_argument("--expect-only-export")
    p.add_argument("--reject-import", action="append", default=[])
    args = p.parse_args(argv)
    image = Image(args.image)
    imports, exports = image.imports(), image.exports()
    if args.expect_only_export and exports != [args.expect_only_export]:
        raise ValueError("Unexpected authored game-library export surface: " + repr(exports))
    rejected = set(args.reject_import)
    if any(rejected.intersection(names) for names in imports.values()):
        raise ValueError("Rejected runtime function is imported instead of locally defined")
    print(json.dumps({"image": str(args.image), "exports": exports, "imports": imports}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
