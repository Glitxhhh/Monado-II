"""Source updater trust-boundary fixtures; no game input or real network."""
import copy
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import sys
import tarfile
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
spec = importlib.util.spec_from_file_location('update_release', ROOT / 'tools/update_release.py')
update = importlib.util.module_from_spec(spec)
spec.loader.exec_module(update)


def encoded(document):
    return (json.dumps(document, sort_keys=True) + '\n').encode()


def identity(data):
    return dict(bytes=len(data), sha256=hashlib.sha256(data).hexdigest())


class Response(io.BytesIO):
    def __init__(self, data, url):
        super().__init__(data)
        self.url = url

    def geturl(self):
        return self.url

    def getcode(self):
        return 200


def response(data, *, final_url=None):
    def opener(request, timeout):
        assert timeout == 30
        return Response(data, final_url or request.full_url)
    return opener


def source_files(version='0.1.4'):
    files = {
        'README.md': b'# Authored updater fixture\n',
        'Monado_Linux/runtime/fixture.c': b'int authored_fixture(void) { return 7; }\n',
        'publication-policy.json': encoded(dict(schema=1, licensed_media={})),
        'update-version.json': encoded(dict(schema=1, protocol=1, version=version)),
        'source-only-status.json': encoded(dict(schema=1, project='Monado II', platform='linux',
            version=version, kind='source-only', local_generation_required=True,
            contains_game_files=False, contains_game_derived_code=False, contains_compiled_binaries=False)),
    }
    return files


def manifest(files, version='0.1.4'):
    return dict(schema=1, project='Monado II', source_only=True, platform='linux',
                release_version=version, release_kind='source-only',
                files={name: identity(data) for name, data in files.items()})


def archive_bytes(files=None, *, version='0.1.4', extra=(), document=None):
    files = source_files(version) if files is None else files.copy()
    files[update.MANIFEST] = encoded(document or manifest(files, version))
    result = io.BytesIO()
    with tarfile.open(fileobj=result, mode='w:gz', format=tarfile.PAX_FORMAT) as archive:
        for name, data in files.items():
            info = tarfile.TarInfo('monado-ii-' + version + '-linux-source/' + name)
            info.mode = 0o755 if name.endswith('.sh') else 0o644
            info.size = len(data)
            archive.addfile(info, io.BytesIO(data))
        for name, kind, data in extra:
            info = tarfile.TarInfo(name)
            info.mode, info.type = 0o644, kind
            if kind == tarfile.REGTYPE:
                info.size = len(data)
            else:
                info.linkname = data.decode()
            archive.addfile(info, io.BytesIO(data) if kind == tarfile.REGTYPE else None)
    return result.getvalue()


def offer(data, version='0.1.4'):
    return dict(version=version, tag='v' + version,
                archive_url='https://github.com/' + update.REPOSITORY + '/releases/download/v' + version +
                            '/monado-ii-' + version + '-linux-source.tar.gz',
                archive_sha256=hashlib.sha256(data).hexdigest(), archive_bytes=len(data))


def release(version='0.1.4'):
    selected = offer(b'fixture', version)
    return dict(tag_name=selected['tag'], draft=False, prerelease=False, assets=[dict(
        name='monado-ii-' + version + '-linux-source.tar.gz', state='uploaded', size=selected['archive_bytes'],
        digest='sha256:' + selected['archive_sha256'], browser_download_url=selected['archive_url'])])


