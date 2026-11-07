"""[XCDE] LINUX-017: optional namespace launch plans preserve ELF identity."""
import hashlib
import importlib.util
import json
from pathlib import Path
import stat
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location('run_linux_dlss_entry', ROOT / 'scripts/run_linux.py')
RUN = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(RUN)


class DlssEntryTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.workspace = Path(self.temporary.name)
        self.root = self.workspace / 'Monado_Linux'
        self.root.mkdir()
        (self.workspace / 'game_files').mkdir()
        self.binary = self.root / 'build/candidate/xcde-linux'
        self.binary.parent.mkdir(parents=True)
        header = bytearray(64)
        header[:6] = b'\x7fELF\x02\x01'
        header[18:20] = (62).to_bytes(2, 'little')
        self.binary.write_bytes(header)
        self.binary.chmod(0o700)
        sysroot = self.root / '.deps/graphics/sysroots/fixture'
        (sysroot / 'usr/lib/x86_64-linux-gnu').mkdir(parents=True)
        (sysroot / '.monado-deps.json').write_text(json.dumps({'lock_sha256': 'locked'}))
        self.document = {'status': 'passed', 'executable': {'path': str(self.binary),
                         'sha256': hashlib.sha256(header).hexdigest()},
                         'dependencies': {'sysroot': str(sysroot)},
                         'sources_before': {'graphics-deps-linux.lock.json': 'locked'}}
        self.receipt = self.binary.parent / 'receipt.json'
        self.state = self.root / 'runs/session/state'
        self.bwrap = self.workspace / 'bwrap'
        self.bwrap.write_bytes(b'synthetic helper identity, never executed')
        self.bwrap.chmod(0o700)
        self.updater = self.workspace / 'nvidia-ngx-updater'
        self.x11 = self.workspace / 'host-x11'
        for name, value in [('ROOT', self.root), ('BWRAP', self.bwrap),
                            ('NGX_UPDATER', self.updater), ('X11_SOCKETS', self.x11)]:
            patch = mock.patch.object(RUN, name, value)
            patch.start()
            self.addCleanup(patch.stop)

    def enable_dlss(self):
        self.document['dlss_enabled'] = True
        resources = self.document['runtime_resources'] = {}
        for name in ('libmonado-ngx.so', 'libnvidia-ngx-dlss.so.310.9.1', 'NVIDIA-DLSS-LICENSE.txt'):
            path = self.binary.parent / name
            path.write_bytes(('synthetic companion ' + name).encode())
            resources[name] = {'path': str(path), 'bytes': path.stat().st_size,
                               'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}

    def plan(self):
        self.receipt.write_text(json.dumps(self.document))
        return RUN.plan(self.binary, self.state)

    def test_legacy_plan_keeps_direct_execution_and_needs_no_bwrap(self):
        self.bwrap.unlink()
        launch = self.plan()
        self.assertEqual(launch['argv'][0], str(self.binary))
        self.assertNotIn('execution_argv', launch)

    def test_framegen_flag_is_boolean_and_requires_dlss_resources(self):
        for invalid in (1, 'yes', None, []):
            with self.subTest(value=invalid):
                self.document['framegen_enabled'] = invalid
                with self.assertRaisesRegex(ValueError, 'invalid frame-generation'):
                    self.plan()
        self.document['framegen_enabled'] = True
        with self.assertRaisesRegex(ValueError, 'frame-generation build receipt'):
            self.plan()
        self.enable_dlss()
        for name in ('libmonado-fidelityfx.so', 'FidelityFX-MIT.txt'):
            with self.assertRaisesRegex(ValueError, 'frame-generation build receipt'):
                self.plan()
            path = self.binary.parent / name
            path.write_bytes(('authored native companion ' + name).encode())
            self.document['runtime_resources'][name] = {'path': str(path), 'bytes': path.stat().st_size,
                'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}
        launch = self.plan()
        self.assertIn('execution_argv', launch)
        self.assertFalse(self.state.exists())
        module = self.binary.parent / 'libmonado-fidelityfx.so'
        module.write_bytes(b'altered provider')
        with self.assertRaisesRegex(ValueError, 'runtime resource'):
            self.plan()
        self.assertFalse(self.state.exists())

    def test_dlss_plan_keeps_actual_argv_and_is_mutation_free(self):
        self.enable_dlss()
        launch = self.plan()
        self.assertEqual(launch['argv'][0], str(self.binary))
        command = launch['execution_argv']
        self.assertEqual(command[0], str(self.bwrap))
        self.assertEqual(command[-len(launch['argv']):], launch['argv'])
        for flag in ('--unshare-user', '--unshare-pid', '--unshare-net'):
            self.assertIn(flag, command)
        self.assertNotIn('--die-with-parent', command)
        self.assertIn(['--bind', str(self.state), '/tmp'], [command[i:i + 3] for i in range(len(command))])
        self.assertFalse(self.state.exists())

    def test_existing_private_tmp_alias_does_not_attempt_nested_bwrap(self):
        self.enable_dlss()
        self.bwrap.unlink()
        with mock.patch.object(RUN, 'private_tmp_alias', return_value=True):
            launch = self.plan()
        self.assertNotIn('execution_argv', launch)
        self.assertEqual(launch['namespace']['kind'], 'inherited-private-dlss')

    def test_existing_alias_rejects_unmasked_updater_and_accepts_null_mask(self):
        same_inode = SimpleNamespace(st_dev=1, st_ino=2, st_mode=stat.S_IFCHR, st_rdev=259)
        updater = mock.Mock()
        updater.lstat.return_value = SimpleNamespace(st_mode=stat.S_IFREG, st_rdev=0)
        with mock.patch.object(Path, 'stat', return_value=same_inode), \
             mock.patch.object(RUN, 'NGX_UPDATER', updater):
            with self.assertRaisesRegex(ValueError, 'unmasked NVIDIA updater'):
                RUN.private_tmp_alias(self.state)
            updater.lstat.return_value = SimpleNamespace(st_mode=stat.S_IFCHR, st_rdev=259)
            self.assertTrue(RUN.private_tmp_alias(self.state))
            updater.lstat.side_effect = FileNotFoundError
            self.assertTrue(RUN.private_tmp_alias(self.state))

    def test_outer_gui_namespace_has_no_pid_or_parent_death_coupling(self):
        command, info = RUN.dlss_namespace([str(self.binary)], self.state, pid_namespace=False)
        self.assertNotIn('--unshare-pid', command)
        self.assertNotIn('--sync-fd', command)
        self.assertNotIn('--die-with-parent', command)
        self.assertFalse(info['pid_namespace'])
        self.assertIn(['--ro-bind', '/proc', '/proc'], [command[i:i + 3] for i in range(len(command))])

    def test_x11_and_updater_are_bound_only_when_present(self):
        self.enable_dlss()
        self.x11.mkdir()
        self.updater.write_bytes(b'synthetic installed updater')
        launch = self.plan()
        triples = [launch['execution_argv'][i:i + 3] for i in range(len(launch['execution_argv']))]
        self.assertIn(['--ro-bind', str(self.x11), '/tmp/.X11-unix'], triples)
        self.assertIn(['--ro-bind', '/dev/null', str(self.updater)], triples)
        self.assertTrue(launch['namespace']['x11_socket_bind'])
        self.assertTrue(launch['namespace']['updater_masked'])
        self.assertFalse(self.state.exists())

    def test_missing_helper_or_runtime_companions_fail_closed(self):
        self.document['dlss_enabled'] = True
        with self.assertRaisesRegex(ValueError, 'runtime companions'):
            self.plan()
        self.enable_dlss()
        self.bwrap.unlink()
        with self.assertRaisesRegex(ValueError, 'namespace helper'):
            self.plan()

    def test_nonboolean_receipt_flag_rejected(self):
        self.document['dlss_enabled'] = 'true'
        with self.assertRaisesRegex(ValueError, 'receipt flag'):
            self.plan()

    def test_unsafe_bind_sources_rejected(self):
        self.enable_dlss()
        self.x11.symlink_to(self.workspace, target_is_directory=True)
        with self.assertRaisesRegex(ValueError, 'ordinary directory'):
            self.plan()
        self.x11.unlink()
        self.updater.mkdir()
        with self.assertRaisesRegex(ValueError, 'installed file'):
            self.plan()

    def test_actual_preparation_creates_only_private_directories(self):
        self.enable_dlss()
        self.x11.mkdir()
        launch = self.plan()
        RUN.prepare_namespace_state(launch)
        self.assertTrue(self.state.is_dir())
        self.assertTrue((self.state / '.X11-unix').is_dir())
        self.assertEqual(list(self.x11.iterdir()), [])

    def test_private_x11_symlink_cannot_escape_state(self):
        self.enable_dlss()
        self.x11.mkdir()
        launch = self.plan()
        self.state.mkdir(parents=True)
        (self.state / '.X11-unix').symlink_to(self.x11, target_is_directory=True)
        with self.assertRaises(OSError):
            RUN.prepare_namespace_state(launch)
        self.assertEqual(list(self.x11.iterdir()), [])

    def test_state_parent_replaced_with_symlink_is_refused_before_write(self):
        self.enable_dlss()
        launch = self.plan()
        (self.root / 'runs').symlink_to(self.workspace, target_is_directory=True)
        with self.assertRaises(OSError):
            RUN.prepare_namespace_state(launch)
        self.assertFalse((self.workspace / 'session').exists())


if __name__ == '__main__':
    unittest.main()
