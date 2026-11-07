"""[XCDE] LINUX-017: dependency setup fails closed without vendor execution."""
import contextlib
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock


SCRIPT = Path(__file__).resolve().parents[2] / 'scripts/setup_linux_dlss.py'
SPEC = importlib.util.spec_from_file_location('setup_linux_dlss', SCRIPT)
SETUP = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SETUP)


class DlssSetupTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.patch = mock.patch.object(SETUP, 'ROOT', self.root)
        self.patch.start()
        self.addCleanup(self.patch.stop)
        self.commit = 'a' * 40
        self.cache = self.root / '.deps/dlss/cache' / self.commit
        files = []
        for name in sorted(SETUP.EXPECTED_FILES):
            data = ('synthetic setup fixture: ' + name).encode()
            target = self.cache / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
            files.append({'path': name, 'url': SETUP.ORIGIN + self.commit + '/' + name,
                          'size': len(data), 'sha256': hashlib.sha256(data).hexdigest()})
        self.lock = {'schema': 1, 'source': {'repository': 'https://github.com/NVIDIA/DLSS',
                     'tag': 'v310.9.1', 'commit': self.commit},
                     'files': files, 'license': {'path': 'LICENSE.txt'}, 'runtime_policy': {}}

    def run_setup(self):
        with contextlib.redirect_stdout(io.StringIO()):
            return SETUP.setup(json.dumps(self.lock).encode(), offline=True)

    def test_offline_install_and_repeat_reverify_without_network(self):
        with mock.patch.object(SETUP.urllib.request, 'build_opener', side_effect=AssertionError('network')):
            target = self.run_setup()
            self.assertEqual(target, self.run_setup())
        self.assertEqual((self.root / '.deps/dlss/active-sdk.txt').read_text().strip(), str(target))
        receipt = json.loads((target / '.monado-dlss.json').read_text())
        self.assertFalse(receipt['vendor_code_executed_by_setup'])
        self.assertEqual(len(receipt['files']), 10)

    def test_missing_offline_file_is_not_downloaded(self):
        (self.cache / 'LICENSE.txt').unlink()
        with mock.patch.object(SETUP.urllib.request, 'build_opener', side_effect=AssertionError('network')):
            with self.assertRaises(FileNotFoundError):
                self.run_setup()

    def test_corrupt_cache_is_preserved_and_rejected(self):
        target = self.cache / 'LICENSE.txt'
        target.write_bytes(b'changed')
        with self.assertRaisesRegex(ValueError, 'hash/size mismatch'):
            self.run_setup()
        self.assertEqual(target.read_bytes(), b'changed')

    def test_existing_install_is_rehashed(self):
        target = self.run_setup() / 'include/nvsdk_ngx.h'
        target.write_bytes(b'changed')
        with self.assertRaisesRegex(ValueError, 'hash/size mismatch'):
            self.run_setup()
        self.assertEqual(target.read_bytes(), b'changed')

    def test_symlink_destination_is_rejected_before_cache_mutation(self):
        base = self.root / '.deps/dlss'
        outside = self.root / 'outside'
        outside.mkdir()
        (base / 'sdk').symlink_to(outside, target_is_directory=True)
        with self.assertRaisesRegex(RuntimeError, 'symlink'):
            self.run_setup()
        self.assertEqual(list(outside.iterdir()), [])

    def test_symlink_cached_file_is_rejected(self):
        target = self.cache / 'LICENSE.txt'
        target.unlink()
        outside = self.root / 'outside'
        outside.write_bytes(b'untouched')
        target.symlink_to(outside)
        with self.assertRaisesRegex(RuntimeError, 'symlink'):
            self.run_setup()
        self.assertEqual(outside.read_bytes(), b'untouched')

    def test_unrecognized_existing_target_is_preserved(self):
        identity = hashlib.sha256(json.dumps(self.lock).encode()).hexdigest()
        target = self.root / '.deps/dlss/sdk' / identity[:16]
        target.mkdir(parents=True)
        (target / 'owned-by-someone-else').write_text('keep')
        with self.assertRaisesRegex(RuntimeError, 'unknown identity'):
            self.run_setup()
        self.assertEqual((target / 'owned-by-someone-else').read_text(), 'keep')

    def test_untrusted_url_traversal_and_duplicate_are_rejected(self):
        for field, value in [('url', 'https://example.com/library'), ('path', '../escape')]:
            with self.subTest(field=field):
                lock = json.loads(json.dumps(self.lock))
                lock['files'][0][field] = value
                with self.assertRaises(ValueError):
                    SETUP.validate_lock(lock)
        self.lock['files'].append(self.lock['files'][0])
        with self.assertRaisesRegex(ValueError, 'duplicates'):
            SETUP.validate_lock(self.lock)

    def test_unknown_active_target_is_preserved(self):
        active = self.root / '.deps/dlss/active-sdk.txt'
        active.write_text('/tmp/foreign-sdk\n')
        with self.assertRaisesRegex(RuntimeError, 'outside the managed'):
            self.run_setup()
        self.assertEqual(active.read_text(), '/tmp/foreign-sdk\n')


if __name__ == '__main__':
    unittest.main()
