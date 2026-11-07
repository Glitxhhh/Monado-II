"""[XCDE] LINUX-002: local dependency setup must reject managed-path symlinks."""
import contextlib
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import stat
import sys
import tempfile
import unittest
from unittest import mock


SOURCE_ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('setup_linux_graphics', SOURCE_ROOT / 'scripts/setup_linux_graphics.py')
setup = importlib.util.module_from_spec(spec)
spec.loader.exec_module(setup)


@unittest.skipUnless(sys.platform == 'linux', 'Linux dependency setup path tests')
class TestGraphicsSetupPaths(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='monado-deps-paths-')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name) / 'linux'
        self.outside = Path(temporary.name) / 'outside'
        self.root.mkdir()
        self.outside.mkdir()
        (self.outside / 'sentinel').write_bytes(b'preserve outside file\n')
        self.lock = self.root / 'graphics-deps-linux.lock.json'
        self.write_lock([])
        patcher = mock.patch.multiple(setup, ROOT=self.root, LOCK=self.lock)
        patcher.start()
        self.addCleanup(patcher.stop)
        # All fixtures are offline, with no archive extraction or network calls.
        for owner, name in ((setup.urllib.request, 'urlopen'), (setup.subprocess, 'run'), (setup.subprocess, 'Popen')):
            patcher = mock.patch.object(owner, name, side_effect=AssertionError('unexpected network/extraction'))
            patcher.start()
            self.addCleanup(patcher.stop)

    def write_lock(self, packages):
        data = json.dumps({'schema': 1, 'packages': packages}).encode()
        self.lock.write_bytes(data)
        self.identity = hashlib.sha256(data).hexdigest()
        self.target = self.root / '.deps/graphics/sysroots' / self.identity[:16]

    def run_setup(self):
        with mock.patch.object(sys, 'argv', ['setup_linux_graphics.py', '--offline']), contextlib.redirect_stdout(io.StringIO()):
            setup.main()

    def outside_snapshot(self):
        return {str(p.relative_to(self.outside)): (p.read_bytes(), stat.S_IMODE(p.stat().st_mode))
                for p in self.outside.rglob('*') if p.is_file()}

    def assert_rejected_link(self, path, *, directory=False, dangling=False):
        path.parent.mkdir(parents=True, exist_ok=True)
        destination = self.outside / 'absent' if dangling else self.outside if directory else self.outside / 'sentinel'
        path.symlink_to(destination, target_is_directory=directory)
        before_files = self.outside_snapshot()
        before_paths = set(self.outside.rglob('*'))
        before_managed = set(self.root.rglob('*'))
        with self.assertRaisesRegex(RuntimeError, 'symlink'):
            self.run_setup()
        self.assertEqual(self.outside_snapshot(), before_files)
        self.assertEqual(set(self.outside.rglob('*')), before_paths)
        self.assertEqual(set(self.root.rglob('*')), before_managed, 'preflight must precede setup mutations')
        self.assertTrue(path.is_symlink())

    def test_deps_parent_symlink_rejected(self):
        self.assert_rejected_link(self.root / '.deps', directory=True)

    def test_dangling_deps_parent_symlink_rejected(self):
        self.assert_rejected_link(self.root / '.deps', directory=True, dangling=True)

    def test_graphics_parent_symlink_rejected(self):
        self.assert_rejected_link(self.root / '.deps/graphics', directory=True)

    def test_cache_directory_symlink_rejected(self):
        self.assert_rejected_link(self.root / '.deps/graphics/debs', directory=True)

    def test_sysroot_parent_symlink_rejected(self):
        self.assert_rejected_link(self.root / '.deps/graphics/sysroots', directory=True)

    def test_sysroot_target_symlink_rejected(self):
        self.assert_rejected_link(self.target, directory=True)

    def test_identity_marker_symlink_rejected(self):
        self.assert_rejected_link(self.target / '.monado-deps.json')

    def test_bin_directory_symlink_rejected(self):
        self.assert_rejected_link(self.root / '.deps/bin', directory=True)

    def test_wrapper_symlink_rejected(self):
        self.assert_rejected_link(self.root / '.deps/bin/pkg-config-graphics')

    def test_dangling_wrapper_symlink_rejected(self):
        self.assert_rejected_link(self.root / '.deps/bin/pkg-config-graphics', dangling=True)

    def test_active_sysroot_symlink_rejected(self):
        self.assert_rejected_link(self.root / '.deps/graphics/active-sysroot.txt')

    def test_cached_archive_symlink_rejected_before_any_setup_mutation(self):
        data = b'synthetic cached fixture'
        self.write_lock([{'filename': 'fixture.deb', 'size': len(data), 'sha256': hashlib.sha256(data).hexdigest()}])
        self.assert_rejected_link(self.root / '.deps/graphics/debs/fixture.deb')

    def test_dangling_cached_archive_symlink_rejected(self):
        self.write_lock([{'filename': 'fixture.deb', 'size': 1, 'sha256': '0' * 64}])
        self.assert_rejected_link(self.root / '.deps/graphics/debs/fixture.deb', dangling=True)

    def test_empty_offline_setup_and_existing_local_directories_work(self):
        self.run_setup()
        self.run_setup()
        self.assertEqual((self.root / '.deps/graphics/active-sysroot.txt').read_text(), str(self.target) + '\n')
        wrapper = self.root / '.deps/bin/pkg-config-graphics'
        self.assertIn(str(self.target), wrapper.read_text())
        self.assertEqual(stat.S_IMODE(wrapper.stat().st_mode), 0o755)

    def test_valid_cache_and_library_symlinks_are_accepted(self):
        data = b'synthetic cached fixture; never extracted'
        self.write_lock([{'filename': 'fixture.deb', 'size': len(data), 'sha256': hashlib.sha256(data).hexdigest()}])
        cache = self.root / '.deps/graphics/debs'
        cache.mkdir(parents=True)
        (cache / 'fixture.deb').write_bytes(data)
        self.target.mkdir(parents=True)
        (self.target / '.monado-deps.json').write_text(json.dumps({'lock_sha256': self.identity}))
        libraries = self.target / 'usr/lib/x86_64-linux-gnu'
        libraries.mkdir(parents=True)
        (libraries / 'libfixture.so.1').write_bytes(b'placeholder library')
        (libraries / 'libfixture.so').symlink_to('libfixture.so.1')
        self.run_setup()
        self.assertTrue((libraries / 'libfixture.so').is_symlink())
        self.assertEqual((libraries / 'libfixture.so').read_bytes(), b'placeholder library')
        self.assertEqual((cache / 'fixture.deb').read_bytes(), data)


if __name__ == '__main__':
    unittest.main()
