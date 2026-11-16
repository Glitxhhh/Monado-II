"""The legacy Windows entry point must retain the exact publication boundary."""
import contextlib
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest

PORT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('legacy_windows_package_guard', PORT / 'tools/package_guard.py')
guard = importlib.util.module_from_spec(spec)
spec.loader.exec_module(guard)


class StrictPublicationFacade(unittest.TestCase):
    def test_complete_reviewed_source_and_changed_content(self):
        with tempfile.TemporaryDirectory(prefix='authored-publication-facade-') as temporary:
            root = Path(temporary).resolve()
            files = {'README.md': b'Authored source fixture.\n',
                     'publication-policy.json': b'{"schema":1,"licensed_media":{}}\n'}
            for name, data in files.items():
                (root / name).write_bytes(data)
            document = {'schema': 1, 'project': 'Monado II', 'files': {
                name: {'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
                for name, data in files.items()}}
            (root / 'publication-manifest.json').write_text(json.dumps(document), encoding='utf-8')
            self.assertEqual(guard.scan(root), [])
            (root / 'README.md').write_bytes(b'Changed unreviewed authored bytes.\n')
            self.assertTrue(any('identity mismatch' in reason for reason in guard.scan(root)))

    def test_missing_manifest_and_unknown_binary_fail(self):
        with tempfile.TemporaryDirectory(prefix='authored-publication-facade-') as temporary:
            root = Path(temporary).resolve()
            (root / 'unknown.dll').write_bytes(b'MZauthored fixture, never executed')
            self.assertTrue(guard.scan(root))

    def test_old_partial_git_mode_is_refused(self):
        with contextlib.redirect_stdout(io.StringIO()) as output:
            self.assertEqual(guard.main(['--git', '.']), 1)
        self.assertIn('clean reviewed source export', output.getvalue())


if __name__ == '__main__':
    unittest.main()
