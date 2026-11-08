"""Authored managed-install boundaries; no input generation, build or game run."""
import importlib.util
import json
import os
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('monado_install_layout_test', ROOT / 'Monado_Linux/scripts/install_layout.py')
layout = importlib.util.module_from_spec(spec)
spec.loader.exec_module(layout)


class InstallationLayout(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='monado-install-layout-')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve() / 'original install'
        self.port = self.root / 'Monado_Linux'
        self.port.mkdir(parents=True)
        self.workspace = self.port / 'updates/release-1/workspace'
        self.candidate = self.workspace / 'Monado_Linux'
        self.candidate.mkdir(parents=True)
        self.marker = self.workspace / 'installation.json'
        self.value = {'schema': 1, 'owner': str(self.root), 'workspace': str(self.workspace),
                      'game_port': str(self.port)}

    def write(self, value=None):
        self.marker.write_text(json.dumps(self.value if value is None else value))

    def test_unmanaged_layout_has_its_own_profile_and_game(self):
        self.assertEqual(layout.installation_root(self.port), self.root)
        self.assertEqual(layout.game_port(self.port), self.port)
        self.assertEqual(layout.profile_port(self.port), self.port)
        self.assertFalse(self.marker.exists())

    def test_managed_layout_retains_original_profile_and_direct_game(self):
        self.write()
        self.assertEqual(layout.installation_root(self.candidate), self.root)
        self.assertEqual(layout.game_port(self.candidate), self.port)
        self.assertEqual(layout.state_roots(self.candidate),
                         (self.port / 'play', self.port / 'runs', self.candidate / 'runs'))

    def test_full_build_can_bind_its_own_managed_game_port(self):
        self.value['game_port'] = str(self.candidate)
        self.write()
        self.assertEqual(layout.game_port(self.candidate), self.candidate)

    def test_another_managed_game_requires_same_owner_marker(self):
        other = self.port / 'updates/release-0/workspace/Monado_Linux'
        other.mkdir(parents=True)
        self.value['game_port'] = str(other)
        self.write()
        with self.assertRaisesRegex(ValueError, 'matching owner'):
            layout.game_port(self.candidate)
        other_value = dict(self.value, workspace=str(other.parent), game_port=str(other))
        (other.parent / 'installation.json').write_text(json.dumps(other_value))
        self.assertEqual(layout.game_port(self.candidate), other)

    def test_external_game_port_and_forged_workspace_location_are_rejected(self):
        outside = self.root.parent / 'outside/Monado_Linux'
        outside.mkdir(parents=True)
        for field, value in (('game_port', outside), ('workspace', self.root), ('owner', self.workspace)):
            bad = dict(self.value, **{field: str(value)})
            self.write(bad)
            with self.subTest(field=field), self.assertRaises(ValueError):
                layout.binding(self.candidate)

    def test_unknown_duplicate_fields_and_boolean_schema_are_rejected(self):
        for value in (dict(self.value, schema=True), dict(self.value, future='unreviewed')):
            self.write(value)
            with self.assertRaises(ValueError):
                layout.binding(self.candidate)
        self.marker.write_text(json.dumps(self.value)[:-1] + ', "schema": 1}')
        with self.assertRaisesRegex(ValueError, 'duplicate'):
            layout.binding(self.candidate)

    def test_malformed_and_oversized_markers_are_rejected(self):
        for value in ('{bad', ' ' * (layout.MAX_MARKER + 1), '[]'):
            self.marker.write_text(value)
            with self.subTest(length=len(value)), self.assertRaises(ValueError):
                layout.binding(self.candidate)

    def test_marker_symlink_and_hardlink_are_rejected(self):
        target = self.root / 'authored-marker.json'
        target.write_text(json.dumps(self.value))
        try:
            self.marker.symlink_to(target)
        except OSError as error:
            self.skipTest(str(error))
        with self.assertRaisesRegex(ValueError, 'single-link'):
            layout.binding(self.candidate)
        self.marker.unlink()
        try:
            os.link(target, self.marker)
        except OSError as error:
            self.skipTest(str(error))
        with self.assertRaisesRegex(ValueError, 'single-link'):
            layout.binding(self.candidate)

    def test_update_workspace_symlink_is_rejected(self):
        alias = self.port / 'updates/alias'
        try:
            alias.symlink_to(self.workspace.parent, target_is_directory=True)
        except OSError as error:
            self.skipTest(str(error))
        with self.assertRaisesRegex(ValueError, 'symlinks'):
            layout.binding(alias / 'workspace/Monado_Linux')

    def test_cyclic_game_bindings_are_rejected(self):
        other = self.port / 'updates/release-0/workspace/Monado_Linux'
        other.mkdir(parents=True)
        self.value['game_port'] = str(other)
        self.write()
        (other.parent / 'installation.json').write_text(json.dumps(dict(self.value,
            workspace=str(other.parent), game_port=str(self.candidate))))
        with self.assertRaisesRegex(ValueError, 'cyclic'):
            layout.binding(self.candidate)

    def test_only_current_and_direct_bound_game_builds_are_selected(self):
        self.write()
        for port in (self.port, self.candidate):
            binary = port / 'build/authored/never-executed'
            binary.parent.mkdir(parents=True)
            binary.write_text('authored fixture')
            self.assertEqual(layout.build_port(self.candidate, binary), port)
        outside = self.port / 'updates/unbound/workspace/Monado_Linux/build/game'
        outside.parent.mkdir(parents=True)
        outside.write_text('authored unbound fixture')
        with self.assertRaisesRegex(ValueError, 'current or bound'):
            layout.build_port(self.candidate, outside)

    def test_input_link_must_resolve_to_original_input(self):
        self.write()
        (self.root / 'game_files').mkdir()
        try:
            (self.workspace / 'game_files').symlink_to(self.root / 'game_files', target_is_directory=True)
        except OSError as error:
            self.skipTest(str(error))
        self.assertEqual(layout.shared_input(self.candidate), self.root / 'game_files')
        (self.workspace / 'game_files').unlink()
        (self.workspace / 'game_files').mkdir()
        with self.assertRaisesRegex(ValueError, 'match'):
            layout.shared_input(self.candidate)


if __name__ == '__main__':
    unittest.main()
