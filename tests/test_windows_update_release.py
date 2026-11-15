"""Authored Windows update/channel/archive fixtures, never game input."""
import hashlib
import io
import json
import os
from pathlib import Path
import stat
import sys
import tempfile
import unittest
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import update_windows_release as update


def encode(value):
    return (json.dumps(value, sort_keys=True) + '\n').encode()


class Response(io.BytesIO):
    def __init__(self, data, url):
        super().__init__(data)
        self.url = url

    def geturl(self):
        return self.url

    def getcode(self):
        return 200


class WindowsSourceUpdates(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='monado-authored-update-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.tag = 'v9.9.9'
        self.version = '1.0.0'
        self.prefix = 'monado-ii-windows-' + self.version + '-source'
        self.files = {
            'README.md': b'# Authored source fixture\n',
            'Monado_Windows/runtime/example.c': b'int authored(void){return 42;}\n',
            'publication-policy.json': encode({'schema': 1, 'licensed_media': {}}),
            'source-only-status.json': encode({'schema': 1, 'project': 'Monado II', 'platform': 'windows',
                'version': self.version, 'kind': 'source-only', 'local_generation_required': True,
                'contains_game_files': False, 'contains_game_derived_code': False, 'contains_compiled_binaries': False}),
        }
        self.manifest = {'schema': 1, 'project': 'Monado II', 'source_only': True, 'platform': 'windows',
                         'windows_implementation': update.IMPLEMENTATION, 'release_version': self.version}
        self.requests = []
        self.refresh()

    def refresh(self, *, status=True):
        self.manifest['files'] = {name: {'bytes': len(data), 'sha256': update._digest(data)}
                                  for name, data in self.files.items() if name != update.MANIFEST}
        self.files[update.MANIFEST] = encode(self.manifest)
        self.archive = self.pack(self.files)
        self.metadata = {'schema': 1, 'platform': update.PLATFORM, 'implementation': update.IMPLEMENTATION,
            'version': self.version, 'archive_name': 'monado-ii-windows-' + self.version + '-setup.zip',
            'archive_root': self.prefix, 'archive_sha256': update._digest(self.archive),
            'archive_bytes': len(self.archive), 'source_manifest_sha256': update._digest(self.files[update.MANIFEST])}
        self.metadata_bytes = encode(self.metadata)
        self.offer = dict(self.metadata, tag=self.tag, metadata_sha256=update._digest(self.metadata_bytes),
                          archive_url=update._download_url(self.tag, self.metadata['archive_name']))
        self.release = {'draft': False, 'prerelease': False, 'tag_name': self.tag,
                        'assets': [self.asset(update.METADATA_NAME, self.metadata_bytes),
                                   self.asset(self.metadata['archive_name'], self.archive)]}

    def asset(self, name, data):
        return {'name': name, 'state': 'uploaded', 'size': len(data), 'digest': 'sha256:' + update._digest(data),
                'browser_download_url': update._download_url(self.tag, name)}

    def pack(self, files):
        output = io.BytesIO()
        with zipfile.ZipFile(output, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
            for name, data in files.items():
                info = zipfile.ZipInfo(self.prefix + '/' + name)
                # ZipInfo normalizes native backslashes on Windows. Preserve
                # the authored raw archive spelling for negative fixtures.
                info.filename = info.orig_filename = self.prefix + '/' + name
                info.create_system = 3
                info.external_attr = (stat.S_IFREG | 0o644) << 16
                archive.writestr(info, data)
        return output.getvalue()

    def open(self, request, timeout):
        self.assertEqual(timeout, 30)
        url = request.full_url
        self.requests.append(url)
        data = {update.common.LATEST_URL: encode(self.release),
                update._download_url(self.tag, update.METADATA_NAME): self.metadata_bytes,
                self.offer['archive_url']: self.archive}[url]
        return Response(data, url)

    def test_independent_windows_version_and_asset_are_selected(self):
        self.assertEqual(update.discover('0.9.0', opener=self.open), self.offer)
        self.assertIsNone(update.discover('1.0.0', opener=self.open))
        self.assertEqual(len(self.requests), 4)

    def test_linux_and_original_windows_assets_are_never_updates(self):
        self.release['assets'] = [{'name': 'monado-ii-9.9.9-linux-setup.tar.gz'},
            {'name': 'monado-ii-windows-original-implementation-setup.zip'}]
        self.assertIsNone(update.discover(opener=self.open))
        self.assertEqual(self.requests, [update.common.LATEST_URL])

    def test_platform_metadata_asset_digest_and_official_url_fail_closed(self):
        for change in ('platform', 'digest', 'url', 'duplicate'):
            with self.subTest(change=change):
                self.refresh()
                if change == 'platform':
                    self.metadata['platform'] = 'linux'
                    self.metadata_bytes = encode(self.metadata)
                    self.release['assets'][0] = self.asset(update.METADATA_NAME, self.metadata_bytes)
                elif change == 'digest':
                    self.release['assets'][0]['digest'] = 'sha256:' + '0' * 64
                elif change == 'url':
                    self.release['assets'][1]['browser_download_url'] += '?other=1'
                else:
                    self.release['assets'].append(self.release['assets'][0])
                with self.assertRaises(update.UpdateError):
                    update.discover(opener=self.open)

    def test_archive_paths_binaries_and_case_collisions_are_rejected(self):
        for path in ('../outside', 'a/../outside', 'a\\outside', 'A/CON', 'a:stream',
                     'game_files/input.txt', 'Monado_Windows/build/thing.txt', 'surprise.exe'):
            with self.subTest(path=path):
                archive = self.pack({**self.files, path: b'authored'})
                with self.assertRaises(update.UpdateError):
                    update._zip_members(archive, self.offer, None)
        for names in (('a.txt', 'A.txt'), ('dir/one.txt', 'DIR/two.txt'), ('dir', 'dir/one.txt')):
            with self.subTest(names=names):
                archive = self.pack({**self.files, **{name: b'authored' for name in names}})
                with self.assertRaises(update.UpdateError):
                    update._zip_members(archive, self.offer, None)

    def test_symlink_and_duplicate_members_are_refused(self):
        output = io.BytesIO()
        with zipfile.ZipFile(output, 'w') as archive:
            info = zipfile.ZipInfo(self.prefix + '/linked.txt')
            info.create_system = 3
            info.external_attr = (stat.S_IFLNK | 0o777) << 16
            archive.writestr(info, 'outside')
        with self.assertRaises(update.UpdateError):
            update._zip_members(output.getvalue(), self.offer, None)

    @unittest.skipUnless(os.name == 'nt', 'native guarded Windows extraction')
    def test_native_extraction_guard_and_distribution_metadata(self):
        source = update.fetch_and_extract(self.offer, self.root / 'new', opener=self.open)
        self.assertEqual(update.verified_source(source, self.offer)['release_version'], self.version)
        self.assertTrue((source.parent / 'extraction.json').is_file())
        (source / 'Monado_Windows/runtime/example.c').write_bytes(b'changed after verification\n')
        with self.assertRaises(update.UpdateError):
            update.verified_source(source, self.offer)

    @unittest.skipUnless(os.name == 'nt', 'native guarded Windows extraction')
    def test_corrupt_inner_manifest_and_wrong_distribution_are_rejected(self):
        for kind in ('changed', 'platform', 'status'):
            with self.subTest(kind=kind):
                self.refresh()
                if kind == 'changed':
                    self.files['README.md'] += b'changed'
                elif kind == 'platform':
                    self.manifest['platform'] = 'linux'
                    self.files[update.MANIFEST] = encode(self.manifest)
                else:
                    self.files['source-only-status.json'] = encode({'schema': 1})
                    self.refresh()
                self.archive = self.pack(self.files)
                self.offer.update(archive_sha256=update._digest(self.archive), archive_bytes=len(self.archive),
                                  source_manifest_sha256=update._digest(self.files[update.MANIFEST]))
                with self.assertRaises(update.UpdateError):
                    update.fetch_and_extract(self.offer, self.root / kind, opener=self.open)

    def test_existing_destination_and_cancellation_preserve_human_data(self):
        directory = self.root / 'human'
        directory.mkdir()
        original = directory / 'notes.txt'
        original.write_bytes(b'human authored data')
        with self.assertRaises(update.UpdateError):
            update.fetch_and_extract(self.offer, directory, opener=self.open)
        with self.assertRaises(update.UpdateCancelled):
            update.fetch_and_extract(self.offer, self.root / 'cancelled', opener=self.open, cancel=lambda: True)
        self.assertEqual(original.read_bytes(), b'human authored data')
        self.assertFalse((self.root / 'cancelled').exists())


if __name__ == '__main__':
    unittest.main()
