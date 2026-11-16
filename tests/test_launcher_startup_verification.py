"""[XCDE] LINUX-111: startup deduplication never caches source/file trust."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

if sys.platform != 'linux':
    raise unittest.SkipTest('Linux installation startup verification')

ROOT = Path(__file__).resolve().parents[1]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


updater = load('_monado_startup_verification_updater', ROOT / 'tools/update_linux.py')
backend = load('_monado_startup_verification_backend', ROOT / 'tools/builder_backend.py')


def encoded(value):
    return json.dumps(value, sort_keys=True).encode()


class SourceVerdicts(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='monado startup verification ')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.content = {'README.md': b'Authored startup fixture.\n',
                        'publication-policy.json': encoded({'schema': 1, 'licensed_media': {}})}
        self.write_manifest()

    def write_manifest(self):
        for name, data in self.content.items():
            (self.root / name).write_bytes(data)
        (self.root / 'publication-manifest.json').write_bytes(encoded({
            'schema': 1, 'source_only': True, 'project': 'Monado II', 'files': {
                name: {'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
                for name, data in self.content.items()}}))

    def test_operation_reuses_pure_verdict_but_reads_every_source_again(self):
        with mock.patch.object(updater, 'content_problem', wraps=updater.content_problem) as scan, \
                mock.patch.object(updater, 'read_regular', wraps=updater.read_regular) as read:
            with updater.source_verification_context():
                first = updater.installed_sources(self.root)
                before = read.call_count
                self.assertEqual(first, updater.installed_sources(self.root))
                reread = {Path(call.args[0]) for call in read.call_args_list[before:]}
                for name in self.content:
                    self.assertIn(self.root / name, reread, 'file identities must never be cached')
            self.assertEqual(scan.call_count, len(self.content))
            # A new operation/lease must not inherit an earlier operation memo.
            with updater.source_verification_context():
                updater.installed_sources(self.root)
            self.assertEqual(scan.call_count, 2 * len(self.content))

    def test_no_context_keeps_full_existing_verification(self):
        with mock.patch.object(updater, 'content_problem', wraps=updater.content_problem) as scan:
            updater.installed_sources(self.root)
            updater.installed_sources(self.root)
        self.assertEqual(scan.call_count, 2 * len(self.content))

    def test_same_size_edit_with_restored_mtime_is_rejected(self):
        path = self.root / 'README.md'
        original = path.stat()
        with updater.source_verification_context():
            updater.installed_sources(self.root)
            replacement = b'X' * len(self.content['README.md'])
            path.write_bytes(replacement)
            os.utime(path, ns=(original.st_atime_ns, original.st_mtime_ns))
            with self.assertRaisesRegex(ValueError, 'Local source changes detected'):
                updater.installed_sources(self.root)

    def test_replaced_symlink_or_hardlink_is_rejected(self):
        path = self.root / 'README.md'
        external = self.root / 'authored-external.txt'
        external.write_bytes(self.content['README.md'])
        with updater.source_verification_context():
            updater.installed_sources(self.root)
            path.unlink()
            path.symlink_to(external)
            with self.assertRaises(ValueError):
                updater.installed_sources(self.root)
            path.unlink()
            os.link(external, path)
            with self.assertRaises(ValueError):
                updater.installed_sources(self.root)

    def test_policy_change_recalculates_even_for_unchanged_source_bytes(self):
        with mock.patch.object(updater, 'content_problem', wraps=updater.content_problem) as scan:
            with updater.source_verification_context():
                updater.installed_sources(self.root)
                self.content['publication-policy.json'] = encoded({
                    'schema': 1, 'licensed_media': {}, 'authored_new_policy_identity': True})
                self.write_manifest()
                updater.installed_sources(self.root)
        self.assertEqual(scan.call_count, 2 * len(self.content))

    def test_failed_operation_does_not_leave_a_cached_verdict(self):
        with self.assertRaisesRegex(RuntimeError, 'authored failure'):
            with updater.source_verification_context():
                updater.installed_sources(self.root)
                raise RuntimeError('authored failure')
        self.assertIsNone(updater._SOURCE_POLICY_VERDICTS.get())


class MigrationTransaction(unittest.TestCase):
    def setUp(self):
        self.selected = {'owner': Path('/authored/owner'), 'version': 'authored',
                         'input_location': {'missing': False}}
        self.broker = object.__new__(backend.Broker)
        self.broker.installation = mock.Mock(return_value=self.selected)
        self.broker.verify_launch = mock.Mock()
        self.broker._register_selected = mock.Mock()
        self.broker._open_selected = mock.Mock(return_value=True)
        self.broker.update = mock.Mock()

    def test_internal_migration_resolves_checks_and_opens_once(self):
        self.assertTrue(self.broker.adopt_and_open({'output': '/authored/owner'}))
        self.broker.installation.assert_called_once_with('/authored/owner')
        self.broker.verify_launch.assert_called_once_with(self.selected)
        self.broker._register_selected.assert_called_once_with(self.selected)
        self.broker._open_selected.assert_called_once_with(self.selected)

    def test_missing_input_registers_recovery_without_native_execution(self):
        self.selected['input_location'] = {'missing': True}
        self.assertFalse(self.broker.adopt_and_open({'output': '/authored/owner'}))
        self.broker._register_selected.assert_called_once_with(self.selected)
        self.broker.verify_launch.assert_not_called()
        self.broker._open_selected.assert_not_called()

    def test_failed_verification_does_not_register_or_start_selection(self):
        self.broker.verify_launch.side_effect = ValueError('authored changed receipt')
        with self.assertRaisesRegex(ValueError, 'changed receipt'):
            self.broker.adopt_and_open({'output': '/authored/owner'})
        self.broker._register_selected.assert_not_called()
        self.broker._open_selected.assert_not_called()

    def test_frontend_open_never_reuses_previous_verification(self):
        self.broker.registered = Path('/authored/owner')
        self.broker.missing_input = mock.Mock(return_value=False)
        self.assertTrue(self.broker.open({}))
        self.assertTrue(self.broker.open({}))
        self.assertEqual(self.broker.installation.call_count, 2)
        self.assertEqual(self.broker.verify_launch.call_count, 2)

    def test_internal_operation_cannot_be_requested_through_frontend_dispatch(self):
        self.broker.seen = set()
        self.broker.thread = None
        with self.assertRaisesRegex(backend.BuilderError, 'Unsupported setup action'):
            self.broker.dispatch({'id': 'authored', 'action': 'adopt_and_open',
                                 'output': '/authored/owner'})
        self.broker.installation.assert_not_called()


if __name__ == '__main__':
    unittest.main()
