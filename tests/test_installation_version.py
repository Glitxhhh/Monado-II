"""Authored installed-marker schema and immutable-backup checks."""
import copy
import hashlib
import json
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import installation_version as version


class InstalledVersion(unittest.TestCase):
    def setUp(self):
        self.raw = b'{"schema":1,"protocol":1,"version":"0.1.4"}\n'
        self.owner = Path('/authored/installation')
        self.manifest = 'a' * 64
        self.identity = dict(bytes=len(self.raw), sha256=hashlib.sha256(self.raw).hexdigest())
        self.backup = version.make_backup(self.owner, self.manifest, self.identity, self.raw)

    def test_backup_roundtrip_preserves_exact_original_bytes(self):
        self.assertEqual(version.original_from_backup(self.backup, self.owner, self.manifest, self.identity), self.raw)
        self.assertEqual(version.metadata(self.raw)['version'], '0.1.4')

    def test_malformed_or_unsupported_marker_is_refused(self):
        cases = [b'', b'[]', b'{}', b'no JSON', b'\xff', b' ' * (version.MAX_METADATA + 1),
                 b'{"schema":1,"schema":1,"protocol":1,"version":"0.1.4"}']
        for key, value in [('schema', True), ('schema', 2), ('protocol', True), ('protocol', 2),
                           ('version', None), ('version', '01.1.4'), ('version', '0.1.4-beta'),
                           ('version', '0.1.4\n'), ('extra', 1)]:
            document = json.loads(self.raw); document[key] = value
            cases.append(json.dumps(document).encode())
        for raw in cases:
            with self.subTest(raw=raw), self.assertRaises(ValueError):
                version.metadata(raw)

    def test_backup_is_bound_to_original_owner_manifest_and_file_identity(self):
        cases = []
        for key, value in [('schema', True), ('owner', '/another'), ('manifest_sha256', 'b' * 64),
                           ('original_metadata', self.raw.decode().replace('0.1.4', '0.1.5')),
                           ('original_metadata', None), ('original_metadata', '\ud800'), ('extra', 1)]:
            document = copy.deepcopy(self.backup); document[key] = value; cases.append(document)
        for document in cases:
            with self.subTest(document=document), self.assertRaises(ValueError):
                version.original_from_backup(document, self.owner, self.manifest, self.identity)
        for identity in ({}, {**self.identity, 'bytes': True}, {**self.identity, 'bytes': len(self.raw) + 1},
                         {**self.identity, 'sha256': '0' * 64}):
            with self.subTest(identity=identity), self.assertRaises(ValueError):
                version.original_from_backup(self.backup, self.owner, self.manifest, identity)


if __name__ == '__main__':
    unittest.main()
