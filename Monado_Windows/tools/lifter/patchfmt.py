"""Patch format for the generator (contract: docs/contracts/patch-format.md).  [XCDE]

A patch replaces one guest instruction by hand-written C. It must name the original instruction word it expects, so
a patch written for one build can never be applied silently to another, and it must cite a requirement or task.
"""
import importlib.util
import os


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


def load_patches(path):
    """Load PATCHES = {addr: Patch} from a python file; a missing file means no patches."""
    if not path or not os.path.exists(path):
        return {}
    spec = importlib.util.spec_from_file_location('xcde_patches', path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    patches = getattr(mod, 'PATCHES', {})
    for a, p in patches.items():
        if a % 4:
            raise ValueError(f'patch address {a:#x} is not 4-byte aligned')
        if not isinstance(p, Patch):
            raise ValueError(f'patch at {a:#x} is not a Patch')
    return patches