class Discovery(unittest.TestCase):
    def test_newer_stable_release_has_exact_pinned_offer(self):
        self.assertEqual(update.discover('0.1.3', opener=response(encoded(release()))), offer(b'fixture'))

    def test_no_newer_release_is_silent(self):
        for current in ('0.1.4', '0.2.0', '1.0.0'):
            with self.subTest(current=current):
                self.assertIsNone(update.discover(current, opener=response(encoded(release()))))

    def test_invalid_or_prerelease_versions_fail_closed(self):
        for value in ('0.1.4-beta', '01.1.4', '../0.1.4', 'v0.1.4', ''):
            with self.subTest(value=value), self.assertRaises(update.UpdateError):
                update.discover(value, opener=response(encoded(release())))
        for field in ('draft', 'prerelease'):
            document = release(); document[field] = True
            with self.subTest(field=field), self.assertRaises(update.UpdateError):
                update.discover('0.1.3', opener=response(encoded(document)))

    def test_missing_duplicate_or_unpinned_asset_is_rejected(self):
        cases = []
        document = release(); document['assets'] = []; cases.append(document)
        document = release(); document['assets'] *= 2; cases.append(document)
        for field, value in (('digest', None), ('digest', 'sha256:' + '0' * 63),
                             ('size', True), ('size', update.MAX_ARCHIVE + 1),
                             ('state', 'new'), ('browser_download_url', 'https://example.com/file')):
            document = release(); document['assets'][0][field] = value; cases.append(document)
        for document in cases:
            with self.subTest(document=document), self.assertRaises(update.UpdateError):
                update.discover('0.1.3', opener=response(encoded(document)))

    def test_untrusted_redirect_is_rejected_before_following(self):
        redirect = update._Redirect()
        request = update.urllib.request.Request(update.LATEST_URL)
        for url in ('http://github.com/file', 'https://evil.example/file',
                    'https://github.com@evil.example/file', 'https://github.com:444/file'):
            with self.subTest(url=url), self.assertRaises(update.UpdateError):
                redirect.redirect_request(request, None, 302, '', {}, url)
        with self.assertRaises(update.UpdateError):
            update.discover('0.1.3', opener=response(encoded(release()), final_url='https://evil.example/file'))

    def test_metadata_size_and_duplicate_json_keys_are_bounded(self):
        with self.assertRaisesRegex(update.UpdateError, 'oversized'):
            update.discover('0.1.3', opener=response(b' ' * (update.MAX_METADATA + 1)))
        with self.assertRaises(update.UpdateError):
            update.discover('0.1.3', opener=response(b'{"draft":false,"draft":false}'))


