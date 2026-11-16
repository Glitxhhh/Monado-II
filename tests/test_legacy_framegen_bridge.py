"""[XCDE] LINUX-111: truthful no-DLSS receipts for pre-FSR updater clients."""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('_monado_framegen_bridge_builder', ROOT / 'tools/build_linux_app.py')
build = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build)


class LegacyFeatureBridge(unittest.TestCase):
    def test_fresh_no_dlss_fsr3_keeps_modern_provider_default(self):
        with mock.patch.object(build, 'build') as operation:
            self.assertEqual(build.main(['--without-dlss']), 0)
        flags = operation.call_args.kwargs
        self.assertFalse(flags['dlss'])
        self.assertTrue(flags['fsr3'])
        self.assertTrue(flags['framegen'])

    def test_managed_implicit_no_dlss_preserves_checked_false_and_true(self):
        for inherited in (False, True):
            with self.subTest(inherited=inherited), \
                    mock.patch.object(build, 'managed_framegen_selection', return_value=inherited) as checked, \
                    mock.patch.object(build, 'build') as operation:
                self.assertEqual(build.main(['--without-dlss']), 0)
            checked.assert_called_once_with(build.ROOT)
            self.assertEqual(operation.call_args.kwargs['framegen'], inherited)

    def test_explicit_modern_provider_never_uses_inherited_default(self):
        with mock.patch.object(build, 'managed_framegen_selection', side_effect=AssertionError('explicit flag wins')), \
                mock.patch.object(build, 'build') as operation:
            self.assertEqual(build.main(['--without-dlss', '--framegen']), 0)
        self.assertTrue(operation.call_args.kwargs['framegen'])

    def test_modern_fsr_only_explicit_provider_and_nvidia_defaults(self):
        for args, dlss in [(['--without-dlss', '--framegen'], False), ([], True)]:
            with self.subTest(args=args), mock.patch.object(build, 'build') as operation:
                self.assertEqual(build.main(args), 0)
            self.assertEqual(operation.call_args.kwargs['dlss'], dlss)
            self.assertTrue(operation.call_args.kwargs['framegen'])

    def test_api_defaults_and_explicit_fsr_provider_remain_distinct(self):
        self.assertTrue(build.validate_features(dlss=False, fsr3=True, framegen=None))
        self.assertTrue(build.validate_features(dlss=False, fsr3=True, framegen=True))
        self.assertTrue(build.validate_features(dlss=True, fsr3=True, framegen=None))
        with self.assertRaises(build.BuildError):
            build.validate_features(dlss=False, fsr3=False, framegen=True)

    def test_gui_reuse_inherits_actual_false_and_true_capabilities(self):
        with tempfile.TemporaryDirectory(prefix='monado framegen receipt ') as temporary:
            game = Path(temporary).resolve() / 'xcde-linux'
            for dlss, framegen in [(False, False), (False, True), (True, False), (True, True)]:
                receipt = {'dlss_enabled': dlss, 'fsr3_enabled': True,
                           'framegen_enabled': framegen}
                (game.parent / 'receipt.json').write_text(json.dumps(receipt))
                args = ['--reuse-game', str(game), *([] if dlss else ['--without-dlss'])]
                with self.subTest(dlss=dlss, framegen=framegen), \
                        mock.patch.object(build, 'build') as operation:
                    self.assertEqual(build.main(args), 0)
                self.assertEqual(operation.call_args.kwargs['framegen'], framegen)
                self.assertTrue(operation.call_args.kwargs['fsr3'])

    def test_reuse_missing_or_malformed_capability_fails_before_build(self):
        with tempfile.TemporaryDirectory(prefix='monado malformed receipt ') as temporary:
            game = Path(temporary).resolve() / 'xcde-linux'
            for value in [None, 'false', 0, 1]:
                receipt = {'dlss_enabled': False, 'fsr3_enabled': True}
                if value is not None:
                    receipt['framegen_enabled'] = value
                (game.parent / 'receipt.json').write_text(json.dumps(receipt))
                with self.subTest(value=value), mock.patch.object(build, 'build') as operation, \
                        contextlib.redirect_stderr(io.StringIO()):
                    self.assertEqual(build.main(['--without-dlss', '--reuse-game', str(game)]), 1)
                operation.assert_not_called()


if __name__ == '__main__':
    unittest.main()
