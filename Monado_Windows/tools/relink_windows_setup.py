#!/usr/bin/env python3
"""Attach an original setup's verified payload to a rebuilt native frontend.

Only Python's standard library is used. No executable is run, payload extracted,
network contacted, or existing output replaced. Verify the published original
setup against its release checksums before using it. Its embedded SHA256 checks
payload integrity; it is not a publisher signature.
"""
from __future__ import annotations

import argparse
import contextlib
import hashlib
import json
import os
from pathlib import Path
import stat
import struct
import sys

FOOTER = struct.Struct('<8sQQ32s')
HEADER = struct.Struct('<8sIIQ')
MAGIC = b'M2BNDL01'
CHUNK = 1024 * 1024
MAX_TOTAL = 2304 * 1024 * 1024
MAX_BASE = 256 * 1024 * 1024
MAX_PAYLOAD = 2 * 1024 * 1024 * 1024
REPARSE = getattr(stat, 'FILE_ATTRIBUTE_REPARSE_POINT', 0x400)
DEVICES = {'con', 'prn', 'aux', 'nul'} | {prefix + str(i) for prefix in ('com', 'lpt') for i in range(1, 10)}


class RelinkError(ValueError):
    """An input identity, payload or fresh-output boundary was rejected."""


def _path(value):
    text = os.fspath(value)
    if not isinstance(text, str) or text.startswith(('\\\\?\\', '\\\\.\\')):
        raise RelinkError('Use an ordinary Unicode filesystem path')
    path = Path(text)
    for part in path.parts:
        if part == path.anchor:
            continue
        if (part in ('.', '..') or part.endswith((' ', '.')) or
                part.split('.', 1)[0].casefold() in DEVICES or
                any(ord(char) < 32 or ord(char) == 127 or char in '\\:<>"|?*' for char in part)):
            raise RelinkError('Use a canonical portable path without parent traversal or device names')
    return path.absolute()


def _linked(info):
    return stat.S_ISLNK(info.st_mode) or bool(getattr(info, 'st_file_attributes', 0) & REPARSE)


def _directory_identity(info):
    if not stat.S_ISDIR(info.st_mode) or _linked(info):
        raise RelinkError('A parent path is linked, redirected or not an ordinary directory')
    return info.st_dev, info.st_ino


def _parents(path):
    return {directory: _directory_identity(directory.lstat()) for directory in reversed(path.parents)}


def _check_parents(identities):
    for path, expected in identities.items():
        if _directory_identity(path.lstat()) != expected:
            raise RelinkError('A parent directory changed during repacking')


def _regular(info, maximum):
    if not stat.S_ISREG(info.st_mode) or _linked(info) or info.st_nlink != 1 or not 0 < info.st_size <= maximum:
        raise RelinkError('Inputs must be bounded ordinary single-link files')


def _common(info):
    # Windows stat/fstat can disagree on executable bits and ctime meanings.
    return info.st_dev, info.st_ino, stat.S_IFMT(info.st_mode), info.st_size, info.st_mtime_ns


def _snapshot(info):
    return (_common(info), info.st_mode, info.st_nlink, info.st_ctime_ns,
            getattr(info, 'st_birthtime_ns', None), getattr(info, 'st_file_attributes', None))


@contextlib.contextmanager
def _input(path, maximum):
    parents = _parents(path)
    before = path.lstat()
    _regular(before, maximum)
    flags = os.O_RDONLY | getattr(os, 'O_BINARY', 0) | getattr(os, 'O_NOFOLLOW', 0) | getattr(os, 'O_NONBLOCK', 0)
    descriptor = os.open(path, flags)
    with os.fdopen(descriptor, 'rb') as stream:
        opened = os.fstat(stream.fileno())
        _regular(opened, maximum)
        if _common(opened) != _common(before):
            raise RelinkError('Input file changed before opening')
        _check_parents(parents)
        yield stream, opened.st_size
        if (_snapshot(os.fstat(stream.fileno())) != _snapshot(opened)
                or _snapshot(path.lstat()) != _snapshot(before)):
            raise RelinkError('Input file changed while repacking')
        _check_parents(parents)


def _read(stream, offset, size):
    stream.seek(offset)
    result = stream.read(size)
    if len(result) != size:
        raise RelinkError('Truncated executable or setup payload')
    return result


def _hash(stream, offset, size):
    stream.seek(offset)
    result = hashlib.sha256()
    while size:
        block = stream.read(min(CHUNK, size))
        if not block:
            raise RelinkError('Truncated executable or setup payload')
        result.update(block)
        size -= len(block)
    return result.hexdigest()


def _pe(stream, size):
    if size < 4096 or _read(stream, 0, 2) != b'MZ':
        raise RelinkError('Expected an unbundled AMD64 Windows frontend executable')
    offset = struct.unpack('<I', _read(stream, 0x3c, 4))[0]
    if not 64 <= offset <= min(1024 * 1024, size - 24):
        raise RelinkError('Invalid frontend PE header bounds')
    header = _read(stream, offset, 24)
    machine, sections = struct.unpack_from('<HH', header, 4)
    optional, flags = struct.unpack_from('<HH', header, 20)
    if (header[:4] != b'PE\0\0' or machine != 0x8664 or not 1 <= sections <= 96
            or not 240 <= optional <= 4096 or not flags & 2 or flags & 0x2000
            or offset + 24 + optional + 40 * sections > size):
        raise RelinkError('Expected an AMD64 PE32+ executable, not a DLL or foreign image')
    if (struct.unpack('<H', _read(stream, offset + 24, 2))[0] != 0x20b or
            struct.unpack('<H', _read(stream, offset + 24 + 68, 2))[0] != 2):
        raise RelinkError('Expected a Windows GUI PE32+ frontend')