class Extraction(unittest.TestCase):
    def setUp(self):
        context = tempfile.TemporaryDirectory(prefix='monado-update-release-')
        self.addCleanup(context.cleanup)
        self.root = Path(context.name)

    def fetch(self, data, *, selected=None, **kwargs):
        return update.fetch_and_extract(selected or offer(data), self.root / 'stage',
                                        opener=response(data), **kwargs)

    def test_real_source_packager_archive_roundtrip(self):
        # Exercise the actual release writer with small authored source only.
        pack_spec = importlib.util.spec_from_file_location('update_packager', ROOT / 'tools/package_source_release.py')
        packager = importlib.util.module_from_spec(pack_spec); pack_spec.loader.exec_module(packager)
        source = self.root / 'authored'; source.mkdir()
        files = source_files()
        files.pop('source-only-status.json')  # The real packager authors this release-only file.
        for name, data in {**files, update.MANIFEST: encoded(manifest(files))}.items():
            path = source / name; path.parent.mkdir(parents=True, exist_ok=True); path.write_bytes(data)
        receipt = packager.package(source, 'linux', '0.1.4', self.root / 'package')
        data = (self.root / 'package' / receipt['archive']).read_bytes()
        progress = []
        extracted = self.fetch(data, progress=progress.append)
        self.assertEqual(extracted, self.root / 'stage/source')
        self.assertEqual((extracted / 'README.md').read_bytes(), files['README.md'])
        self.assertEqual(update.verified_source(extracted)['release_version'], '0.1.4')
        self.assertTrue(any(p['phase'] == 'download' for p in progress))
        self.assertTrue(any(p['phase'] == 'extract' for p in progress))

    def test_pinned_size_hash_and_truncation_are_checked(self):
        data = archive_bytes()
        for field, value in (('archive_sha256', 'f' * 64), ('archive_bytes', len(data) - 1),
                             ('archive_bytes', len(data) + 1)):
            with self.subTest(field=field, value=value):
                context = tempfile.TemporaryDirectory(dir=self.root)
                self.addCleanup(context.cleanup)
                selected = offer(data); selected[field] = value
                with self.assertRaises(update.UpdateError):
                    update.fetch_and_extract(selected, Path(context.name) / 'stage', opener=response(data))
                self.assertFalse((Path(context.name) / 'stage/source').exists())

    def test_links_traversal_duplicates_collisions_and_other_roots_are_rejected(self):
        prefix = 'monado-ii-0.1.4-linux-source/'
        cases = [(prefix + '../escape', tarfile.REGTYPE, b'bad'),
                 (prefix + 'link', tarfile.SYMTYPE, b'../../escape'),
                 (prefix + 'link', tarfile.LNKTYPE, b'README.md'),
                 (prefix + 'device', tarfile.CHRTYPE, b''),
                 (prefix + 'README.md', tarfile.REGTYPE, b'duplicate'),
                 (prefix + 'readme.MD', tarfile.REGTYPE, b'collision'),
                 ('other-root/file.txt', tarfile.REGTYPE, b'wrong root')]
        for member in cases:
            with self.subTest(member=member):
                context = tempfile.TemporaryDirectory(dir=self.root); self.addCleanup(context.cleanup)
                data = archive_bytes(extra=[member])
                with self.assertRaises(update.UpdateError):
                    update.fetch_and_extract(offer(data), Path(context.name) / 'stage', opener=response(data))
        self.assertFalse((self.root / 'escape').exists())

    def test_reserved_installation_marker_is_rejected_even_when_manifested(self):
        files = source_files(); files['installation.json'] = b'{}\n'
        with self.assertRaisesRegex(update.UpdateError, 'reserved'):
            self.fetch(archive_bytes(files))

    def test_file_directory_and_directory_case_collisions_are_rejected(self):
        for first, second in (('file', 'file/child.py'), ('Tree/first.py', 'tree/second.py')):
            with self.subTest(first=first, second=second):
                context = tempfile.TemporaryDirectory(dir=self.root); self.addCleanup(context.cleanup)
                files = source_files(); files[first] = b'# authored\n'; files[second] = b'# authored\n'
                data = archive_bytes(files)
                with self.assertRaisesRegex(update.UpdateError, 'collide'):
                    update.fetch_and_extract(offer(data), Path(context.name) / 'stage', opener=response(data))
                self.assertFalse((Path(context.name) / 'stage/source').exists())

    def test_malformed_gzip_is_reported_as_an_update_error(self):
        with self.assertRaises(update.UpdateError):
            self.fetch(b'not a gzip source archive')

    def test_source_manifest_hash_metadata_and_member_set_are_verified(self):
        cases = []
        files = source_files(); document = manifest(files)
        document['files']['README.md']['sha256'] = 'f' * 64
        cases.append(archive_bytes(files, document=document))
        document = manifest(files); document['release_version'] = '0.1.5'
        cases.append(archive_bytes(files, document=document))
        document = manifest(files); document['files'].pop('README.md')
        cases.append(archive_bytes(files, document=document))
        files = source_files(); files['source-only-status.json'] = encoded(dict(version='0.1.5'))
        cases.append(archive_bytes(files))
        files = source_files(); files['update-version.json'] = encoded(dict(schema=1, protocol=2, version='0.1.4'))
        cases.append(archive_bytes(files))
        for data in cases:
            with self.subTest(digest=hashlib.sha256(data).hexdigest()):
                context = tempfile.TemporaryDirectory(dir=self.root); self.addCleanup(context.cleanup)
                with self.assertRaises(update.UpdateError):
                    update.fetch_and_extract(offer(data), Path(context.name) / 'stage', opener=response(data))

    def test_installed_guard_rejects_disguised_binary_content(self):
        files = source_files(); files['README.md'] = b'NSO0AUTHORED NEGATIVE FIXTURE'
        with self.assertRaisesRegex(update.UpdateError, 'publication guard'):
            self.fetch(archive_bytes(files))

    def test_downloaded_guard_is_never_executed(self):
        files = source_files()
        files['tools/publication_guard.py'] = b'raise RuntimeError("downloaded code executed")\n'
        extracted = self.fetch(archive_bytes(files))
        self.assertEqual((extracted / 'tools/publication_guard.py').read_bytes(),
                         files['tools/publication_guard.py'])

    def test_member_count_file_size_and_expansion_limits(self):
        for constant, value in (('MAX_MEMBERS', 2), ('MAX_FILE', 10), ('MAX_EXTRACTED', 20), ('MAX_TAR', 512)):
            with self.subTest(constant=constant), mock.patch.object(update, constant, value):
                context = tempfile.TemporaryDirectory(dir=self.root); self.addCleanup(context.cleanup)
                data = archive_bytes()
                with self.assertRaises(update.UpdateError):
                    update.fetch_and_extract(offer(data), Path(context.name) / 'stage', opener=response(data))

    def test_existing_linked_or_noncanonical_staging_is_refused(self):
        data = archive_bytes()
        existing = self.root / 'existing'; existing.mkdir()
        link = self.root / 'link'; link.symlink_to(existing, target_is_directory=True)
        for path in (existing, link, link / 'stage'):
            with self.subTest(path=path), self.assertRaises(update.UpdateError):
                update.fetch_and_extract(offer(data), path, opener=response(data))

    def test_cancel_before_network_and_during_download_preserves_external_files(self):
        sentinel = self.root / 'save.sentinel'; sentinel.write_bytes(b'keep')
        with self.assertRaises(update.UpdateCancelled):
            update.fetch_and_extract(offer(b'x'), self.root / 'stage',
                                     opener=mock.Mock(side_effect=AssertionError('no network')), cancel=lambda: True)
        self.assertFalse((self.root / 'stage').exists())
        cancelled = []
        with self.assertRaises(update.UpdateCancelled):
            self.fetch(archive_bytes(), progress=lambda p: cancelled.append(p), cancel=lambda: bool(cancelled))
        self.assertEqual(sentinel.read_bytes(), b'keep')
        self.assertFalse((self.root / 'stage/source').exists())

    def test_cancellation_during_extraction_cannot_report_a_verified_candidate(self):
        extracted = []
        with self.assertRaises(update.UpdateCancelled):
            self.fetch(archive_bytes(), progress=lambda p: extracted.append(p) if p['phase'] == 'extract' else None,
                       cancel=lambda: bool(extracted))
        self.assertTrue(extracted)


