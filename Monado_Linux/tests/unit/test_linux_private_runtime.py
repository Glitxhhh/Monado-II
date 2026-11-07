"""[XCDE] LINUX-017: private NGX lock directory preserves desktop IPC routes."""
import importlib.util
import os
from pathlib import Path
import stat
import tempfile
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location('run_linux_private_runtime', ROOT / 'scripts/run_linux.py')
RUN = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(RUN)


class PrivateRuntimeTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.original = self.base / 'desktop'
        self.original.mkdir(mode=0o700)
        self.state = self.base / 'state'
        self.endpoints = set()
        # This unit suite tests metadata-based routing without requiring socket
        # creation in a network-restricted sandbox. Real Unix/Wayland connection
        # checks use the independent, bounded namespace component fixture.
        sockets = mock.patch.object(Path, 'is_socket', lambda path: str(path) in self.endpoints)
        sockets.start()
        self.addCleanup(sockets.stop)

    def endpoint(self, name):
        path = self.original / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b'synthetic socket metadata fixture')
        self.endpoints.add(str(path))
        return path

    def prepare(self, extra=None, **kwargs):
        environment = {'XDG_RUNTIME_DIR': str(self.original), 'DISPLAY': ':test', 'XAUTHORITY': '/original/xauth'}
        environment.update(extra or {})
        return RUN.private_runtime_environment(environment, self.state, planned_namespace=True, **kwargs)

    def test_routes_to_original_desktop_and_leaves_source_mapping_unchanged(self):
        pulse = self.endpoint('pulse/native')
        bus = self.endpoint('bus')
        environment = {'XDG_RUNTIME_DIR': str(self.original), 'WAYLAND_DISPLAY': 'wayland-test', 'DISPLAY': ':test'}
        original = dict(environment)
        result = RUN.private_runtime_environment(environment, self.state, planned_namespace=True)
        self.assertEqual(environment, original)
        self.assertEqual(result['XDG_RUNTIME_DIR'], '/tmp/runtime')
        self.assertEqual(result['WAYLAND_DISPLAY'], str(self.original / 'wayland-test'))
        self.assertEqual(result['PIPEWIRE_RUNTIME_DIR'], str(self.original))
        self.assertEqual(result['PULSE_RUNTIME_PATH'], str(self.original / 'pulse'))
        self.assertEqual(result['PULSE_SERVER'], 'unix:' + str(pulse))
        self.assertEqual(result['DBUS_SESSION_BUS_ADDRESS'], 'unix:path=' + str(bus))
        self.assertEqual(result['DISPLAY'], ':test')
        self.assertFalse(self.state.exists())

    def test_explicit_ipc_overrides_are_preserved_exactly(self):
        values = {'WAYLAND_DISPLAY': '/custom/wayland', 'PIPEWIRE_RUNTIME_DIR': '/custom/pipewire',
                  'PIPEWIRE_REMOTE': 'custom-remote', 'PULSE_RUNTIME_PATH': '/custom/pulse',
                  'PULSE_SERVER': 'unix:/custom/server', 'DBUS_SESSION_BUS_ADDRESS': 'unix:path=/custom/bus',
                  'XAUTHORITY': '/custom/xauth'}
        result = self.prepare(values)
        for key, expected in values.items():
            self.assertEqual(result[key], expected)

    def test_existing_default_wayland_socket_keeps_its_original_location(self):
        default = self.endpoint('wayland-0')
        self.assertEqual(self.prepare()['WAYLAND_DISPLAY'], str(default))

    def test_absent_default_sockets_do_not_invent_desktop_endpoints(self):
        result = self.prepare()
        for key in ('WAYLAND_DISPLAY', 'PULSE_SERVER', 'DBUS_SESSION_BUS_ADDRESS'):
            self.assertNotIn(key, result)

    def test_explicit_pulse_runtime_does_not_get_overridden_by_discovered_server(self):
        self.endpoint('pulse/native')
        result = self.prepare({'PULSE_RUNTIME_PATH': '/custom/pulse'})
        self.assertNotIn('PULSE_SERVER', result)

    def test_missing_or_empty_original_runtime_preserves_headless_defaults(self):
        for environment in ({}, {'XDG_RUNTIME_DIR': ''}):
            with self.subTest(environment=environment):
                result = RUN.private_runtime_environment(environment, self.state, planned_namespace=True)
                self.assertEqual(result, environment)
        self.assertFalse(self.state.exists())

    def test_relative_original_runtime_fails_before_writes(self):
        with self.assertRaisesRegex(ValueError, 'absolute original'):
            RUN.private_runtime_environment({'XDG_RUNTIME_DIR': 'relative/runtime'}, self.state, planned_namespace=True)
        self.assertFalse(self.state.exists())

    def test_original_runtime_must_be_an_existing_directory(self):
        file = self.base / 'file'
        file.write_text('preserve')
        for invalid in (self.base / 'missing', file):
            with self.subTest(invalid=invalid), self.assertRaisesRegex(ValueError, 'existing directory'):
                self.prepare({'XDG_RUNTIME_DIR': str(invalid)})
        self.assertEqual(file.read_text(), 'preserve')

    def test_unplanned_namespace_requires_actual_alias_attestation(self):
        with mock.patch.object(RUN, 'private_tmp_alias', return_value=False), self.assertRaisesRegex(ValueError, 'verified state'):
            RUN.private_runtime_environment({'XDG_RUNTIME_DIR': str(self.original)}, self.state)

    def test_prepared_environment_is_idempotent_in_inherited_namespace(self):
        first = self.prepare({'WAYLAND_DISPLAY': 'wayland-test'})
        with mock.patch.object(RUN, 'private_tmp_alias', return_value=True):
            second = RUN.private_runtime_environment(first, self.state)
        self.assertEqual(first, second)

    def test_private_environment_cannot_rebase_a_relative_wayland_endpoint(self):
        with mock.patch.object(RUN, 'private_tmp_alias', return_value=True), self.assertRaisesRegex(ValueError, 'cannot rebase'):
            RUN.private_runtime_environment({'XDG_RUNTIME_DIR': '/tmp/runtime', 'WAYLAND_DISPLAY': 'wayland-test'}, self.state)

    def test_private_directory_is_created_0700_without_host_runtime_changes(self):
        before = self.original.stat()
        RUN.prepare_namespace_state({'state': str(self.state), 'namespace': {'x11_socket_bind': False}})
        self.assertEqual(stat.S_IMODE((self.state / 'runtime').stat().st_mode), 0o700)
        self.assertEqual(list(self.original.iterdir()), [])
        self.assertEqual(self.original.stat().st_mtime_ns, before.st_mtime_ns)

    def test_existing_nonprivate_mode_is_preserved_and_rejected(self):
        (self.state / 'runtime').mkdir(parents=True, mode=0o755)
        (self.state / 'runtime').chmod(0o755)
        with self.assertRaisesRegex(ValueError, 'mode 0700'):
            RUN.prepare_namespace_state({'state': str(self.state), 'namespace': {}})
        self.assertEqual(stat.S_IMODE((self.state / 'runtime').stat().st_mode), 0o755)

    def test_private_runtime_symlink_is_refused(self):
        self.state.mkdir()
        (self.state / 'runtime').symlink_to(self.original, target_is_directory=True)
        with self.assertRaises(OSError):
            RUN.prepare_namespace_state({'state': str(self.state), 'namespace': {}})
        self.assertEqual(list(self.original.iterdir()), [])


if __name__ == '__main__':
    unittest.main()
