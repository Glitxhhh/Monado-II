"""Patch format for the generator (contract: docs/contracts/patch-format.md).  [XCDE]

A patch replaces one guest instruction by hand-written C. It must name the original instruction word it expects, so
a patch written for one build can never be applied silently to another, and it must cite a requirement or task.
"""
import importlib.util
import os
import re


class PatchMismatch(Exception):
    pass


class Patch:
    def __init__(self, expected, emit, req, note=''):
        if not req:
            raise ValueError('a patch must cite a requirement or task (req)')
        self.expected = expected
        self.emit = emit
        self.req = req
        self.note = note

    def apply(self, gen, addr):
        found = gen.word(addr)
        if found != self.expected:
            raise PatchMismatch(f'patch at {addr:#x}: expected word {self.expected:#010x}, found {found:#010x} '
                                f'(req {self.req})')
        return self.emit(gen, addr)


class FunctionEntries:
    """[XCDE] LINUX-041: checked static entries, without replacing instructions.

    A bounded native indirect branch can cross a generator partition boundary.
    Entry metadata keeps its legitimate destinations in the dispatch table. It
    must identify one module/build and guard both entry and dispatch words.
    """
    def __init__(self, module, build_id, entries, guards, req, note=''):
        if not req:
            raise ValueError('function entries must cite a requirement or task (req)')
        if not isinstance(module, str) or not re.fullmatch(r'[A-Za-z0-9_]+', module):
            raise ValueError('function entries need a module name')
        if not isinstance(build_id, str) or not re.fullmatch(r'(?:[0-9a-f]{2}){8,32}', build_id):
            raise ValueError('function entries need a 16..64 digit lowercase build ID')
        self.module, self.build_id, self.req, self.note = module, build_id, req, note
        self.entries = tuple(entries.items() if isinstance(entries, dict) else entries)
        self.guards = tuple(guards.items() if isinstance(guards, dict) else guards)
        if not self.entries or not self.guards:
            raise ValueError('function entries require entries and dispatch guards')
        seen = set()
        for address, word in self.entries + self.guards:
            if type(address) is not int or address < 0 or address % 4:
                raise ValueError('function entry/guard address must be aligned and nonnegative')
            if type(word) is not int or not 0 <= word <= 0xffffffff:
                raise ValueError('function entry/guard word must be a uint32')
            if address in seen:
                raise ValueError(f'duplicate function entry/guard at {address:#x}')
            seen.add(address)

    def apply(self, gen):
        if gen.name != self.module:
            return set()
        if not gen.nso.build_id.startswith(self.build_id):
            raise PatchMismatch(f'function entries for {self.module}: unexpected build ID '
                                f'{gen.nso.build_id} (req {self.req})')
        # Validate the whole set before returning any new partition boundary.
        for address, expected in self.entries + self.guards:
            if not gen.in_text(address):
                raise PatchMismatch(f'function entry/guard {address:#x} is outside module text '
                                    f'(req {self.req})')
            found = gen.word(address)
            if found != expected:
                raise PatchMismatch(f'function entry/guard at {address:#x}: expected word '
                                    f'{expected:#010x}, found {found:#010x} (req {self.req})')
        return {address for address, _ in self.entries}


def load_patch_set(path):
    """Load authored patches and optional checked entry sets once from a file."""
    if not path or not os.path.exists(path):
        return {}, ()
    spec = importlib.util.spec_from_file_location('xcde_patches', path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    patches = getattr(mod, 'PATCHES', {})
    for a, p in patches.items():
        if a % 4:
            raise ValueError(f'patch address {a:#x} is not 4-byte aligned')
        if not isinstance(p, Patch):
            raise ValueError(f'patch at {a:#x} is not a Patch')
    entries = tuple(getattr(mod, 'FUNCTION_ENTRIES', ()))
    if any(not isinstance(entry, FunctionEntries) for entry in entries):
        raise ValueError('FUNCTION_ENTRIES must contain FunctionEntries objects')
    return patches, entries


def load_patches(path):
    """Compatibility API for callers inspecting instruction patches only."""
    return load_patch_set(path)[0]
