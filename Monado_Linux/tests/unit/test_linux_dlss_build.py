"""[XCDE] LINUX-017: optional build inputs and runtime companions stay bound."""
import hashlib
import importlib.util
from pathlib import Path
import tempfile
import threading
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location('build_linux_dlss_test', ROOT / 'scripts/build_linux.py')
BUILD = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(BUILD)


class DlssBuildTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.output = self.root / 'build/audit'
        self.output.mkdir(parents=True)
        self.patch = mock.patch.object(BUILD, 'ROOT', self.root)
        self.patch.start()
        self.addCleanup(self.patch.stop)

    def builder(self):
        b = BUILD.Builder.__new__(BUILD.Builder)
        b.output = self.output
        b.failed = threading.Event()
        b.lock = threading.Lock()
        b.receipt = {'sources_before': {}, 'generated': {}, 'errors': [], 'dependencies': {}}
        return b

    def test_default_source_list_needs_no_native_sdk_provider(self):
        provider = self.root / 'runtime/nvn/vulkan/dlss_linux.cpp'
        bridge = self.root / 'runtime/nvn/vulkan/dlss_ngx_loader.cpp'
        self.assertNotIn(provider, BUILD.authored_sources())
        self.assertIn(provider, BUILD.authored_sources(with_dlss=True))
        self.assertNotIn(bridge, BUILD.authored_sources(with_dlss=True))

    def test_companion_copy_is_identical_and_receipted(self):
        source = self.root / 'source.so'
        source.write_bytes(b'synthetic library bytes, never executable')
        expected = BUILD.digest(source)
        b = self.builder()
        b.copy_runtime_resource(source, 'runtime.so', expected)
        self.assertEqual((self.output / 'runtime.so').read_bytes(), source.read_bytes())
        self.assertEqual(b.receipt['runtime_resources']['runtime.so']['sha256'], expected)

    def test_stale_source_or_existing_companion_is_preserved(self):
        source = self.root / 'source.so'
        source.write_bytes(b'new source')
        b = self.builder()
        with self.assertRaisesRegex(ValueError, 'verified source'):
            b.copy_runtime_resource(source, 'runtime.so', '0' * 64)
        existing = self.output / 'runtime.so'
        existing.write_bytes(b'accepted previous bytes')
        with self.assertRaises(FileExistsError):
            b.copy_runtime_resource(source, existing.name, BUILD.digest(source))
        self.assertEqual(existing.read_bytes(), b'accepted previous bytes')

    def test_symlink_source_and_destination_traversal_are_rejected(self):
        source = self.root / 'source.so'
        source.write_bytes(b'unchanged')
        link = self.root / 'link.so'
        link.symlink_to(source)
        b = self.builder()
        with self.assertRaises(ValueError):
            b.copy_runtime_resource(link, 'runtime.so', BUILD.digest(source))
        with self.assertRaises(ValueError):
            b.copy_runtime_resource(source, '../outside.so', BUILD.digest(source))
        self.assertFalse((self.output.parent / 'outside.so').exists())

    def test_changed_or_missing_companion_cannot_pass_build(self):
        for missing in (False, True):
            with self.subTest(missing=missing):
                companion = self.output / 'runtime.so'
                companion.write_bytes(b'verified companion')
                b = self.builder()
                b.receipt['runtime_resources'] = {companion.name: {
                    'path': str(companion), 'sha256': BUILD.digest(companion)}}
                if missing:
                    companion.unlink()
                else:
                    companion.write_bytes(b'corrupted companion')
                with mock.patch.object(BUILD, 'source_snapshot', return_value={}):
                    self.assertFalse(b.finish())
                self.assertTrue(any('runtime resource/dependency changed' in e for e in b.receipt['errors']))

    def test_changed_gnu_runtime_cannot_pass_build(self):
        library = self.root / 'libstdc++.so.6'
        library.write_bytes(b'original GNU runtime')
        b = self.builder()
        b.receipt['dependencies']['linked_libraries'] = {
            'gnu_runtime': {'path': str(library), 'sha256': BUILD.digest(library)}}
        library.write_bytes(b'replaced GNU runtime')
        with mock.patch.object(BUILD, 'source_snapshot', return_value={}):
            self.assertFalse(b.finish())

    def test_snapshot_binds_dlss_lock_setup_and_optional_icon(self):
        for name in ('scripts/build_linux.py', 'graphics-deps-linux.lock.json',
                     'requirements-linux.lock', 'tools/gen_smaa_inc.py',
                     'dlss-linux.lock.json', 'scripts/setup_linux_dlss.py',
                     'fsr-linux.lock.json', 'scripts/setup_linux_fsr.py',
                     'scripts/linux_runtime_check.py', 'scripts/linux_runtime_graph.py'):
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('synthetic authored build input')
        before = BUILD.source_snapshot()
        lock = self.root / 'dlss-linux.lock.json'
        lock.write_text('changed pinned dependency')
        after = BUILD.source_snapshot()
        self.assertNotEqual(before['dlss-linux.lock.json'], after['dlss-linux.lock.json'])
        self.assertIn('scripts/setup_linux_dlss.py', after)
        icon = self.root / 'local_assets/monado-icon.png'
        icon.parent.mkdir()
        icon.write_bytes(b'local icon test bytes')
        self.assertEqual(BUILD.source_snapshot()['local_assets/monado-icon.png'],
                         hashlib.sha256(icon.read_bytes()).hexdigest())


if __name__ == '__main__':
    unittest.main()
