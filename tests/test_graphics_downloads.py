"""Bounded official dependency downloads/cache reuse; authored bytes, no network."""
import contextlib
import errno
import hashlib
import http.client
import importlib.util
import io
import json
import os
from pathlib import Path
import ssl
import stat
import tempfile
import unittest
from unittest import mock
import urllib.error
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('graphics_download_fixtures',
    ROOT / 'Monado_Linux/scripts/setup_linux_graphics.py')
setup = importlib.util.module_from_spec(spec)
spec.loader.exec_module(setup)
URL = 'https://archive.ubuntu.com/ubuntu/pool/main/a/authored/fixture.deb'


class Response(io.BytesIO):
    def __init__(self, data, *, url=URL, length=None):
        super().__init__(data)
        self.url = url
        self.headers = {} if length is None else {'Content-Length': str(length)}

    def geturl(self):
        return self.url


class BrokenResponse(Response):
    def read(self, count=-1):
        if self.tell():
            raise http.client.IncompleteRead(b'', 5)
        return super().read(min(5, count))


class GraphicsDownloads(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='monado-graphics-downloads-')
        self.addCleanup(temporary.cleanup)
        self.base = Path(temporary.name).resolve()
        self.owner = self.base / 'My Games/Monado II'
        self.owner_port = self.owner / 'Monado_Linux'
        self.owner_port.mkdir(parents=True)
        self.port = self.owner_port
        self.cache = self.port / '.deps/graphics/debs'
        self.cache.mkdir(parents=True)
        self.data = b'AUTHORED DEPENDENCY DOWNLOAD FIXTURE, NOT AN ARCHIVE\n'
        self.package = {'filename': 'fixture.deb', 'url': URL, 'size': len(self.data),
                        'sha256': hashlib.sha256(self.data).hexdigest()}
        patch = mock.patch.object(setup, 'ROOT', self.port)
        patch.start(); self.addCleanup(patch.stop)
        patch = mock.patch.object(setup, 'open_package', side_effect=AssertionError('unexpected network'))
        self.opener = patch.start(); self.addCleanup(patch.stop)
        patch = mock.patch.object(setup.time, 'sleep')
        self.sleep = patch.start(); self.addCleanup(patch.stop)

    def fetch(self, *, offline=False):
        with contextlib.redirect_stdout(io.StringIO()) as log:
            result = setup.fetch(self.package, self.cache, offline)
        self.log = log.getvalue()
        return result

    def target(self):
        return self.cache / self.package['filename']

    def no_partials(self):
        self.assertEqual([p.name for p in self.cache.iterdir() if p.name.endswith('.partial')], [])

    def managed(self):
        self.port = self.owner_port / 'updates/update-authored/workspace/Monado_Linux'
        self.port.mkdir(parents=True)
        (self.port.parent / 'installation.json').write_text(json.dumps({'schema': 1,
            'owner': str(self.owner), 'workspace': str(self.port.parent), 'game_port': str(self.port)}))
        self.cache = self.port / '.deps/graphics/debs'
        self.cache.mkdir(parents=True)
        setup.ROOT = self.port
        return self.owner_port / '.deps/graphics/debs/fixture.deb'

    def snapshot(self, directory):
        return {p.relative_to(directory).as_posix(): (p.read_bytes(), p.stat().st_mtime_ns,
                p.stat().st_ctime_ns, p.stat().st_nlink, stat.S_IMODE(p.stat().st_mode))
                for p in directory.rglob('*') if p.is_file() and not p.is_symlink()}

    def test_verified_local_cache_is_read_only_and_needs_no_network(self):
        self.target().write_bytes(self.data)
        before = self.snapshot(self.cache)
        self.assertEqual(self.fetch(offline=True), self.target())
        self.assertEqual(self.snapshot(self.cache), before)
        self.opener.assert_not_called(); self.sleep.assert_not_called()

    def test_initial_download_is_verified_and_leaves_no_partial(self):
        response = Response(self.data, length=len(self.data))
        self.opener.side_effect = [response]
        self.assertEqual(self.fetch().read_bytes(), self.data)
        self.assertTrue(response.closed)
        self.sleep.assert_not_called(); self.no_partials()
        self.assertEqual(self.target().stat().st_nlink, 1)

    def test_handshake_timeout_retries_with_new_clean_partials(self):
        self.opener.side_effect = [urllib.error.URLError(TimeoutError('SSL handshake timed out')),
                                   TimeoutError('temporary timeout'), Response(self.data)]
        paths = []
        create = setup.tempfile.mkstemp
        def temporary(**kwargs):
            descriptor, name = create(**kwargs); paths.append(name)
            return descriptor, name
        self.sleep.side_effect = lambda delay: self.assertEqual(list(self.cache.iterdir()), [])
        with mock.patch.object(setup.tempfile, 'mkstemp', side_effect=temporary):
            self.assertEqual(self.fetch().read_bytes(), self.data)
        self.assertEqual(self.opener.call_count, 3)
        self.assertEqual(self.sleep.call_args_list, [mock.call(1), mock.call(2)])
        self.assertEqual(len(set(paths)), 3)
        self.no_partials()

    def test_read_exception_and_short_clean_eof_retry_before_fresh_complete_response(self):
        self.opener.side_effect = [BrokenResponse(self.data), Response(self.data[:8]), Response(self.data)]
        self.assertEqual(self.fetch().read_bytes(), self.data)
        self.assertEqual(self.opener.call_count, 3)
        self.no_partials()

    def test_exhaustion_is_bounded_and_keeps_cache_empty(self):
        self.opener.side_effect = urllib.error.URLError(TimeoutError('handshake timeout'))
        with self.assertRaisesRegex(RuntimeError, r'fixture\.deb after 3 attempts') as failure:
            self.fetch()
        self.assertIsInstance(failure.exception.__cause__, urllib.error.URLError)
        self.assertEqual(self.opener.call_count, 3)
        self.assertEqual(self.sleep.call_args_list, [mock.call(1), mock.call(2)])
        self.assertEqual(list(self.cache.iterdir()), [])

    def test_retryable_http_and_connection_failures_are_bounded(self):
        errors = [urllib.error.HTTPError(URL, code, 'authored response', {}, None)
                  for code in (408, 429, 500, 502, 503, 504)]
        errors += [ConnectionResetError('reset'), ssl.SSLEOFError('unexpected TLS EOF')]
        for error in errors:
            with self.subTest(error=error):
                self.opener.reset_mock(); self.sleep.reset_mock()
                self.opener.side_effect = [error, Response(self.data)]
                self.assertEqual(self.fetch().read_bytes(), self.data)
                self.assertEqual(self.opener.call_count, 2)
                self.sleep.assert_called_once_with(1)
                self.target().unlink(); self.no_partials()

    def test_permanent_http_certificate_and_unknown_tls_errors_fail_fast(self):
        errors = [urllib.error.HTTPError(URL, code, 'authored response', {}, None)
                  for code in (400, 401, 403, 404, 410)]
        errors += [ssl.SSLCertVerificationError('bad certificate'),
                   urllib.error.URLError(ssl.SSLCertVerificationError('bad certificate')),
                   ssl.SSLError('permanent TLS configuration error')]
        for error in errors:
            with self.subTest(error=error):
                self.opener.reset_mock(); self.sleep.reset_mock(); self.opener.side_effect = error
                with self.assertRaises(type(error)):
                    self.fetch()
                self.opener.assert_called_once_with(URL)
                self.sleep.assert_not_called(); self.assertEqual(list(self.cache.iterdir()), [])

    def test_bad_hash_oversize_and_wrong_content_length_fail_fast(self):
        for response in (Response(b'x' * len(self.data)), Response(self.data + b'extra'),
                         Response(self.data, length=len(self.data) - 1)):
            with self.subTest(response=response):
                self.opener.reset_mock(); self.sleep.reset_mock(); self.opener.side_effect = [response]
                with self.assertRaisesRegex(ValueError, 'hash/size|Content-Length'):
                    self.fetch()
                self.assertEqual(self.opener.call_count, 1)
                self.sleep.assert_not_called(); self.assertEqual(list(self.cache.iterdir()), [])

    def test_filesystem_failures_never_enter_network_retry(self):
        for code in (errno.EACCES, errno.ENOSPC, errno.ETIMEDOUT):
            with self.subTest(code=code):
                self.opener.reset_mock(); self.sleep.reset_mock(); self.opener.side_effect = [Response(self.data)]
                with mock.patch.object(setup.os, 'fsync', side_effect=OSError(code, 'authored disk failure')):
                    with self.assertRaises(OSError):
                        self.fetch()
                self.assertEqual(self.opener.call_count, 1)
                self.sleep.assert_not_called(); self.assertEqual(list(self.cache.iterdir()), [])

    def test_cache_appearance_during_download_is_not_overwritten(self):
        def appeared(_):
            self.target().write_bytes(b'PRESERVE OTHER WRITER')
            return Response(self.data)
        self.opener.side_effect = appeared
        with self.assertRaises(FileExistsError):
            self.fetch()
        self.assertEqual(self.target().read_bytes(), b'PRESERVE OTHER WRITER')
        self.sleep.assert_not_called(); self.no_partials()

    def test_offline_missing_cache_never_opens_network(self):
        with self.assertRaisesRegex(FileNotFoundError, 'offline package missing'):
            self.fetch(offline=True)
        self.opener.assert_not_called(); self.assertEqual(list(self.cache.iterdir()), [])

    def test_invalid_target_cache_is_preserved_and_never_falls_back(self):
        old = self.managed(); old.write_bytes(self.data)
        self.target().write_bytes(b'CORRUPT LOCAL CACHE')
        with self.assertRaisesRegex(ValueError, 'preserve and investigate'):
            self.fetch()
        self.assertEqual(self.target().read_bytes(), b'CORRUPT LOCAL CACHE')
        self.assertEqual(old.read_bytes(), self.data)
        self.opener.assert_not_called(); self.no_partials()

    def test_verified_owner_cache_is_copied_offline_without_changing_original_state(self):
        original = self.managed(); original.write_bytes(self.data)
        protected = self.owner_port / 'play/launcher/save'
        protected.mkdir(parents=True); (protected / 'sentinel').write_bytes(b'PRESERVE SAVE')
        output = self.owner_port / 'build/accepted'
        output.mkdir(parents=True); (output / 'sentinel').write_bytes(b'PRESERVE BUILD')
        originals = self.snapshot(original.parent), self.snapshot(protected), self.snapshot(output)
        result = self.fetch(offline=True)
        self.assertEqual(result.read_bytes(), self.data)
        self.assertNotEqual((result.stat().st_dev, result.stat().st_ino),
                            (original.stat().st_dev, original.stat().st_ino))
        self.assertEqual(result.stat().st_nlink, 1); self.assertEqual(original.stat().st_nlink, 1)
        self.assertEqual(originals, (self.snapshot(original.parent), self.snapshot(protected), self.snapshot(output)))
        self.assertIn('reused verified original-installation', self.log)
        self.opener.assert_not_called(); self.sleep.assert_not_called(); self.no_partials()

    def test_missing_owner_package_downloads_or_reports_offline_missing(self):
        self.managed()
        with self.assertRaises(FileNotFoundError): self.fetch(offline=True)
        self.opener.assert_not_called()
        self.opener.side_effect = [Response(self.data)]
        self.assertEqual(self.fetch().read_bytes(), self.data)

    def test_corrupt_original_optional_cache_is_preserved_and_refused(self):
        original = self.managed(); original.write_bytes(b'CORRUPT ORIGINAL CACHE')
        before = self.snapshot(original.parent)
        with self.assertRaisesRegex(ValueError, 'preserve and investigate'):
            self.fetch()
        self.assertEqual(self.snapshot(original.parent), before)
        self.assertEqual(list(self.cache.iterdir()), []); self.opener.assert_not_called()

    def test_other_unbound_caches_are_not_searched(self):
        unrelated = self.base / 'unrelated/.deps/graphics/debs'
        unrelated.mkdir(parents=True); (unrelated / 'fixture.deb').write_bytes(self.data)
        with self.assertRaises(FileNotFoundError): self.fetch(offline=True)
        self.assertEqual((unrelated / 'fixture.deb').read_bytes(), self.data)

    def test_invalid_binding_does_not_read_or_copy_external_cache(self):
        original = self.managed(); original.write_bytes(self.data)
        marker = self.port.parent / 'installation.json'
        value = json.loads(marker.read_text()); value['owner'] = str(self.base)
        marker.write_text(json.dumps(value))
        with self.assertRaises(ValueError): self.fetch()
        self.assertEqual(list(self.cache.iterdir()), []); self.opener.assert_not_called()

    @unittest.skipUnless(hasattr(os, 'symlink'), 'host link support required')
    def test_source_and_destination_cache_links_are_rejected(self):
        original = self.managed()
        external = self.base / 'external.deb'; external.write_bytes(self.data)
        try: original.symlink_to(external)
        except OSError as error: self.skipTest(str(error))
        with self.assertRaisesRegex(ValueError, 'cached package type'): self.fetch()
        self.assertEqual(external.read_bytes(), self.data)
        original.unlink(); original.write_bytes(self.data)
        self.target().symlink_to(external)
        with self.assertRaisesRegex(RuntimeError, 'symlink'): self.fetch()
        self.assertEqual(external.read_bytes(), self.data); self.no_partials()
        self.opener.assert_not_called()

    @unittest.skipUnless(hasattr(os, 'link'), 'host hard-link support required')
    def test_hard_linked_original_and_target_cache_are_refused(self):
        original = self.managed(); original.write_bytes(self.data)
        external = self.base / 'linked.deb'
        try: os.link(original, external)
        except OSError as error: self.skipTest(str(error))
        with self.assertRaisesRegex(ValueError, 'cached package type'): self.fetch()
        external.unlink()
        os.link(original, self.target())
        with self.assertRaisesRegex(ValueError, 'cached package type'): self.fetch()
        self.assertEqual(original.read_bytes(), self.data); self.no_partials()
        self.opener.assert_not_called()

    @unittest.skipUnless(hasattr(os, 'symlink'), 'host link support required')
    def test_linked_owner_cache_directory_is_refused_without_external_reads(self):
        self.managed()
        old_cache = self.owner_port / '.deps/graphics/debs'
        old_cache.rmdir(); external = self.base / 'external-cache'; external.mkdir()
        (external / 'fixture.deb').write_bytes(self.data)
        try: old_cache.symlink_to(external, target_is_directory=True)
        except OSError as error: self.skipTest(str(error))
        with self.assertRaisesRegex(ValueError, 'ordinary directories'): self.fetch()
        self.assertEqual(list(self.cache.iterdir()), []); self.opener.assert_not_called()

    def test_initial_and_response_https_origins_are_required(self):
        for url in ('http://archive.ubuntu.com/ubuntu/pool/a.deb', 'https://evil.example/ubuntu/pool/a.deb',
                    'https://archive.ubuntu.com/not-pool/a.deb', 'https://user@archive.ubuntu.com/ubuntu/pool/a.deb',
                    'https://archive.ubuntu.com:444/ubuntu/pool/a.deb'):
            with self.subTest(url=url):
                self.package['url'] = url
                with self.assertRaisesRegex(ValueError, 'origin'): self.fetch()
        self.opener.assert_not_called(); self.package['url'] = URL
        self.opener.side_effect = [Response(self.data, url='https://evil.example/ubuntu/pool/a.deb')]
        with self.assertRaisesRegex(ValueError, 'origin'): self.fetch()
        self.assertEqual(self.opener.call_count, 1); self.sleep.assert_not_called(); self.no_partials()

    def test_redirect_handler_rejects_foreign_or_insecure_destination_before_following(self):
        handler = setup.ApprovedRedirect()
        request = urllib.request.Request(URL)
        for url in ('http://archive.ubuntu.com/ubuntu/pool/a.deb', 'https://evil.example/ubuntu/pool/a.deb'):
            with self.subTest(url=url), self.assertRaisesRegex(ValueError, 'origin'):
                handler.redirect_request(request, None, 302, 'fixture', {}, url)
        self.assertEqual(handler.redirect_request(request, None, 302, 'fixture', {}, URL).full_url, URL)

    def test_real_opener_keeps_fixed_timeout_and_guarded_redirects(self):
        # setUp replaces open_package; load a separate module for this one call.
        original_spec = importlib.util.spec_from_file_location('graphics_opener_fixture',
            ROOT / 'Monado_Linux/scripts/setup_linux_graphics.py')
        original = importlib.util.module_from_spec(original_spec); original_spec.loader.exec_module(original)
        with mock.patch.object(original.urllib.request, 'build_opener') as factory:
            original.open_package(URL)
        self.assertIsInstance(factory.call_args.args[0], original.ApprovedRedirect)
        factory.return_value.open.assert_called_once_with(URL, timeout=30)


if __name__ == '__main__':
    unittest.main()
