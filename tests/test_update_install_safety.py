"""Independent updater regressions for candidate trust and private-state safety."""
import hashlib
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
spec = importlib.util.spec_from_file_location('update_install_safety_core', ROOT / 'tools/update_linux.py')
updater = importlib.util.module_from_spec(spec)
spec.loader.exec_module(updater)


def encoded(value):
    return (json.dumps(value, sort_keys=True) + '\n').encode()


def identity(data):
    return dict(sha256=hashlib.sha256(data).hexdigest(), bytes=len(data))


def source_tree(root, files):
    files = {**files, 'publication-policy.json': encoded(dict(schema=1, licensed_media={}))}
    document = dict(schema=1, source_only=True, project='Monado II',
                    files={name: identity(data) for name, data in files.items()})
    for name, data in {**files, 'publication-manifest.json': encoded(document)}.items():
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
    return document


class InstallSafety(unittest.TestCase):
    def setUp(self):
        context = tempfile.TemporaryDirectory(prefix='monado-update-safety-')
        self.addCleanup(context.cleanup)
        self.owner = Path(context.name)

    def test_corrupt_candidate_helper_cannot_execute_before_source_verification(self):
        original = self.owner / 'Monado_Linux'
        scripts = original / 'scripts'; scripts.mkdir(parents=True)
        # This is trusted installed code; only its marker/path validation is used.
        (scripts / 'install_layout.py').write_bytes(
            (ROOT / 'Monado_Linux/scripts/install_layout.py').read_bytes())
        identifier = 'update-negative-fixture'
        candidate = original / 'updates' / identifier / 'workspace'
        candidate.mkdir(parents=True)
        source_tree(candidate, {
            'update-version.json': encoded(dict(schema=1, protocol=1, version='0.1.4')),
            'Monado_Linux/scripts/install_layout.py': b'# Original reviewed source fixture\n',
        })
        (candidate / 'installation.json').write_bytes(encoded(dict(schema=1,
            owner=str(self.owner), workspace=str(candidate), game_port=str(original))))
        sentinel = self.owner / 'candidate-code-executed'
        # Deliberately differs from its manifest. Verification must only read it.
        (candidate / 'Monado_Linux/scripts/install_layout.py').write_text(
            'from pathlib import Path\n'
            + 'Path(' + repr(str(sentinel)) + ').write_text("unexpected execution")\n'
            + 'def installation_root(port): return Path(' + repr(str(self.owner)) + ')\n')
        record = dict(schema=1, version='0.1.4', id=identifier,
            workspace=candidate.relative_to(self.owner).as_posix(), image_sha256='a' * 64,
            receipt_sha256='b' * 64,
            source_manifest_sha256=hashlib.sha256((candidate / 'publication-manifest.json').read_bytes()).hexdigest())
        with mock.patch.object(updater, 'ROOT', self.owner):
            with self.assertRaisesRegex(ValueError, 'Local source changes'):
                updater.active_record(self.owner, record)
        self.assertFalse(sentinel.exists(), 'unverified candidate code was executed')

    def test_installed_source_check_does_not_read_or_modify_private_data(self):
        document = source_tree(self.owner, {'README.md': b'# Authored installation fixture\n'})
        save = self.owner / 'Monado_Linux/play/launcher/saves/authored.sav'
        save.parent.mkdir(parents=True); save.write_bytes(b'authored save sentinel')
        private = self.owner / 'game_files/romfs/private.sentinel'
        private.parent.mkdir(parents=True); private.write_bytes(b'authored input sentinel')
        generated = self.owner / 'Monado_Linux/generated/private.c'
        generated.parent.mkdir(parents=True); generated.write_bytes(b'authored generated sentinel')
        before = {path: path.read_bytes() for path in (save, private, generated)}
        checked = []
        original = updater.read_regular
        def read(path):
            checked.append(Path(path))
            return original(path)
        with mock.patch.object(updater, 'read_regular', side_effect=read):
            self.assertEqual(updater.installed_sources(self.owner), document)
        self.assertTrue(set(checked) <= {self.owner / 'README.md', self.owner / 'publication-policy.json',
                                        self.owner / 'publication-manifest.json'})
        for path, data in before.items():
            self.assertEqual(path.read_bytes(), data)

    def test_authored_local_changes_are_preserved_and_refused(self):
        source_tree(self.owner, {'README.md': b'# Published authored source\n'})
        changed = b'# Local author edits that must survive\n'
        (self.owner / 'README.md').write_bytes(changed)
        with self.assertRaisesRegex(ValueError, 'Local source changes'):
            updater.installed_sources(self.owner)
        self.assertEqual((self.owner / 'README.md').read_bytes(), changed)
        self.assertFalse((self.owner / 'Monado_Linux/updates/active.json').exists())


if __name__ == '__main__':
    unittest.main()
