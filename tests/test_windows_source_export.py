"""Native authored clean-export checks; private workspace files stay untouched."""
import hashlib
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import export_windows_sources as export
import publication_guard as guard


@unittest.skipUnless(os.name == 'nt', 'native Windows clean source export')
class WindowsSourceExport(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='monado-authored-export-')
        self.addCleanup(self.temporary.cleanup)
        self.base = Path(self.temporary.name).resolve()
        self.source = self.base / 'checkout'
        self.source.mkdir()
        self.files = {'README.md': b'# Authored fixture\n',
            'Monado_Windows/runtime/example.c': b'int authored(void){return 1;}\n',
            'Monado_Linux/runtime/example.c': b'int reference(void){return 2;}\n',
            guard.POLICY: export.encoded({'schema': 1, 'licensed_media': {}})}
        self.refresh()

    def refresh(self):
        for name, value in self.files.items():
            path = self.source / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(value)
        document = {'schema': 1, 'project': 'Monado II', 'source_only': True,
            'files': {name: {'bytes': len(value), 'sha256': export.digest(value)}
                      for name, value in self.files.items()}}
        self.raw = export.encoded(document)
        (self.source / guard.MANIFEST).write_bytes(self.raw)
        self.sha = export.digest(self.raw)

    def test_explicit_source_only_never_reads_private_input_or_profiles(self):
        private = self.source / 'game_files/human.txt'
        private.parent.mkdir()
        private.write_bytes(b'Authored private fixture, excluded from source.')
        output = self.base / 'export'
        with mock.patch.object(export, 'read_regular', wraps=guard.read_regular) as read:
            export.export(self.source, output, self.sha)
        self.assertFalse(any(Path(call.args[0]).is_relative_to(private.parent) for call in read.call_args_list))
        self.assertEqual(private.read_bytes(), b'Authored private fixture, excluded from source.')
        self.assertFalse((output / 'source/Monado_Linux').exists())
        self.assertFalse((output / 'source/game_files').exists())
        self.assertEqual(guard.verify_tree(output / 'source'), [])
        declaration = json.loads((output / 'source-manifest.json').read_bytes())
        self.assertEqual(declaration['platform'], 'windows-amd64')
        for name, identity in declaration['files'].items():
            value = (output / 'source' / name).read_bytes()
            self.assertEqual(identity, {'bytes': len(value), 'sha256': export.digest(value)})

    def test_changed_source_wrong_manifest_hash_and_existing_output_fail_closed(self):
        with self.assertRaises(ValueError):
            export.export(self.source, self.base / 'wrong', '0' * 64)
        original = self.base / 'human'
        original.mkdir()
        (original / 'keep.txt').write_bytes(b'keep')
        with self.assertRaises(ValueError):
            export.export(self.source, original, self.sha)
        self.assertEqual((original / 'keep.txt').read_bytes(), b'keep')
        (self.source / 'README.md').write_bytes(b'edited after review')
        with self.assertRaises(ValueError):
            export.export(self.source, self.base / 'changed', self.sha)
        self.assertFalse((self.base / 'changed').exists())

    def test_manifest_cannot_admit_private_or_compiled_content(self):
        for name, value in (('game_files/unknown.txt', b'private'),
                            ('Monado_Windows/runtime/unknown.txt', bytes((77, 90)) + b' authored magic')):
            with self.subTest(name=name):
                self.files[name] = value
                self.refresh()
                with self.assertRaises(ValueError):
                    export.export(self.source, self.base / 'rejected', self.sha)
                self.files.pop(name)


if __name__ == '__main__':
    unittest.main()