def _payload(stream, size):
    if size < 4096 + HEADER.size + FOOTER.size or size > MAX_TOTAL:
        raise RelinkError('Original setup has no bounded portable payload')
    magic, offset, count, expected = FOOTER.unpack(_read(stream, size - FOOTER.size, FOOTER.size))
    if (magic != MAGIC or offset < 4096 or not HEADER.size <= count <= MAX_PAYLOAD
            or offset != size - FOOTER.size - count):
        raise RelinkError('Original setup footer magic or payload bounds differ')
    header, version, members, expanded = HEADER.unpack(_read(stream, offset, HEADER.size))
    if header != b'MONADOP1' or version != 1 or not 1 <= members <= 100000 or expanded > 4 * 1024 * 1024 * 1024:
        raise RelinkError('Original setup payload header is unsupported or oversized')
    if _hash(stream, offset, count) != expected.hex():
        raise RelinkError('Original setup payload SHA256 differs')
    return offset, count, expected


def _copy(stream, offset, size, output, complete):
    stream.seek(offset)
    result = hashlib.sha256()
    while size:
        block = stream.read(min(CHUNK, size))
        if not block:
            raise RelinkError('Input became truncated while copying')
        if output.write(block) != len(block):
            raise OSError('Short write while creating the modified setup')
        result.update(block)
        complete.update(block)
        size -= len(block)
    return result.hexdigest()


def repack(original_setup, base_executable, output):
    """Copy a new base and exact original payload into one exclusively new file.

    Files/ancestors are checked before and after streaming; this portable tool
    does not claim native ancestor-lease isolation against hostile namespace
    races. A failed fresh partial output is retained for inspection, not removed.
    """
    original, base, output = map(_path, (original_setup, base_executable, output))
    if len({original, base, output}) != 3:
        raise RelinkError('Original setup, unbundled base and fresh output must be distinct')
    parents = _parents(output)
    if output.exists() or output.is_symlink():
        raise RelinkError('Output must be new; existing files are preserved')
    with _input(original, MAX_TOTAL) as (source, original_size), _input(base, MAX_BASE) as (frontend, base_size):
        if os.path.samestat(os.fstat(source.fileno()), os.fstat(frontend.fileno())):
            raise RelinkError('Original setup and base executable alias one file')
        _pe(source, original_size)
        _pe(frontend, base_size)
        if _read(frontend, base_size - FOOTER.size, 8) == MAGIC:
            raise RelinkError('Pass the unbundled rebuilt frontend, not another complete setup')
        old_offset, payload_size, payload_hash = _payload(source, original_size)
        total = base_size + payload_size + FOOTER.size
        if total > MAX_TOTAL:
            raise RelinkError('Modified setup exceeds the native portable reader bound')
        original_hash = _hash(source, 0, original_size)
        base_hash = _hash(frontend, 0, base_size)
        _check_parents(parents)
        flags = os.O_RDWR | os.O_CREAT | os.O_EXCL | getattr(os, 'O_BINARY', 0) | getattr(os, 'O_NOFOLLOW', 0)
        descriptor = os.open(output, flags, 0o600)
        with os.fdopen(descriptor, 'w+b') as destination:
            created = os.fstat(destination.fileno())
            if not stat.S_ISREG(created.st_mode) or created.st_nlink != 1:
                raise RelinkError('Fresh output is not an ordinary single-link file')
            _check_parents(parents)
            complete = hashlib.sha256()
            if _copy(frontend, 0, base_size, destination, complete) != base_hash:
                raise RelinkError('Rebuilt frontend changed during copying')
            if _copy(source, old_offset, payload_size, destination, complete) != payload_hash.hex():
                raise RelinkError('Original payload changed during copying')
            footer = FOOTER.pack(MAGIC, base_size, payload_size, payload_hash)
            if destination.write(footer) != len(footer):
                raise OSError('Short write while creating the modified setup footer')
            complete.update(footer)
            destination.flush()
            os.fsync(destination.fileno())
            final = os.fstat(destination.fileno())
            if final.st_size != total or final.st_nlink != 1:
                raise RelinkError('Modified setup size or link count changed')
            actual = _hash(destination, 0, total)
            if actual != complete.hexdigest() or _common(output.lstat()) != _common(final):
                raise RelinkError('Modified setup readback or file identity differs')
            _check_parents(parents)
    return dict(schema=1, status='repacked', original_setup_sha256=original_hash,
        original_offset=old_offset, base_sha256=base_hash, payload_sha256=payload_hash.hex(),
        payload_bytes=payload_size, new_offset=base_size, output_bytes=total, output_sha256=actual,
        payload_unchanged=True, executable_run=False)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--original-setup', required=True, type=Path)
    parser.add_argument('--base-executable', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path, help='fresh output; existing files are never replaced')
    args = parser.parse_args(argv)
    try:
        result = repack(args.original_setup, args.base_executable, args.output)
    except (OSError, ValueError) as error:
        parser.exit(1, 'Setup repack rejected: ' + str(error) + '\n')
    print(json.dumps(result, sort_keys=True))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
