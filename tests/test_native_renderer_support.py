"""Configured native fixture prerequisites fail closed; no tool or GPU execution."""
from pathlib import Path
import json
import os
import tempfile
import unittest
from unittest import mock

import native_renderer_support as support


class NativeFixtureAvailability(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='monado-native-availability-')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.zig = self.root / 'authored-zig.exe'
        for patcher in (mock.patch.dict(os.environ, {}, clear=True),
                        mock.patch.object(support, 'PORT', self.root),
                        mock.patch.object(support, 'WINDOWS', True),
                        mock.patch.object(support, '_zig_candidates', return_value=(self.zig,))):
            patcher.start()
            self.addCleanup(patcher.stop)

    def config(self, data=None):
        path = self.root / '.deps/windows-deps.json'
        path.parent.mkdir(exist_ok=True)
        path.write_text(json.dumps(data or {'prefix': str(self.root / 'graphics')}))
        return path

    def test_wholly_absent_source_ci_setup_skips(self):
        self.assertIsNone(support.compiler_command())
        with self.assertRaises(unittest.SkipTest):
            support.require_native_graphics()
        with self.assertRaises(unittest.SkipTest):
            support.require_pinned_zig()
        self.assertEqual(list(self.root.iterdir()), [])

    def test_explicit_graphics_without_compiler_fails(self):
        with mock.patch.dict(os.environ, {'MONADO_TEST_GRAPHICS_PREFIX': str(self.root)}):
            with self.assertRaisesRegex(AssertionError, 'missing.*compiler'):
                support.require_native_graphics()
            with self.assertRaisesRegex(AssertionError, 'missing.*compiler'):
                support.require_pinned_zig()

    def test_receipted_partial_setup_fails(self):
        self.config()
        with self.assertRaisesRegex(AssertionError, 'missing.*compiler'):
            support.compiler_command()
        with self.assertRaisesRegex(AssertionError, 'missing.*compiler'):
            support.require_native_graphics()

    def test_malformed_config_is_not_skipped_when_compiler_absent(self):
        self.config().write_text('{invalid authored metadata')
        with self.assertRaises(ValueError):
            support.require_native_graphics()

    def test_installed_compiler_without_dependency_metadata_fails(self):
        self.zig.write_bytes(b'authored compiler identity; never executed')
        with self.assertRaisesRegex(AssertionError, 'missing graphics dependency metadata'):
            support.require_native_graphics()

    def test_configured_missing_graphics_directory_fails(self):
        self.config()
        self.zig.write_bytes(b'authored compiler identity; never executed')
        with self.assertRaisesRegex(AssertionError, 'prefix is missing'):
            support.require_native_graphics()
        with mock.patch.dict(os.environ, {'MONADO_TEST_GRAPHICS_PREFIX': 'relative-authored-prefix'}):
            with self.assertRaisesRegex(AssertionError, 'must be absolute'):
                support.require_native_graphics()

    def test_explicit_or_malformed_compiler_is_not_an_absence_skip(self):
        with mock.patch.dict(os.environ, {'MONADO_NATIVE_TEST_COMPILER': str(self.zig)}):
            with self.assertRaises(FileNotFoundError):
                support.require_pinned_zig()
        self.zig.mkdir()
        with self.assertRaisesRegex(AssertionError, 'not an ordinary file'):
            support.require_pinned_zig()

    def test_fsr4_default_builder_requires_actual_default_paths(self):
        prefix = self.root / 'graphics'
        prefix.mkdir()
        self.zig.write_bytes(b'authored compiler identity; never executed')
        with mock.patch.dict(os.environ, {'MONADO_TEST_GRAPHICS_PREFIX': str(prefix)}):
            self.assertEqual(support.require_native_graphics(), prefix)
            with self.assertRaisesRegex(AssertionError, 'Production native FSR4.*incomplete'):
                support.require_native_graphics(production_defaults=True)
            default = self.root / '.venv/Lib/site-packages/ziglang/zig.exe'
            default.parent.mkdir(parents=True)
            default.write_bytes(b'authored default compiler; never executed')
            with self.assertRaisesRegex(AssertionError, 'Production native FSR4.*incomplete'):
                support.require_native_graphics(production_defaults=True)
            self.config({'prefix': str(prefix)})
            self.assertEqual(support.require_native_graphics(production_defaults=True), prefix)


if __name__ == '__main__':
    unittest.main()
