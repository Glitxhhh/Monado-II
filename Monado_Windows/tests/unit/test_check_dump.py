"""T005 (tests first): check_dump.py with synthetic dumps. No game data is used (Constitution art. I)."""
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', '..', 'tools', 'extract'))
import check_dump as cd  # noqa: E402
from manifest import build_manifest  # noqa: E402


def wb(path, data, mode='wb'):
    with open(path, mode) as f:
        f.write(data)


def make_nso(path, build_id_hex):
    raw = bytearray(0x100)
    raw[0:4] = b'NSO0'
    bid = bytes.fromhex(build_id_hex)
    raw[0x40:0x40 + len(bid)] = bid
    with open(path, 'wb') as f:
        f.write(raw)


def make_dump(root, build_ids, files):
    exefs = os.path.join(root, 'exefs')
    romfs = os.path.join(root, 'romfs')
    os.makedirs(exefs)
    os.makedirs(romfs)
    for name, bid in build_ids.items():
        make_nso(os.path.join(exefs, name), bid)
    wb(os.path.join(exefs, 'main.npdm'), b'META' + bytes(60))
    for rel, size in files.items():
        p = os.path.join(romfs, rel)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        wb(p, bytes(size))
    return exefs, romfs


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = self.tmp.name
        self.ids = {'rtld': 'aa' * 8, 'main': 'bb' * 8, 'sdk': 'cc' * 8, 'subsdk0': 'dd' * 8}
        self.files = {'a/one.bin': 10, 'a/two.bin': 20, 'b/three.bin': 5}
        self.profile = {
            'version': '9.9.9',
            'modules': [{'name': n, 'build_id': b} for n, b in self.ids.items()],
            'exefs_required': ['main', 'main.npdm', 'rtld', 'sdk', 'subsdk0'],
        }
        exefs, romfs = make_dump(os.path.join(self.root, 'dump'), self.ids, self.files)
        self.exefs, self.romfs = exefs, romfs
        self.profile['romfs_manifest'] = build_manifest(romfs)

    def tearDown(self):
        self.tmp.cleanup()


class TestGoodDump(Base):
    def test_accepts_correct_dump(self):
        r = cd.check_dump(os.path.dirname(self.exefs), self.profile)
        self.assertTrue(r.ok, r.messages)

    def test_accepts_separate_exefs_romfs_arguments(self):
        r = cd.check_dump(None, self.profile, exefs=self.exefs, romfs=self.romfs)
        self.assertTrue(r.ok, r.messages)

    def test_path_with_spaces_and_non_ascii(self):
        weird = os.path.join(self.root, 'José Ñ Juegos (copia)')
        exefs, romfs = make_dump(weird, self.ids, self.files)
        r = cd.check_dump(weird, self.profile)
        self.assertTrue(r.ok, r.messages)


class TestRejections(Base):
    def test_rejects_encrypted_containers_without_reading(self):
        for ext in ('.nsp', '.xci', '.nca', '.nsz', '.xcz'):
            p = os.path.join(self.root, 'game' + ext)
            wb(p, b'secret')
            r = cd.check_dump(p, self.profile)
            self.assertFalse(r.ok)
            self.assertEqual(r.code, cd.ERR_CONTAINER, ext)

    def test_rejects_folder_holding_only_containers(self):
        d = os.path.join(self.root, 'only_nsp')
        os.makedirs(d)
        wb(os.path.join(d, 'x.nsp'), b'x')
        r = cd.check_dump(d, self.profile)
        self.assertFalse(r.ok)
        self.assertEqual(r.code, cd.ERR_CONTAINER)

    def test_wrong_version_reports_found_and_expected(self):
        make_nso(os.path.join(self.exefs, 'main'), 'ee' * 8)
        r = cd.check_dump(os.path.dirname(self.exefs), self.profile)
        self.assertFalse(r.ok)
        self.assertEqual(r.code, cd.ERR_VERSION)
        text = '\n'.join(r.messages)
        self.assertIn('9.9.9', text)
        self.assertIn('eeeeeeeeeeeeeeee', text)

    def test_missing_exefs_file(self):
        os.remove(os.path.join(self.exefs, 'sdk'))
        r = cd.check_dump(os.path.dirname(self.exefs), self.profile)
        self.assertFalse(r.ok)
        self.assertEqual(r.code, cd.ERR_EXEFS)
        self.assertIn('sdk', '\n'.join(r.messages))

    def test_not_an_nso(self):
        wb(os.path.join(self.exefs, 'main'), b'not an nso at all' * 20)
        r = cd.check_dump(os.path.dirname(self.exefs), self.profile)
        self.assertFalse(r.ok)
        self.assertEqual(r.code, cd.ERR_EXEFS)

    def test_incomplete_romfs_names_the_directory(self):
        os.remove(os.path.join(self.romfs, 'a', 'two.bin'))
        r = cd.check_dump(os.path.dirname(self.exefs), self.profile)
        self.assertFalse(r.ok)
        self.assertEqual(r.code, cd.ERR_ROMFS)
        self.assertIn("'a'", '\n'.join(r.messages))

    def test_truncated_file_is_detected_by_size(self):
        wb(os.path.join(self.romfs, 'b', 'three.bin'), bytes(2))
        r = cd.check_dump(os.path.dirname(self.exefs), self.profile)
        self.assertFalse(r.ok)
        self.assertEqual(r.code, cd.ERR_ROMFS)

    def test_missing_romfs(self):
        import shutil
        shutil.rmtree(self.romfs)
        r = cd.check_dump(os.path.dirname(self.exefs), self.profile)
        self.assertFalse(r.ok)
        self.assertEqual(r.code, cd.ERR_ROMFS)

    def test_nothing_is_printed_from_keys_files(self):
        wb(os.path.join(os.path.dirname(self.exefs), 'prod.keys'), 'master_key_00 = ' + 'ab' * 16, 'w')
        r = cd.check_dump(os.path.dirname(self.exefs), self.profile)
        self.assertNotIn('abababab', '\n'.join(r.messages))


if __name__ == '__main__':
    unittest.main()
