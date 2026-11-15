"""Native authored cleanup fixtures; no game inputs or existing installations."""
import contextlib
import hashlib
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'tools'))
if os.name == 'nt':
    import _winapi
    import update_windows_retention as retention
    import windows_file_guard as guard


@unittest.skipUnless(os.name == 'nt', 'Native Win32 ownership/handle cleanup fixture')
class Retention(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='monado-retention-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.owner = self.root/'owned install 雪 $() `literal`'
        self.authorities = {}
        (self.owner/'updates').mkdir(parents=True)
        self.input = self.root/'authored input'
        (self.input/'exefs').mkdir(parents=True)
        (self.input/'romfs').mkdir()
        (self.input/'exefs/main').write_bytes(b'authored, never executed')
        (self.input/'romfs/sentinel').write_bytes(b'preserve original input')
        self.current = self.owner/'updates/windows-99999999-abcdef01'
        self.current.mkdir()
        (self.current/'sentinel').write_bytes(b'current runtime is preserved')
        self.profile = self.owner/'profile'
        self.profile.mkdir()
        (self.profile/'save.sentinel').write_bytes(b'preserve used save')

    def candidate(self, number=1):
        candidate = self.owner/f'updates/windows-{number}-abcdef01'
        source = candidate/'source'
        source.mkdir(parents=True)
        files = {'publication-manifest.json': b'{"authored":true}\n',
                 'Monado_Windows/runtime/authored.c': b'/* Authored fixture. */\n'}
        identities = {}
        for name, raw in files.items():
            path = source/name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(raw)
            identities[name] = {'bytes': len(raw), 'sha256': hashlib.sha256(raw).hexdigest()}
        (candidate/'source.zip').write_bytes(b'authored verified-download boundary fixture')
        output = source/f'Monado_Windows/build/native-{number}-abcdef01'
        (output/'build').mkdir(parents=True)
        (output/'build/xcde.exe').write_bytes(b'authored binary fixture, never executed')
        (output/'build/xcde_launcher.exe').write_bytes(b'authored launcher fixture, never executed')
        (output/'build-receipt.json').write_bytes(b'{"authored":"completed build boundary"}\n')
        (output/'generated').mkdir()
        (output/'generated/authored.c').write_text('/* No game code. */\n')
        (output/'targets').mkdir()
        links = {}
        for name in ('exefs', 'romfs'):
            link = output/'targets'/name
            _winapi.CreateJunction(str(self.input/name), str(link))
            links[str(link)] = str(self.input/name)
        cache = source/'Monado_Windows/.deps/zig-cache'
        cache.mkdir(parents=True)
        (cache/'authored-cache').write_bytes(b'authored temporary compiler output')
        artifacts = {}
        for name in ('build/xcde.exe', 'build/xcde_launcher.exe', 'generated/authored.c', 'build-receipt.json'):
            raw = (output/name).read_bytes()
            artifacts[name] = {'bytes': len(raw), 'sha256': hashlib.sha256(raw).hexdigest()}
        kwargs = dict(source_files=identities, build_root=output, build_files=artifacts,
                      input_links=links, derived_roots=[cache])
        return candidate, kwargs

    def prepare(self, number=1):
        candidate, kwargs = self.candidate(number)
        token = retention.prepare(self.owner, candidate, **kwargs)
        self.authorities[token['candidate']] = token['manifest_sha256']
        return candidate

    def cleanup(self, **kwargs):
        return retention.cleanup(self.owner, current=self.current, protected=[self.profile],
                                 authorities=self.authorities,
                                 idle=lambda root: None, selection_unchanged=lambda: None, **kwargs)

    def assert_preserved(self):
        self.assertEqual((self.input/'romfs/sentinel').read_bytes(), b'preserve original input')
        self.assertEqual((self.input/'exefs/main').read_bytes(), b'authored, never executed')
        self.assertEqual((self.profile/'save.sentinel').read_bytes(), b'preserve used save')
        self.assertTrue((self.current/'sentinel').is_file())

    def test_exact_obsolete_tree_and_input_junction_objects_only(self):
        old = self.prepare()
        report = self.cleanup()
        self.assertEqual(report['status'], 'complete', report)
        self.assertEqual(report['removed'], [old.name])
        self.assertFalse(old.exists())
        self.assert_preserved()

    def test_changed_unknown_and_hardlinked_nodes_defer_entire_tree(self):
        for kind in ('changed', 'unknown', 'hardlink'):
            with self.subTest(kind=kind):
                old = self.prepare({'changed': 1, 'unknown': 2, 'hardlink': 3}[kind])
                target = old/'source/Monado_Windows/runtime/authored.c'
                if kind == 'changed': target.write_bytes(b'human edit')
                elif kind == 'unknown': (old/'human-document.txt').write_bytes(b'keep')
                else: os.link(target, self.root/'human-hardlink')
                report = self.cleanup()
                self.assertEqual(report['status'], 'pending', report)
                self.assertTrue(target.is_file())
                self.assertTrue((old/'source.zip').is_file())
        self.assert_preserved()

    def test_locked_file_and_busy_session_defer_without_process_control(self):
        old = self.prepare()
        with guard.open_regular(old/'source.zip', owner=True):
            report = self.cleanup()
        self.assertEqual(report['status'], 'pending', report)
        self.assertTrue((old/'source.zip').exists())
        def busy(root): raise ValueError('authored checked runtime is still open')
        report = retention.cleanup(self.owner, current=self.current, protected=[self.profile],
                                   authorities=self.authorities,
                                   idle=busy, selection_unchanged=lambda: None)
        self.assertEqual(report['status'], 'pending')
        self.assertTrue(old.exists())
        self.assert_preserved()

    def test_interrupted_partial_removal_retries_exact_remaining_snapshot(self):
        old = self.prepare()
        real_remove, calls = retention._remove_node, []
        def interrupt(path, node):
            calls.append(path)
            if len(calls) == 3: raise OSError('authored interruption')
            return real_remove(path, node)
        with patch.object(retention, '_remove_node', side_effect=interrupt):
            first = self.cleanup()
        self.assertEqual(first['status'], 'pending')
        self.assertTrue(old.exists())
        second = self.cleanup()
        self.assertEqual(second['status'], 'complete', second)
        self.assertFalse(old.exists())
        self.assert_preserved()

    def test_current_profile_and_changed_selection_are_protected(self):
        old = self.prepare()
        report = retention.cleanup(self.owner, current=old, protected=[self.profile],
                                   authorities=self.authorities,
                                   idle=lambda root: self.fail('current root was inventoried'),
                                   selection_unchanged=lambda: None)
        self.assertEqual(report['removed'], [])
        def changed(): raise ValueError('selection changed')
        report = retention.cleanup(self.owner, current=self.current, protected=[self.profile],
                                   authorities=self.authorities,
                                   idle=lambda root: None, selection_unchanged=changed)
        self.assertEqual(report['status'], 'pending')
        self.assertTrue(old.exists())
        self.assert_preserved()

    def test_prepare_rejects_unknown_source_extra_cache_or_reparse(self):
        for kind in ('unknown', 'cache', 'junction', 'profile', 'build-note'):
            with self.subTest(kind=kind):
                old, kwargs = self.candidate({'unknown': 1, 'cache': 2, 'junction': 3, 'profile': 4, 'build-note': 5}[kind])
                if kind == 'unknown': (old/'human-note').write_text('keep')
                elif kind == 'cache': kwargs['derived_roots'] = [old/'source/other-cache']
                elif kind == 'junction': _winapi.CreateJunction(str(self.input), str(old/'source/link'))
                elif kind == 'profile': (kwargs['build_root']/'xcde.ini').write_text('human setting')
                else: (kwargs['build_root']/'build/human-note.txt').write_text('keep')
                with self.assertRaises((OSError, ValueError)):
                    retention.prepare(self.owner, old, **kwargs)
                self.assertTrue(old.exists())
        self.assert_preserved()

    def test_authority_replaced_during_cleanup_is_not_removed(self):
        old = self.prepare()
        manifest = self.owner/('updates/.retention-'+old.name+'.json')
        real_remove = retention._remove_node
        def substitute(path, node):
            real_remove(path, node)
            if path == old:
                manifest.write_bytes(b'{"human":"replacement authority"}\n')
        with patch.object(retention, '_remove_node', side_effect=substitute):
            report = self.cleanup()
        self.assertEqual(report['status'], 'pending', report)
        self.assertEqual(manifest.read_bytes(), b'{"human":"replacement authority"}\n')
        self.assert_preserved()

    def test_external_manifest_hardlink_and_scope_escape_are_rejected(self):
        old = self.prepare()
        manifest = self.owner/('updates/.retention-'+old.name+'.json')
        os.link(manifest, self.root/'manifest-hardlink')
        report = self.cleanup()
        self.assertEqual(report['status'], 'pending')
        self.assertTrue(old.exists())
        with self.assertRaisesRegex(ValueError, 'exact owned'):
            retention.cleanup(self.owner, current=self.owner, idle=lambda root: None,
                              authorities=self.authorities,
                              selection_unchanged=lambda: None)
        self.assert_preserved()

    def test_edited_authority_and_unregistered_lookalike_never_authorize_removal(self):
        old = self.prepare()
        manifest = self.owner/('updates/.retention-'+old.name+'.json')
        original = manifest.read_bytes()
        manifest.write_bytes(original + b' ')
        report = self.cleanup()
        self.assertEqual(report['status'], 'pending')
        self.assertTrue(old.exists())
        # Even a structurally valid snapshot is inert without the durable token.
        manifest.write_bytes(original)
        report = retention.cleanup(self.owner, current=self.current, authorities={},
                                   protected=[self.profile], idle=lambda root: self.fail('unregistered authority used'),
                                   selection_unchanged=lambda: None)
        self.assertEqual(report['removed'], [])
        self.assertTrue(old.exists())
        self.assert_preserved()


if __name__ == '__main__':
    unittest.main(verbosity=2)
