"""LINUX-099: native observer companion ABI/receipt transaction checks, no ELF execution."""
from pathlib import Path
import importlib.util
import os
import tempfile
import unittest
SPEC = importlib.util.spec_from_file_location('test_observer_builder', Path(__file__).resolve().parents[1] / 'Monado_Linux/scripts/setup_present_observer_linux.py')
helper = importlib.util.module_from_spec(SPEC); SPEC.loader.exec_module(helper)


class FakeBuilder:
    def __init__(self, output):
        self.output = output; (output / 'inc').mkdir(); self.cc = '/authored/nonexecuted-compiler'; self.flags = []
        self.receipt = {}; self.saved = False; self.bad_exports = False; self.bad_needed = False; self.fail = None
    def run(self, name, argv):
        if name == self.fail: return {'exit': 1}, ''
        if name == 'present-observer-module':
            (self.output / (helper.RESOURCE + '.partial')).write_bytes(b'authored-not-executed')
        if name == 'present-observer-exports': return {'exit': 0}, '\n'.join('0000 T ' + n for n in helper.EXPORTS + (['unexpected'] if self.bad_exports else []))
        if name == 'present-observer-dynamic': return {'exit': 0}, '(NEEDED) [libc.so.6]\n' + ('(NEEDED) [untrusted.so]\n' if self.bad_needed else '')
        return {'exit': 0}, ''
    def save(self): self.saved = True


class ObserverBuild(unittest.TestCase):
    def builder(self):
        temp = tempfile.TemporaryDirectory(prefix='monado-observer-build-'); self.addCleanup(temp.cleanup)
        return FakeBuilder(Path(temp.name))
    def test_exact_exports_dependencies_source_identity_and_companion_receipt(self):
        b = self.builder(); output = helper.build(b)
        self.assertEqual(output, b.output / helper.RESOURCE); self.assertTrue(b.saved)
        self.assertEqual(b.receipt['present_observer']['exported_symbols'], helper.EXPORTS)
        self.assertEqual(b.receipt['present_observer']['needed'], ['libc.so.6'])
        self.assertFalse(b.receipt['present_observer']['generated_classification'])
        self.assertFalse(b.receipt['present_observer']['library_executed'])
        self.assertEqual(b.receipt['runtime_resources'][helper.RESOURCE]['sha256'], helper.digest(output))
        self.assertEqual(len(b.receipt['present_observer']['sources']), 2)
    def test_failed_compile_abi_and_dependency_checks_never_publish_resource(self):
        for mode in ('compile', 'export', 'dependency'):
            with self.subTest(mode=mode):
                b = self.builder()
                if mode == 'compile': b.fail = 'present-observer-module'
                elif mode == 'export': b.bad_exports = True
                else: b.bad_needed = True
                with self.assertRaises(RuntimeError): helper.build(b)
                self.assertFalse(b.saved); self.assertFalse((b.output / helper.RESOURCE).exists())
                self.assertNotIn('runtime_resources', b.receipt)
    def test_existing_or_dangling_output_is_preserved(self):
        for suffix in ('', '.partial'):
            with self.subTest(suffix=suffix):
                b = self.builder(); target = b.output / 'missing'; path = b.output / (helper.RESOURCE + suffix)
                try:
                    path.symlink_to(target)
                except OSError as error:
                    if os.name == 'nt' and error.winerror == 1314:
                        self.skipTest('Windows symbolic-link privilege unavailable for Linux observer fixture')
                    raise
                with self.assertRaises(ValueError): helper.build(b)
                self.assertTrue(path.is_symlink()); self.assertFalse(target.exists()); self.assertFalse(b.saved)


if __name__ == '__main__': unittest.main()
