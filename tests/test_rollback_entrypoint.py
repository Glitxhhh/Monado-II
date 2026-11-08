"""Verified rollback selection; only authored updater fixtures are executed."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
NAME = 'Rollback_Monado_II.sh'


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


@unittest.skipUnless(os.name == 'posix' and shutil.which('bash')
                     and Path('/usr/bin/python3.14').is_file(), 'Linux CPython 3.14 rollback wrapper')
class RollbackEntrypoint(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='monado-rollback-entry-')
        self.addCleanup(temporary.cleanup)
        self.base = Path(temporary.name).resolve()
        self.owner = self.base / 'My Games/Monado II'
        self.owner.mkdir(parents=True)
        self.populate(self.owner)
        self.managed = self.owner / 'Monado_Linux/updates/update-authored/workspace'
        self.save = self.owner / 'Monado_Linux/play/launcher/save/authored-save'
        self.save.parent.mkdir(parents=True)
        self.save.write_text('preserve authored save')

    def populate(self, workspace):
        workspace.mkdir(parents=True, exist_ok=True)
        (workspace / 'tools').mkdir()
        (workspace / 'Monado_Linux/scripts').mkdir(parents=True)
        shutil.copyfile(ROOT / NAME, workspace / NAME)
        (workspace / NAME).chmod(0o755)
        (workspace / 'tools/update_linux.py').write_text(
            'import json,sys\nprint(json.dumps({"fixture_argv":sys.argv}))\n')
        (workspace / 'tools/installation_version.py').write_text('# authored version-aware fixture\n')
        (workspace / 'tools/update_release.py').write_text('# authored imported fixture\n')
        (workspace / 'Monado_Linux/scripts/install_layout.py').write_text('# authored layout fixture\n')
        (workspace / 'update-version.json').write_text(json.dumps({'schema': 1, 'protocol': 1, 'version': '0.1.6'}))
        self.manifest(workspace)

    def manifest(self, workspace):
        files = {}
        for name in (NAME, 'tools/update_linux.py', 'tools/installation_version.py',
                     'tools/update_release.py', 'Monado_Linux/scripts/install_layout.py', 'update-version.json'):
            path = workspace / name
            files[name] = {'bytes': path.stat().st_size, 'sha256': digest(path)}
        (workspace / 'publication-manifest.json').write_text(json.dumps({
            'schema': 1, 'project': 'Monado II', 'source_only': True, 'files': files}))

    def bridge(self, workspace=None):
        workspace = workspace or self.managed
        if workspace != self.owner:
            self.populate(workspace)
            (workspace / 'installation.json').write_text(json.dumps({'schema': 1,
                'owner': str(self.owner), 'workspace': str(workspace), 'game_port': str(workspace / 'Monado_Linux')}))
        directory = self.owner / 'Monado_Linux/updates'
        directory.mkdir(parents=True, exist_ok=True)
        self.record = directory / 'version-bridge.json'
        self.descriptor = {'schema': 1, 'owner': str(self.owner),
            'workspace': '.' if workspace == self.owner else workspace.relative_to(self.owner).as_posix(),
            'manifest_sha256': digest(workspace / 'publication-manifest.json'),
            'updater_sha256': digest(workspace / 'tools/update_linux.py'),
            'version_helper_sha256': digest(workspace / 'tools/installation_version.py'),
            'wrapper_sha256': digest(self.owner / NAME)}
        self.record.write_text(json.dumps(self.descriptor))

    def run_entry(self, *args):
        result = subprocess.run([shutil.which('bash'), str(self.owner / NAME), *args],
            cwd=self.base, stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=15)
        self.assertEqual(self.save.read_text(), 'preserve authored save')
        return result

    def reject(self):
        result = self.run_entry('--check')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('rollback was not started', result.stderr)
        self.assertNotIn('fixture_argv', result.stdout)

    def test_fresh_capable_install_uses_own_updater_without_gui(self):
        result = self.run_entry()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout)['fixture_argv'],
            [str(self.owner / 'tools/update_linux.py'), '--rollback', '--owner', str(self.owner)])

    def test_managed_bridge_keeps_capable_updater_after_rollback_to_older_active(self):
        self.bridge()
        (self.owner / 'update-version.json').write_text('{"schema":1,"protocol":1,"version":"0.1.5"}')
        (self.owner / 'Monado_Linux/updates/active.json').write_text('{"version":"0.1.5"}')
        result = self.run_entry('--check')
        self.assertEqual(result.returncode, 0, result.stderr)
        value = json.loads(result.stdout)
        self.assertEqual(value['workspace'], str(self.managed))
        self.assertEqual(value['argv'][-2:], ['--owner', str(self.owner)])
        self.assertFalse(value['rollback_started'])

    def test_original_capable_bridge_defers_only_marker_validation(self):
        self.bridge(self.owner)
        (self.owner / 'update-version.json').write_text('{"version":"0.1.5"}')
        result = self.run_entry('--check')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout)['workspace'], str(self.owner))

    def test_legacy_without_bridge_is_refused(self):
        (self.owner / 'update-version.json').write_text('{"schema":1,"protocol":1,"version":"0.1.5"}')
        self.reject()

    def test_fresh_boolean_version_protocol_is_refused(self):
        (self.owner / 'update-version.json').write_text('{"schema":true,"protocol":1,"version":"0.1.6"}')
        self.reject()

    def test_bridge_path_escape_absolute_and_wrong_owner_refused(self):
        self.bridge()
        for key, value in (('workspace', '../outside'), ('workspace', str(self.managed)),
                           ('workspace', 'Monado_Linux/updates/update-authored/workspace/../elsewhere'),
                           ('owner', str(self.base))):
            with self.subTest(key=key, value=value):
                record = dict(self.descriptor, **{key: value})
                self.record.write_text(json.dumps(record))
                self.reject()

    def test_all_bridge_hashes_must_match(self):
        self.bridge()
        for key in ('manifest_sha256', 'updater_sha256', 'version_helper_sha256', 'wrapper_sha256'):
            with self.subTest(key=key):
                self.record.write_text(json.dumps(dict(self.descriptor, **{key: '0' * 64})))
                self.reject()

    def test_changed_imported_source_is_refused(self):
        self.bridge()
        (self.managed / 'tools/update_release.py').write_text('# changed\n')
        self.reject()

    def test_unreviewed_import_source_is_refused(self):
        self.bridge()
        for suffix in ('.py', '.pyc', '.so'):
            with self.subTest(suffix=suffix):
                path = self.managed / ('tools/unreviewed' + suffix)
                path.write_text('# unreviewed\n')
                self.reject()
                path.unlink()

    def test_malformed_duplicate_and_oversize_metadata_refused(self):
        self.bridge()
        for value in ('{', '{"schema":1,"schema":1}', 'x' * 8193,
                      json.dumps(dict(self.descriptor, unexpected=True))):
            with self.subTest(value=value[:40]):
                self.record.write_text(value)
                self.reject()

    def test_bridge_and_retained_source_links_refused(self):
        self.bridge()
        saved = self.record.read_bytes()
        other = self.base / 'bridge-copy'; other.write_bytes(saved)
        self.record.unlink(); self.record.symlink_to(other)
        self.reject()
        self.record.unlink(); self.record.write_bytes(saved)
        source = self.managed / 'tools/update_linux.py'
        other = self.base / 'updater-copy'; other.write_bytes(source.read_bytes())
        source.unlink(); source.symlink_to(other)
        self.reject()

    def test_retained_binding_conflict_is_refused(self):
        self.bridge()
        path = self.managed / 'installation.json'
        record = json.loads(path.read_text());record['owner'] = str(self.base)
        path.write_text(json.dumps(record))
        self.reject()

    def test_retained_workspace_directory_alias_is_refused(self):
        self.bridge()
        other = self.base / 'workspace-copy'
        self.managed.rename(other)
        self.managed.symlink_to(other, target_is_directory=True)
        self.reject()

    def test_changed_owner_wrapper_is_refused(self):
        self.bridge()
        path = self.owner / NAME
        path.write_text(path.read_text() + '\n# changed fixture\n')
        self.reject()

    def test_boolean_schema_and_wrong_manifest_identity_refused(self):
        self.bridge()
        self.record.write_text(json.dumps(dict(self.descriptor, schema=True)))
        self.reject()
        self.record.write_text(json.dumps(self.descriptor))
        path = self.managed / 'publication-manifest.json'
        value = json.loads(path.read_text());value['source_only'] = False
        path.write_text(json.dumps(value))
        self.descriptor['manifest_sha256'] = digest(path)
        self.record.write_text(json.dumps(self.descriptor))
        self.reject()

    def test_unknown_arguments_do_not_execute_updater(self):
        result = self.run_entry('--owner', '/outside')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('usage:', result.stderr)
        self.assertNotIn('fixture_argv', result.stdout)


if __name__ == '__main__':
    unittest.main()
