"""[XCDE] LINUX-008: launcher artifacts must not accept stale/invalid output."""
import importlib.util
from pathlib import Path
import sys
import tempfile
import threading
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
try:
    spec = importlib.util.spec_from_file_location('build_launcher_linux', ROOT / 'scripts/build_launcher_linux.py')
    build = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(build)
finally:
    sys.path.pop(0)


@unittest.skipUnless(sys.platform == 'linux', 'Linux launcher build safety')
class LauncherBuildSafety(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory(prefix='monado-launcher-build-')
        self.addCleanup(tmp.cleanup)
        self.out = Path(tmp.name)

    def builder(self):
        b = build.LauncherBuilder.__new__(build.LauncherBuilder)
        b.output = self.out
        b.lock = threading.Lock()
        b.failed = threading.Event()
        b.cxx = Path('/unused/compiler')
        b.libdir = self.out / 'lib'
        b.library_path = str(b.libdir)
        b.sysroot = self.out
        b.runtime_policy = None
        b.libs = ['-lSDL3']
        b.receipt = {'sources_before': {'launcher.h': 'before'}, 'objects': {}, 'errors': []}
        return b

    def link_command(self, *, architecture=62, stack='RW', loader='libSDL3.so.0 => /local/libSDL3.so.0'):
        def command(name, argv, **kwargs):
            if name == 'link-launcher':
                header = bytearray(64)
                header[:6] = b'\x7fELF\x02\x01'
                header[18:20] = architecture.to_bytes(2, 'little')
                Path(argv[-1]).write_bytes(header)
            if name == 'elf-segments':
                return ' GNU_STACK 0 0 0 0 0 ' + stack + ' 0x10\n'
            if name == 'elf-loader-list':
                return loader
            return ''
        return command

    def test_local_icon_is_optional_and_copied_without_pixel_changes(self):
        b = self.builder()
        with mock.patch.object(build, 'ROOT', self.out / 'source'):
            b.install_icon()
            self.assertNotIn('presentation_icon', b.receipt)
            icon = build.ROOT / 'local_assets/monado-icon.png'
            icon.parent.mkdir(parents=True)
            data = (b'\x89PNG\r\n\x1a\n\x00\x00\x00\rIHDR' +
                    (2).to_bytes(4, 'big') * 2 + b'\x08\x06\x00\x00\x00' + b'crc!')
            icon.write_bytes(data)
            b.receipt['sources_before']['local_assets/monado-icon.png'] = build.digest(icon)
            b.install_icon()
            self.assertEqual((self.out / 'icon.png').read_bytes(), data)
            self.assertEqual(b.receipt['presentation_icon']['sha256'], build.digest(icon))
            (self.out / 'icon.png').write_bytes(b'changed icon')
            with mock.patch.object(build, 'source_snapshot', return_value=b.receipt['sources_before']):
                self.assertFalse(b.finish())
            self.assertIn('launcher window icon changed during build', b.receipt['errors'])

    def test_source_drift_rejects_otherwise_successful_link(self):
        b = self.builder()
        with mock.patch.object(build, 'source_snapshot', return_value={'launcher.h': 'after'}):
            self.assertFalse(b.finish())
        self.assertEqual(b.receipt['status'], 'failed')
        self.assertTrue(b.receipt['sources_changed_during_build'])
        self.assertNotIn('executable', b.receipt)

    def test_compiler_failure_cannot_be_overridden_by_stable_sources(self):
        b = self.builder()
        b.failed.set()
        with mock.patch.object(build, 'source_snapshot', return_value=b.receipt['sources_before']):
            self.assertFalse(b.finish())
        self.assertEqual(b.receipt['status'], 'failed')

    def test_wrong_architecture_rejected(self):
        b = self.builder()
        b.checked_run = self.link_command(architecture=183)
        with self.assertRaisesRegex(RuntimeError, 'x86-64 ELF'):
            b.link()
        self.assertNotIn('executable', b.receipt)

    def test_executable_stack_rejected(self):
        b = self.builder()
        b.checked_run = self.link_command(stack='RWE')
        with self.assertRaisesRegex(RuntimeError, 'nonexecutable stack'):
            b.link()

    def test_missing_transitive_library_rejected(self):
        b = self.builder()
        b.checked_run = self.link_command(loader='libSDL3.so.0 => not found')
        with self.assertRaisesRegex(RuntimeError, 'resolution is incomplete'):
            b.link()

    def test_native_runtime_guard_rejects_postlink_dependency_change(self):
        b = self.builder()
        b.checked_run = self.link_command()
        with mock.patch.object(build.runtime_check, 'check_artifacts', side_effect=ValueError('authored missing runtime ABI')):
            with self.assertRaisesRegex(ValueError, 'missing runtime ABI'):
                b.link()
        self.assertNotIn('executable', b.receipt)

    def test_pkg_config_cannot_select_another_sysroot(self):
        b = self.builder()
        b.cc = b.cxx
        b.pkg = Path('/unused/pkg-config')
        b.sysroot = self.out / 'selected-sdk'
        b.checked_run = mock.Mock(side_effect=['compiler version', 'compiler version', '/other-sdk\n'])
        with self.assertRaisesRegex(RuntimeError, 'sysroot differ'):
            b.setup()
        self.assertNotIn('compile_flags', b.receipt)

    def test_changed_compiler_wrapper_rejects_final_artifact(self):
        b = self.builder()
        b.sysroot = self.out
        marker = self.out / '.monado-deps.json'
        marker.write_text('{}\n')
        active = self.out / '.deps/graphics/active-sysroot.txt'
        active.parent.mkdir(parents=True)
        active.write_text(str(self.out) + '\n')
        compiler = self.out / 'compiler'
        compiler.write_text('original compiler wrapper\n')
        b.receipt['dependencies'] = {'compiler_wrappers': {'compiler': build.digest(compiler)},
            'sysroot_marker_sha256': build.digest(marker),
            'active_sysroot_file_sha256': build.digest(active)}
        compiler.write_text('replacement compiler wrapper\n')
        with mock.patch.object(build, 'ROOT', self.out), \
             mock.patch.object(build, 'source_snapshot', return_value=b.receipt['sources_before']):
            self.assertFalse(b.finish())
        self.assertTrue(any('dependency identity changed' in e for e in b.receipt['errors']))

    def test_new_launcher_header_changes_source_identity(self):
        with mock.patch.object(build, 'ROOT', self.out):
            for name in [*build.CPP, *build.C, 'scripts/build_launcher_linux.py',
                         'scripts/build_linux.py', 'scripts/linux_runtime_check.py', 'scripts/linux_runtime_graph.py',
                         'graphics-deps-linux.lock.json', 'requirements-linux.lock',
                         'tools/launcher/theme_resource.py']:
                p = self.out / name
                p.parent.mkdir(parents=True, exist_ok=True)
                p.write_text('// synthetic authored input\n')
            before = build.source_snapshot()
            (self.out / 'tools/launcher/linux_host.h').write_text('// added platform behavior\n')
            after = build.source_snapshot()
            assets = self.out / 'tools/launcher/assets'
            (assets / 'fonts').mkdir(parents=True)
            (assets / 'gaur_plain.png').write_bytes(b'synthetic artwork identity')
            (assets / 'fonts/LICENSE-DejaVu.txt').write_text('retained redistribution licence')
            with_assets = build.source_snapshot()
        self.assertNotEqual(before, after)
        self.assertIn('tools/launcher/linux_host.h', after)
        self.assertEqual(with_assets['tools/launcher/assets/gaur_plain.png'],
                         build.digest(assets / 'gaur_plain.png'))
        self.assertEqual(with_assets['tools/launcher/assets/fonts/LICENSE-DejaVu.txt'],
                         build.digest(assets / 'fonts/LICENSE-DejaVu.txt'))

    def test_changed_generated_artwork_rejects_final_artifact(self):
        b = self.builder()
        generated = self.out / 'inc/launcher_art.inc'
        generated.parent.mkdir()
        generated.write_text('// embedded original artwork\n')
        b.receipt['presentation_resources'] = {'generated': {
            generated.name: {'sha256': build.digest(generated)}}}
        generated.write_text('// different artwork after compilation\n')
        with mock.patch.object(build, 'source_snapshot', return_value=b.receipt['sources_before']):
            self.assertFalse(b.finish())
        self.assertTrue(any('generated launcher resource changed' in e for e in b.receipt['errors']))

    def test_missing_generated_font_rejects_final_artifact(self):
        b = self.builder()
        b.receipt['presentation_resources'] = {'generated': {
            'launcher_fonts.inc': {'sha256': 'absent'}}}
        with mock.patch.object(build, 'source_snapshot', return_value=b.receipt['sources_before']):
            self.assertFalse(b.finish())
        self.assertTrue(any('launcher_fonts.inc' in e for e in b.receipt['errors']))

    def test_stable_generated_resource_accepts_final_artifact(self):
        b = self.builder()
        generated = self.out / 'inc/launcher_art.inc'
        generated.parent.mkdir()
        generated.write_text('// stable embedded artwork\n')
        b.receipt['presentation_resources'] = {'generated': {
            generated.name: {'sha256': build.digest(generated)}}}
        with mock.patch.object(build, 'source_snapshot', return_value=b.receipt['sources_before']):
            self.assertTrue(b.finish())
        self.assertEqual(b.receipt['status'], 'passed')


if __name__ == '__main__':
    unittest.main()