class Classification(unittest.TestCase):
    def base(self):
        return manifest(source_files())

    def changed(self, name, *, delete=False):
        initial = self.base(); candidate = copy.deepcopy(initial)
        initial['files'][name] = identity(b'old')
        if delete:
            candidate['files'].pop(name, None)
        else:
            candidate['files'][name] = identity(b'new')
        return initial, candidate

    def test_only_known_launcher_presentation_and_metadata_paths_reuse_game(self):
        for name in ('Monado_Linux/tools/launcher/launcher.cpp',
                     'Monado_Linux/tools/launcher/assets/gaur_plain.png',
                     'Monado_Linux/scripts/build_launcher_linux.py', 'tools/update_linux.py',
                     'tools/update_release.py', 'tests/test_update_release.py', 'update-version.json',
                     'tools/appimage/driver.py', 'docs/BUILD_LINUX.md', 'NOTICE.md'):
            for delete in (False, True):
                with self.subTest(name=name, delete=delete):
                    self.assertEqual(update.classify(*self.changed(name, delete=delete)), 'launcher')

    def test_unknown_runtime_generator_header_and_dependency_changes_require_game(self):
        for name in ('Monado_Linux/runtime/fixture.c', 'Monado_Linux/runtime/cpu.h',
                     'Monado_Linux/tools/lifter/gen.py', 'Monado_Linux/games/xcde/patches.py',
                     'Monado_Linux/scripts/run_linux.py', 'Monado_Linux/scripts/install_layout.py',
                     'Monado_Linux/scripts/build_linux.py', 'Monado_Linux/dlss-linux.lock.json',
                     'unknown.py', 'tools/build_linux_app.py'):
            with self.subTest(name=name):
                self.assertEqual(update.classify(*self.changed(name)), 'game')

    def test_windows_changes_are_ignored_and_identical_tree_reuses_game(self):
        self.assertEqual(update.classify(self.base(), self.base()), 'launcher')
        self.assertEqual(update.classify(*self.changed('Monado_Windows/runtime/cpu.h')), 'launcher')

    def test_invalid_manifest_identity_is_not_classified_as_safe(self):
        candidate = self.base(); candidate['files']['README.md']['sha256'] = 'not a hash'
        with self.assertRaises(update.UpdateError):
            update.classify(self.base(), candidate)


if __name__ == '__main__':
    unittest.main()
