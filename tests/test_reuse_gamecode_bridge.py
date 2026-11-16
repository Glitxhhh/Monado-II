"""Authored managed-update planning fixtures; no old library/build/game runs."""
import contextlib
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('_monado_gamecode_bridge', ROOT / 'tools/build_linux_app.py')
build = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build)


@unittest.skipUnless(sys.platform == 'linux', 'Linux managed payload ownership')
class GamecodeBridge(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='monado gamecode bridge ')
        self.addCleanup(temporary.cleanup)
        self.base = Path(temporary.name).resolve()
        self.owner = self.base / 'owner'
        (self.owner / 'Monado_Linux').mkdir(parents=True)
        self.workspace, self.port = self.payload('payload-candidate')
        self.previous, self.old_port = self.payload('payload-active')
        self.game = self.old_port / 'build/accepted/xcde-linux'
        self.game.parent.mkdir(parents=True)
        self.game.write_bytes(b'authored never-executed native placeholder')
        self.library = self.game.parent / 'lasgame.so'
        self.library.write_bytes(b'authored stale library that must not be consumed')
        self.features = {'status': 'passed', 'split_game': True,
                         'executable': {'path': str(self.game), 'sha256': self.sha(self.game)}}

    @staticmethod
    def sha(path):
        return hashlib.sha256(path.read_bytes()).hexdigest()

    def payload(self, identity):
        workspace = self.owner / 'Monado_Linux/updates' / identity / 'workspace'
        port = workspace / 'Monado_Linux'
        port.mkdir(parents=True)
        (workspace / 'installation.json').write_text(json.dumps(dict(
            schema=1, owner=str(self.owner), workspace=str(workspace), game_port=str(port))))
        return workspace, port

    def checker(self):
        return mock.patch.object(build, 'checked_managed_game_features', return_value=self.features)

    def normalize(self, game=None, *, game_only=True, split_game=True):
        return build.normalize_gamecode_reuse(self.workspace, game or self.game,
            game_only=game_only, split_game=split_game)

    def plan(self, game=None, **extra):
        return build.layout(self.workspace, 2, require_input=False, identity='authored',
            reuse_game_code=game or self.game, game_only=True, split_game=True,
            dlss=True, framegen=True, fsr3=True, fsr4=True, portable=True, **extra)

    def test_exact_active_cross_payload_regenerates_without_reading_old_library(self):
        before = {str(p): p.read_bytes() for p in self.base.rglob('*') if p.is_file()}
        original = build.digest
        def checked_digest(path):
            self.assertNotEqual(Path(path), self.library)
            return original(path)
        with self.checker() as checked, mock.patch.object(build, 'digest', side_effect=checked_digest), \
                contextlib.redirect_stderr(io.StringIO()) as notice:
            paths, commands = self.plan()
        checked.assert_called_once_with(self.workspace)
        stages = dict(commands)
        self.assertIn('generate-game', stages)
        self.assertIn('--generated', stages['build-game'])
        self.assertNotIn('--reuse-game-code', stages['build-game'])
        self.assertEqual(stages['build-game'][stages['build-game'].index('--generated') + 1], str(paths['generated']))
        for flag in ('--dlss', '--framegen', '--fsr3', '--fsr4', '--split-game'):
            self.assertIn(flag, stages['build-game'])
        self.assertEqual(paths['state'], self.owner / 'Monado_Linux/play/launcher')
        self.assertEqual(paths['input'], self.workspace / 'game_files')
        self.assertIn('Regenerating and compiling game code locally', notice.getvalue())
        self.assertEqual(before, {str(p): p.read_bytes() for p in self.base.rglob('*') if p.is_file()})

    def test_direct_bound_reuse_keeps_native_checks_and_omits_generation(self):
        marker = self.workspace / 'installation.json'
        value = json.loads(marker.read_text()); value['game_port'] = str(self.old_port)
        marker.write_text(json.dumps(value))
        with mock.patch.object(build, 'checked_managed_game_features', side_effect=AssertionError('not a fallback')):
            paths, commands = self.plan()
        stages = dict(commands)
        self.assertNotIn('generate-game', stages)
        self.assertIn('--reuse-game-code', stages['build-game'])
        self.assertNotIn('--generated', stages['build-game'])

    def test_cross_payload_fallback_requires_game_only_and_split_mode(self):
        for game_only, split_game in ((False, True), (True, False), (False, False)):
            with self.subTest(game_only=game_only, split_game=split_game), self.checker() as checked, \
                    self.assertRaises(build.BuildError):
                self.normalize(game_only=game_only, split_game=split_game)
            checked.assert_not_called()

    def test_unmanaged_or_non_self_bound_foreign_requests_fail_closed(self):
        with self.checker() as checked, self.assertRaises(build.BuildError):
            build.normalize_gamecode_reuse(self.owner, self.game, game_only=True, split_game=True)
        checked.assert_not_called()
        value = json.loads((self.workspace / 'installation.json').read_text())
        value['game_port'] = str(self.owner / 'Monado_Linux')
        (self.workspace / 'installation.json').write_text(json.dumps(value))
        with self.checker() as checked, self.assertRaises(build.BuildError):
            self.normalize()
        checked.assert_not_called()

    def test_same_owner_sibling_or_foreign_executable_cannot_substitute(self):
        sibling, _ = self.payload('payload-unselected')
        foreign = self.base / 'foreign/xcde-linux'
        other = sibling / 'Monado_Linux/build/accepted/xcde-linux'
        for path in (foreign, other):
            path.parent.mkdir(parents=True); path.write_bytes(self.game.read_bytes())
            with self.subTest(path=path), self.checker(), \
                    self.assertRaisesRegex(build.BuildError, 'exact verified active game'):
                self.normalize(path)

    def test_changed_native_hash_or_active_selection_rejects(self):
        self.features['executable']['sha256'] = '0' * 64
        with self.checker(), self.assertRaisesRegex(build.BuildError, 'exact verified active game'):
            self.normalize()
        with mock.patch.object(build, 'checked_managed_game_features', side_effect=ValueError('active receipt changed')), \
                self.assertRaisesRegex(ValueError, 'active receipt changed'):
            self.normalize()

    def test_alias_and_multiply_linked_native_inputs_are_rejected(self):
        alias = self.game.parent / 'alias'
        alias.symlink_to(self.game)
        with self.checker() as checked, self.assertRaises(build.BuildError):
            self.normalize(alias)
        checked.assert_not_called()

        alias.unlink(); os.link(self.game, alias)
        with self.checker() as checked, self.assertRaises(build.BuildError):
            self.normalize(alias)
        checked.assert_not_called()

    def test_requested_parent_traversal_is_rejected_before_selection(self):
        requested = self.game.parent / '..' / 'accepted' / self.game.name
        with self.checker() as checked, self.assertRaises(build.BuildError):
            self.normalize(requested)
        checked.assert_not_called()

    def test_managed_binding_alias_hardlink_or_traversal_is_rejected(self):
        marker = self.workspace / 'installation.json'
        original = marker.read_bytes()
        other = self.workspace / 'authored-marker.json'
        other.write_bytes(original)
        marker.unlink(); marker.symlink_to(other)
        with self.checker() as checked, self.assertRaises(ValueError):
            self.normalize()
        checked.assert_not_called()
        marker.unlink(); os.link(other, marker)
        with self.checker() as checked, self.assertRaises(ValueError):
            self.normalize()
        checked.assert_not_called()
        marker.unlink()
        value = json.loads(original); value['game_port'] = str(self.port / '..' / 'Monado_Linux')
        marker.write_text(json.dumps(value))
        with self.checker() as checked, self.assertRaises(ValueError):
            self.normalize()
        checked.assert_not_called()

    def test_plan_stdout_remains_machine_readable_and_notice_is_stderr_only(self):
        with mock.patch.object(build, 'ROOT', self.workspace), self.checker(), \
                contextlib.redirect_stdout(io.StringIO()) as output, \
                contextlib.redirect_stderr(io.StringIO()) as notice:
            self.assertEqual(build.main(['--plan', '--game-only', '--reuse-game-code', str(self.game)]), 0)
        value = json.loads(output.getvalue())
        stages = {row['name']: row['argv'] for row in value['commands']}
        self.assertIn('generate-game', stages)
        self.assertNotIn('--reuse-game-code', stages['build-game'])
        self.assertIn('Regenerating and compiling', notice.getvalue())


if __name__ == '__main__':
    unittest.main()
