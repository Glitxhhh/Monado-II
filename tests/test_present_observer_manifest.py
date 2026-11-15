"""LINUX-099: fixed explicit-layer identity and private manifest boundaries."""
from pathlib import Path
import hashlib
import importlib.util
import json
import os
import sys
import tempfile
import unittest
from unittest import mock
ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('test_present_manifest', ROOT / 'Monado_Linux/scripts/present_observer_manifest.py')
manifest = importlib.util.module_from_spec(SPEC); SPEC.loader.exec_module(manifest)


@unittest.skipUnless(sys.platform == 'linux', 'Linux ELF observer identity and POSIX ownership/manifest permissions')
class ObserverManifest(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory(prefix='monado-observer-manifest-'); self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name); self.state = self.root / 'state'; self.state.mkdir(mode=0o700)
        self.build = self.root / 'build'; self.build.mkdir()
        self.binary = self.build / 'authored-no-game'
        self.library = self.build / manifest.RESOURCE
        header = bytearray(64); header[:6] = b'\x7fELF\x02\x01'; header[18:20] = (62).to_bytes(2, 'little')
        self.library.write_bytes(header)
        self.receipt = {'present_observer': {'abi': 1, 'generated_classification': False},
            'runtime_resources': {manifest.RESOURCE: {'path': str(self.library), 'bytes': 64,
                'sha256': hashlib.sha256(header).hexdigest()}}}
    def test_resource_exact_receipt_hash_abi_and_ordinary_owned_file(self):
        self.assertEqual(manifest.checked_resource(self.binary, self.receipt), str(self.library))
        self.receipt['present_observer']['generated_classification'] = True
        self.assertIsNone(manifest.checked_resource(self.binary, self.receipt))
        self.receipt['present_observer']['generated_classification'] = False
        self.library.write_bytes(b'MZ' + bytes(62)); self.assertIsNone(manifest.checked_resource(self.binary, self.receipt))
        self.library.unlink(); self.library.symlink_to(self.binary)
        self.assertIsNone(manifest.checked_resource(self.binary, self.receipt))
    def test_first_use_idempotence_and_receipted_version_replacement(self):
        directory = Path(manifest.prepare(self.state, str(self.library)))
        self.assertEqual(set(p.name for p in directory.iterdir()), {'owner', 'observer.json'})
        self.assertEqual(manifest.prepare(self.state, str(self.library)), str(directory))
        next_library = self.root / 'next-build' / manifest.RESOURCE
        manifest.prepare(self.state, str(next_library))
        doc = json.loads((directory / 'observer.json').read_text())
        self.assertEqual(doc['layer']['library_path'], str(next_library))
        self.assertEqual(doc['layer']['name'], manifest.LAYER)
        self.assertEqual(doc['layer']['type'], 'GLOBAL')
        for p in directory.iterdir(): self.assertEqual(p.stat().st_mode & 0o777, 0o600)
        self.assertEqual(directory.stat().st_mode & 0o777, 0o700)
    def test_resource_path_replaced_during_read_fails_closed(self):
        digest = manifest.hashlib.file_digest
        old = self.library.read_bytes()
        def replace(stream, algorithm):
            result = digest(stream, algorithm)
            replacement = self.build / 'replacement'; replacement.write_bytes(old)
            replacement.replace(self.library)
            return result
        with mock.patch.object(manifest.hashlib, 'file_digest', side_effect=replace):
            self.assertIsNone(manifest.checked_resource(self.binary, self.receipt))
    def test_foreign_entries_owner_contents_and_manifest_injection_are_preserved(self):
        directory = Path(manifest.prepare(self.state, str(self.library)))
        foreign = directory / 'foreign.json'; foreign.write_text('do not overwrite')
        with self.assertRaises(ValueError): manifest.prepare(self.state, str(self.library))
        self.assertEqual(foreign.read_text(), 'do not overwrite'); foreign.unlink()
        owner = directory / 'owner'; owner.write_text('foreign owner')
        with self.assertRaises(ValueError): manifest.prepare(self.state, str(self.library))
        self.assertEqual(owner.read_text(), 'foreign owner'); owner.write_bytes(manifest.OWNER)
        file = directory / 'observer.json'; doc = json.loads(file.read_text()); doc['layer']['enable_environment'] = {'UNTRUSTED': '1'}; file.write_text(json.dumps(doc))
        with self.assertRaises(ValueError): manifest.prepare(self.state, str(self.library))
        self.assertIn('enable_environment', file.read_text())
    def test_symlink_hardlink_nonprivate_mode_and_nonregular_files_fail_closed(self):
        directory = Path(manifest.prepare(self.state, str(self.library))); file = directory / 'observer.json'
        outside = self.root / 'sentinel'; outside.write_text('preserve outside')
        original = file.read_bytes(); file.unlink(); file.symlink_to(outside)
        with self.assertRaises(OSError): manifest.prepare(self.state, str(self.library))
        self.assertEqual(outside.read_text(), 'preserve outside'); file.unlink()
        os.link(outside, file)
        with self.assertRaises(ValueError): manifest.prepare(self.state, str(self.library))
        self.assertEqual(outside.read_text(), 'preserve outside'); file.unlink(); file.write_bytes(original); file.chmod(0o644)
        with self.assertRaises(ValueError): manifest.prepare(self.state, str(self.library))
        file.chmod(0o600); file.unlink(); os.mkfifo(file, mode=0o600)
        with self.assertRaises(ValueError): manifest.prepare(self.state, str(self.library))
    def test_symlinked_parent_and_directory_are_never_followed(self):
        runtime = self.state / 'runtime'; outside = self.root / 'outside'; outside.mkdir(mode=0o700); runtime.symlink_to(outside)
        with self.assertRaises(OSError): manifest.prepare(self.state, str(self.library))
        self.assertFalse(list(outside.iterdir()))
        runtime.unlink(); runtime.mkdir(mode=0o700); (runtime / manifest.DIRECTORY).symlink_to(outside)
        with self.assertRaises(OSError): manifest.prepare(self.state, str(self.library))
        self.assertFalse(list(outside.iterdir()))


if __name__ == '__main__': unittest.main()
