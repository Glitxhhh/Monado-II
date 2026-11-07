"""[XCDE] LINUX-017: local vendor/resource identity and traversal regressions."""
import hashlib
import importlib.util
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('dlss_run_linux', ROOT / 'scripts/run_linux.py')
run = importlib.util.module_from_spec(spec)
spec.loader.exec_module(run)


class Resources(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.binary = self.root / 'xcde-linux'
        self.resource = self.root / 'libmonado-ngx.so'
        self.resource.write_bytes(b'authored synthetic module identity')
        self.entry = {'path': str(self.resource), 'bytes': self.resource.stat().st_size,
                      'sha256': hashlib.sha256(self.resource.read_bytes()).hexdigest()}

    def check(self, name='libmonado-ngx.so'):
        run.verify_runtime_resources(self.binary, {'runtime_resources': {name: self.entry}})

    def test_valid(self):
        self.check()

    def test_legacy_receipt(self):
        run.verify_runtime_resources(self.binary, {})

    def test_missing(self):
        self.resource.unlink()
        with self.assertRaises(ValueError): self.check()

    def test_same_size_change(self):
        self.resource.write_bytes(b'x' * self.entry['bytes'])
        with self.assertRaises(ValueError): self.check()

    def test_size_change(self):
        self.resource.write_bytes(b'x')
        with self.assertRaises(ValueError): self.check()

    def test_symlink(self):
        target = self.root / 'other'; self.resource.rename(target)
        self.resource.symlink_to(target)
        with self.assertRaises(ValueError): self.check()

    def test_path_relocation(self):
        self.entry['path'] = str(self.root / 'other')
        with self.assertRaises(ValueError): self.check()

    def test_traversal(self):
        with self.assertRaises(ValueError): self.check('../libmonado-ngx.so')

    def test_absolute(self):
        with self.assertRaises(ValueError): self.check(str(self.resource))


if __name__ == '__main__':
    unittest.main()
