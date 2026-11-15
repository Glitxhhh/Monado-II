#!/usr/bin/env python3
"""Versioned content identity for locally compiled game code, never its contents.

Renderer/UI-only changes can retain an identical library. Generator, guest ABI,
patch, toolchain or strict floating-point policy changes invalidate it.
"""
import hashlib
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
GUEST_FLAGS = ('-target', 'x86_64-linux-gnu.2.39', '-O1', '-UNDEBUG', '-march=x86_64_v2', '-ffp-contract=off',
               '-fno-strict-aliasing', '-fvisibility=hidden', '-fPIC', '-pthread')
FIXED = ('runtime/cpu.h', 'runtime/simd.h', 'runtime/simd_fp.h', 'runtime/gamelib.h', 'runtime/rt.h', 'runtime/hle.h', 'runtime/platform.h',
         'runtime/host/gamelib_entry_linux.c', 'tools/gen_all.py', 'games/xcde/profile.json',
         'games/xcde/patches.py', 'games/xcde/hle_symbols.txt', 'requirements-linux.lock',
         'scripts/gamecode_identity.py')


def identity(root=ROOT, compiler=None):
    root = Path(root)
    paths = set(FIXED)
    for directory in ('tools/lifter', 'tools/extract'):
        paths.update(p.relative_to(root).as_posix() for p in (root / directory).rglob('*.py'))
    for relative in paths:
        if (root / relative).is_symlink() or not (root / relative).is_file():
            raise ValueError('game-code ABI input must be an ordinary file: ' + relative)
    # Follow quoted project headers, including ABI headers added in the future.
    # System headers are supplied by the separately pinned compiler/sysroot.
    pending = [p for p in paths if p.endswith(('.h', '.c'))]
    while pending:
        relative = pending.pop()
        source = root / relative
        for name in re.findall(r'^\s*#\s*include\s+"([^"]+)"', source.read_text(), re.MULTILINE):
            target = (source.parent / name).resolve()
            if not target.is_relative_to(root.resolve()) or not target.is_file():
                raise ValueError('guest ABI header closure escaped or is missing: ' + name)
            item = target.relative_to(root).as_posix()
            if item not in paths:
                paths.add(item); pending.append(item)
    hashes = {}
    for name in sorted(paths):
        p = root / name
        if p.is_symlink() or not p.is_file():
            raise ValueError('game-code ABI input must be an ordinary file: ' + name)
        hashes[name] = hashlib.sha256(p.read_bytes()).hexdigest()
    body = {'schema': 1, 'abi': 1, 'platform': 'linux-x86_64-v2',
            'flags': list(GUEST_FLAGS), 'sources': hashes, 'compiler': compiler or {}}
    body['id'] = hashlib.sha256(json.dumps(body, sort_keys=True, separators=(',', ':')).encode()).hexdigest()
    return body


if __name__ == '__main__':
    print(json.dumps(identity(), indent=2))
