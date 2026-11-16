"""Authored Windows release ZIP → installed updater contract; no network/game."""
import importlib.util
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock
import zipfile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('native_windows_release_package',
    ROOT / 'Monado_Windows/tools/package_windows_release.py')
release = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = release
spec.loader.exec_module(release)


class Response(io.BytesIO):
    def __init__(self, data, url):
        super().__init__(data)
        self.url = url

    def geturl(self):
        return self.url

    def getcode(self):
        return 200


@unittest.skipUnless(os.name == 'nt', 'native Windows source publication/file leases')
class WindowsReleasePackage(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='monado-authored-release-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.source = self.root / 'checked source 漢字 $() `'
        self.source.mkdir()
        self.files = {'README.md': b'# Authored native source fixture\n',
            'LICENSE': b'Authored fixture license\n',
            'publication-policy.json': release.encoded({'schema': 1, 'licensed_media': {}}),
            'Monado_Windows/runtime/example.c': b'int authored(void){return 42;}\n'}
        self.manifest = dict(schema=1, project='Monado II', source_only=True,
            platform='windows', windows_implementation='windows-parity-development')
        self.refresh()

    def refresh(self):
        for name, data in self.files.items():
            path = self.source / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        self.manifest['files'] = {name: {'bytes': len(data), 'sha256': release.digest(data)}
                                  for name, data in self.files.items()}
        raw = release.encoded(self.manifest)
        (self.source / release.MANIFEST).write_bytes(raw)
        self.sha = release.digest(raw)

    def package(self, output='artifacts', version='0.1.0'):
        return release.package(self.source, self.root / output, version, self.sha)

    def test_generated_artifacts_roundtrip_installed_updater_and_publication_guard(self):
        before = {name: (self.source / name).read_bytes() for name in self.files}
        receipt = self.package()
        output = self.root / 'artifacts'
        self.assertEqual(receipt['windows_identity'], 'WindowsExperimental')
        self.assertEqual(receipt['version'], '0.1.0')
        self.assertTrue(receipt['archive_readback_verified'])
        self.assertFalse(receipt['portable_exe_included'])
        self.assertFalse(receipt['release_published'])
        metadata_raw = (output / release.update.METADATA_NAME).read_bytes()
        metadata = json.loads(metadata_raw)
        archive = (output / receipt['archive']).read_bytes()
        self.assertEqual(set(path.name for path in output.iterdir()),
            {receipt['archive'], release.update.METADATA_NAME, 'SHA256SUMS', 'release-receipt.json'})
        self.assertEqual(receipt['metadata_sha256'], release.digest(metadata_raw))
        self.assertEqual(metadata['archive_sha256'], release.digest(archive))
        self.assertEqual(metadata['source_manifest_sha256'], receipt['release_manifest_sha256'])
        self.assertNotEqual(receipt['reviewed_manifest_sha256'], receipt['release_manifest_sha256'])
        for row in (output / 'SHA256SUMS').read_text('ascii').splitlines():
            expected, name = row.split('  ')
            self.assertEqual(expected, release.digest((output / name).read_bytes()))
        tag = 'v0.1.20'
        assets = {}
        for name, data in ((receipt['archive'], archive), (release.update.METADATA_NAME, metadata_raw)):
            url = release.update._download_url(tag, name)
            assets[url] = data
        declaration = dict(draft=False, prerelease=False, tag_name=tag, assets=[
            dict(name=name, state='uploaded', size=len(data), digest='sha256:' + release.digest(data),
                 browser_download_url=release.update._download_url(tag, name))
            for name, data in ((receipt['archive'], archive), (release.update.METADATA_NAME, metadata_raw))])
        assets[release.update.common.LATEST_URL] = release.encoded(declaration)
        requests = []

        def opener(request, timeout):
            self.assertEqual(timeout, 30)
            requests.append(request.full_url)
            return Response(assets[request.full_url], request.full_url)

        offer = release.update.discover('0.0.0', opener=opener)
        self.assertEqual(offer['version'], '0.1.0')
        source = release.update.fetch_and_extract(offer, self.root / 'extracted 漢字', opener=opener)
        verified = release.update.verified_source(source, offer)
        self.assertEqual(verified['windows_identity'], 'WindowsExperimental')
        self.assertNotIn(release.MANIFEST, verified['files'])
        self.assertNotIn(release.update.METADATA_NAME, verified['files'])
        self.assertEqual(release.verify_tree(source), [])
        for name, data in before.items():
            self.assertEqual((source / name).read_bytes(), data)
            self.assertEqual((self.source / name).read_bytes(), data)
        status = json.loads((source / 'source-only-status.json').read_bytes())
        self.assertEqual(status['release_channel'], 'experimental')
        self.assertEqual(status['runtime_validation'], 'not established by packaging')
        self.assertIsNone(release.update.discover('0.1.0', opener=opener))
        self.assertEqual(len(requests), 5)

    def test_repeated_packaging_is_byte_deterministic_and_manifest_has_no_hash_cycle(self):
        left, right = self.package('left'), self.package('right')
        self.assertEqual(left, right)
        for name in (left['archive'], left['metadata'], 'SHA256SUMS', 'release-receipt.json'):
            self.assertEqual((self.root / 'left' / name).read_bytes(), (self.root / 'right' / name).read_bytes())
        with zipfile.ZipFile(self.root / 'left' / left['archive']) as archive:
            self.assertEqual(len(archive.infolist()), left['archived_files'])
            self.assertTrue(all(item.date_time == release.ZIP_DATE and not item.is_dir()
                                for item in archive.infolist()))
            manifest = json.loads(archive.read(left['archive_root'] + '/' + release.MANIFEST))
            self.assertEqual(set(archive.namelist()),
                {left['archive_root'] + '/' + name for name in manifest['files']} |
                {left['archive_root'] + '/' + release.MANIFEST})

    def test_wrong_identity_edited_source_and_unknown_files_fail_before_output(self):
        with self.assertRaises(ValueError):
            release.package(self.source, self.root / 'wrong', '0.1.0', '0' * 64)
        self.assertFalse((self.root / 'wrong').exists())
        (self.source / 'README.md').write_bytes(b'changed after source review\n')
        with self.assertRaises(ValueError):
            self.package('changed')
        self.assertFalse((self.root / 'changed').exists())
        self.refresh()
        (self.source / 'unknown.txt').write_bytes(b'human authored unknown, preserve\n')
        with self.assertRaises(ValueError):
            self.package('unknown')
        self.assertEqual((self.source / 'unknown.txt').read_bytes(), b'human authored unknown, preserve\n')

    def test_cross_platform_legacy_identity_private_binary_and_reserved_files_rejected(self):
        cases = [('Monado_Linux/runtime/example.c', b'int reference(void){return 1;}\n'),
                 ('update-version.json', b'{"version":"9.9.9"}\n'),
                 ('RELEASE_README.md', b'existing authored human readme\n'),
                 ('source-only-status.json', b'{}\n'),
                 ('Monado_Windows/runtime/hidden.txt', bytes((77, 90)) + b' authored binary fixture')]
        for index, (name, data) in enumerate(cases):
            with self.subTest(name=name):
                self.files[name] = data
                self.refresh()
                with self.assertRaises(ValueError):
                    self.package('rejected-' + str(index))
                self.assertFalse((self.root / ('rejected-' + str(index))).exists())
                self.files.pop(name)
                (self.source / name).unlink()
                # Empty forbidden cross-platform directories are still guarded.
                if name.startswith('Monado_Linux/'):
                    (self.source / 'Monado_Linux/runtime').rmdir()
                    (self.source / 'Monado_Linux').rmdir()
        self.manifest['windows_implementation'] = 'original'
        self.refresh()
        with self.assertRaises(ValueError):
            self.package('legacy')

    def test_non_numeric_version_and_existing_or_nested_output_preserve_original(self):
        for version in ('0.1.0-experimental', 'v0.1.0', '01.0.0', '0.1', '../0.1.0'):
            with self.subTest(version=version), self.assertRaises(ValueError):
                self.package(version=version)
        output = self.root / 'human output'
        output.mkdir()
        (output / 'keep.txt').write_bytes(b'original unknown')
        with self.assertRaises(OSError):
            release.package(self.source, output, '0.1.0', self.sha)
        self.assertEqual((output / 'keep.txt').read_bytes(), b'original unknown')
        with self.assertRaises(ValueError):
            release.package(self.source, self.source / 'inside', '0.1.0', self.sha)

    def test_native_source_file_leases_prevent_mutation_during_archive_creation(self):
        original = release._archive

        def while_leased(contents, prefix):
            with self.assertRaises(OSError):
                (self.source / 'README.md').write_bytes(b'concurrent mutation')
            with self.assertRaises(OSError):
                self.source.rename(self.root / 'renamed')
            return original(contents, prefix)

        with mock.patch.object(release, '_archive', side_effect=while_leased):
            self.package()

    def test_hardlink_and_write_locked_source_members_fail_closed(self):
        leaf = self.source / 'README.md'
        alias = self.root / 'alias.txt'
        os.link(leaf, alias)
        with self.assertRaises(OSError):
            self.package('hardlink')
        alias.unlink()
        handle, _ = release.guard._open(str(leaf), directory=False, access=0x40000000 | release.guard._READ_ATTR)
        try:
            with self.assertRaises(OSError):
                self.package('locked')
        finally:
            release.guard.K.CloseHandle(handle)
        self.assertEqual(leaf.read_bytes(), self.files['README.md'])

    def test_corrupt_archive_never_publishes_outputs(self):
        with mock.patch.object(release, '_archive', return_value=b'authored invalid ZIP'):
            with self.assertRaises(ValueError):
                self.package('corrupt')
        self.assertFalse((self.root / 'corrupt').exists())


if __name__ == '__main__':
    unittest.main()
