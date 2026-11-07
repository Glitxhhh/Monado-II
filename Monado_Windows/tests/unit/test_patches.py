"""T012 (tests first): patch format with expected original bytes (contract: docs/contracts/patch-format.md)."""
import os
import sys
import tempfile
import unittest

LIFTER = os.path.join(os.path.dirname(__file__), '..', '..', 'tools', 'lifter')
sys.path.insert(0, LIFTER)
import patchfmt  # noqa: E402


class FakeGen:
    def __init__(self, words):
        self._w = words

    def word(self, a):
        return self._w[a]


class TestPatch(unittest.TestCase):
    def test_apply_returns_emitted_code_when_bytes_match(self):
        p = patchfmt.Patch(expected=0xD503201F, emit=lambda gen, a: f'/* nop at {a:#x} */', req='FR-013', note='t')
        self.assertEqual(p.apply(FakeGen({0x100: 0xD503201F}), 0x100), '/* nop at 0x100 */')

    def test_mismatch_aborts_with_address_and_found_bytes(self):
        p = patchfmt.Patch(expected=0xD503201F, emit=lambda g, a: 'x', req='FR-013')
        with self.assertRaises(patchfmt.PatchMismatch) as cm:
            p.apply(FakeGen({0x100: 0x12345678}), 0x100)
        msg = str(cm.exception)
        self.assertIn('0x100', msg)
        self.assertIn('0x12345678', msg)
        self.assertIn('0xd503201f', msg.lower())

    def test_req_is_mandatory(self):
        with self.assertRaises(ValueError):
            patchfmt.Patch(expected=1, emit=lambda g, a: 'x', req='')

    def test_load_patches_from_file(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, 'patches.py')
            with open(path, 'w') as f:
                f.write("from patchfmt import Patch\n"
                        "PATCHES = {0x200: Patch(expected=7, emit=lambda g, a: 'ok', req='T012')}\n")
            loaded = patchfmt.load_patches(path)
            self.assertEqual(set(loaded), {0x200})
            self.assertEqual(loaded[0x200].apply(FakeGen({0x200: 7}), 0x200), 'ok')

    def test_load_patches_missing_file_is_empty(self):
        self.assertEqual(patchfmt.load_patches(os.path.join(tempfile.gettempdir(), 'nope_patches.py')), {})

    def test_duplicate_or_misaligned_addresses_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, 'patches.py')
            with open(path, 'w') as f:
                f.write("from patchfmt import Patch\n"
                        "PATCHES = {0x201: Patch(expected=7, emit=lambda g, a: 'ok', req='T012')}\n")
            with self.assertRaises(ValueError):
                patchfmt.load_patches(path)


if __name__ == '__main__':
    unittest.main()
