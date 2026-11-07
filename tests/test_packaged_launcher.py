"""Authored ELF-marker/plan fixtures, plus a no-GUI native namespace component.

The target wrapper imports fcntl and implements Linux process/state isolation.
Windows skips this whole module's test class instead of faking those APIs.
The native component executes only a private copy of the installed true utility.
"""
import copy
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
SCRIPTS = ROOT / 'Monado_Linux/scripts'
SUPPORTED = sys.platform == 'linux' and (SCRIPTS / 'launcher_linux.py').is_file()
launcher = None
if SUPPORTED:
    # Isolate the source-local import from any other test's cached run_linux.
    def load(name, path):
        spec = importlib.util.spec_from_file_location(name, path)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module
    native_plan = load('monado_packaged_run_linux', SCRIPTS / 'run_linux.py')
    with mock.patch.dict(sys.modules, {'run_linux': native_plan}):
        launcher = load('monado_packaged_launcher', SCRIPTS / 'launcher_linux.py')


def authored_elf(payload=b'authored fixture; never executed'):
    header = bytearray(64)
    header[:6] = b'\x7fELF\x02\x01'
    header[6] = 1
    header[16:18] = (3).to_bytes(2, 'little')
    header[18:20] = (62).to_bytes(2, 'little')
    header[20:24] = (1).to_bytes(4, 'little')
    header[52:54] = (64).to_bytes(2, 'little')
    return bytes(header) + payload


@unittest.skipUnless(SUPPORTED, 'Linux wrapper requires fcntl and Linux state/process APIs; no Windows emulation')
class PackagedLauncherTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='monado-packaged-launcher-')
        self.addCleanup(temporary.cleanup)
        self.base = Path(temporary.name).resolve()
        self.bytes = authored_elf()
        self.identity = hashlib.sha256(self.bytes).hexdigest()
        self.packaged = self.base / 'image-mount/usr/bin/monado-launcher'
        self.packaged.parent.mkdir(parents=True)
        self.write_payload(self.packaged, self.bytes)

    def write_payload(self, path, data, mode=0o755):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        path.chmod(mode)

    def verify(self, path=None, expected=None):
        return launcher.verified_packaged_launcher(
            self.packaged if path is None else path,
            self.identity if expected is None else expected)

    def test_byte_identical_native64_copy_passes_without_execution(self):
        before = (self.packaged.read_bytes(), self.packaged.stat())
        with mock.patch.object(launcher.subprocess, 'Popen', side_effect=AssertionError('ELF execution forbidden')):
            self.assertEqual(self.verify(), self.packaged)
        self.assertEqual(self.packaged.read_bytes(), before[0])
        self.assertEqual(self.packaged.stat().st_ino, before[1].st_ino)

    def test_minimal64_byte_header_boundary_passes(self):
        data = authored_elf(b'')
        self.write_payload(self.packaged, data)
        self.assertEqual(self.verify(expected=hashlib.sha256(data).hexdigest()), self.packaged)

    def test_changed_payload_is_rejected_against_original_identity(self):
        changed = self.bytes[:-1] + bytes([self.bytes[-1] ^ 1])
        self.write_payload(self.packaged, changed)
        with self.assertRaises(ValueError):
            self.verify()

    def test_truncated_native_header_is_rejected_even_with_matching_hash(self):
        data = authored_elf(b'')[:63]
        self.write_payload(self.packaged, data)
        with mock.patch.object(launcher.hashlib, 'file_digest', side_effect=AssertionError('invalid size must not be hashed')):
            with self.assertRaises(ValueError):
                self.verify(expected=hashlib.sha256(data).hexdigest())

    def test_non_native_formats_are_rejected_even_with_matching_hash(self):
        for field, value in ((0, 0), (4, 1), (5, 2), (18, 183)):
            data = bytearray(self.bytes)
            data[field] = value
            self.write_payload(self.packaged, data)
            with self.subTest(field=field), self.assertRaises(ValueError):
                self.verify(expected=hashlib.sha256(data).hexdigest())

    def test_nonexecutable_copy_is_rejected(self):
        self.packaged.chmod(0o644)
        self.assertFalse(os.access(self.packaged, os.X_OK))
        with self.assertRaises(ValueError):
            self.verify()

    def test_symlink_file_and_parent_are_rejected_without_hashing(self):
        file_alias = self.base / 'aliased-launcher'
        file_alias.symlink_to(self.packaged)
        directory_alias = self.base / 'aliased-bin'
        directory_alias.symlink_to(self.packaged.parent, target_is_directory=True)
        for path in (file_alias, directory_alias / self.packaged.name):
            with self.subTest(path=path), \
                    mock.patch.object(launcher.hashlib, 'file_digest', side_effect=AssertionError('links must not be hashed')):
                with self.assertRaises(ValueError):
                    self.verify(path)

    def test_hardlinked_copy_is_rejected_without_hashing(self):
        os.link(self.packaged, self.base / 'second-name')
        self.assertEqual(self.packaged.stat().st_nlink, 2)
        with mock.patch.object(launcher.hashlib, 'file_digest', side_effect=AssertionError('hardlink must not be hashed')):
            with self.assertRaises(ValueError):
                self.verify()

    def test_special_fifo_and_directory_are_rejected_without_opening(self):
        fifo = self.base / 'not-an-elf'
        os.mkfifo(fifo, 0o700)
        self.assertTrue(stat.S_ISFIFO(fifo.lstat().st_mode))
        for path in (fifo, self.packaged.parent):
            with self.subTest(path=path), \
                    mock.patch.object(launcher.os, 'open', side_effect=AssertionError('special entry must not be opened')):
                with self.assertRaises(ValueError):
                    self.verify(path)

    def test_oversize_sparse_file_is_rejected_without_hashing(self):
        with self.packaged.open('r+b') as stream:
            stream.truncate(64 * 1024 * 1024 + 1)
        with mock.patch.object(launcher.hashlib, 'file_digest', side_effect=AssertionError('oversize must not be hashed')):
            with self.assertRaises(ValueError):
                self.verify()

    def test_missing_copy_is_rejected(self):
        self.packaged.unlink()
        with self.assertRaises((ValueError, OSError)):
            self.verify()

    def test_path_swap_during_hash_is_rejected_even_for_identical_bytes(self):
        original_digest = launcher.hashlib.file_digest
        previous = self.base / 'original-open-inode'

        def swapped(stream, algorithm):
            result = original_digest(stream, algorithm)
            self.packaged.rename(previous)
            self.write_payload(self.packaged, self.bytes)
            return result

        with mock.patch.object(launcher.hashlib, 'file_digest', side_effect=swapped):
            with self.assertRaises(ValueError):
                self.verify()
        self.assertEqual(self.packaged.read_bytes(), previous.read_bytes())
        self.assertNotEqual(self.packaged.stat().st_ino, previous.stat().st_ino)

    def test_content_change_during_hash_is_rejected_after_old_digest_matches(self):
        original_digest = launcher.hashlib.file_digest
        original_stat = self.packaged.stat()

        def changed(stream, algorithm):
            result = original_digest(stream, algorithm)
            with self.packaged.open('r+b') as output:
                output.seek(len(self.bytes) - 1)
                output.write(bytes([self.bytes[-1] ^ 1]))
                output.flush()
                os.fsync(output.fileno())
            # Same size and restored mtime do not make a changed inode stable.
            os.utime(self.packaged, ns=(original_stat.st_atime_ns, original_stat.st_mtime_ns))
            return result

        with mock.patch.object(launcher.hashlib, 'file_digest', side_effect=changed):
            with self.assertRaises(ValueError):
                self.verify()
        self.assertNotEqual(self.packaged.read_bytes(), self.bytes)

    def plan_fixture(self, *, dlss=False):
        self.port = self.base / 'source/Monado_Linux'
        self.port.mkdir(parents=True)
        self.original_gui = self.port / 'build/gui/monado-launcher'
        self.game = self.port / 'build/game/xcde-linux'
        self.state = self.port / 'play/private-test'
        self.input = self.base / 'authored-input'
        self.input.mkdir()
        self.write_payload(self.original_gui, self.bytes)
        game_bytes = authored_elf(b'authored game-format marker; never executed')
        self.write_payload(self.game, game_bytes)
        self.gui_receipt = {'artifact_kind': 'linux-launcher'}
        self.game_receipt = {'generated': {'main': 'authored metadata'}, 'dlss_enabled': dlss}
        self.save_receipts()
        self.gui_plan = dict(argv=[str(self.original_gui), '--state-root', str(self.state), str(self.input)],
                             state=str(self.state), input=str(self.input), binary_sha256=self.identity,
                             library_path=str(self.port / '.deps/gui-library'))
        self.game_plan = dict(argv=[str(self.game), '--state-root', str(self.state), str(self.input)],
                              state=str(self.state), input=str(self.input),
                              binary_sha256=hashlib.sha256(game_bytes).hexdigest(),
                              library_path=str(self.port / '.deps/game-library'))
        root_patch = mock.patch.object(launcher, 'ROOT', self.port)
        root_patch.start()
        self.addCleanup(root_patch.stop)
        _, args = launcher.arguments(['--launcher-executable', str(self.original_gui),
                                      '--game-executable', str(self.game),
                                      '--state-root', str(self.state)])
        return args

    def save_receipts(self):
        (self.original_gui.parent / 'receipt.json').write_text(json.dumps(self.gui_receipt))
        (self.game.parent / 'receipt.json').write_text(json.dumps(self.game_receipt))

    def test_prepare_substitutes_only_gui_argv0_after_both_normal_plans(self):
        args = self.plan_fixture()
        plans_before = copy.deepcopy((self.gui_plan, self.game_plan))
        with mock.patch.object(launcher.run_linux, 'plan', side_effect=[self.gui_plan, self.game_plan]) as plans:
            original = launcher.prepare(args)
        args.packaged_launcher = self.packaged
        with mock.patch.object(launcher.run_linux, 'plan', side_effect=[self.gui_plan, self.game_plan]) as plans, \
                mock.patch.object(launcher, 'verified_packaged_launcher', wraps=launcher.verified_packaged_launcher) as verified:
            packaged = launcher.prepare(args)
        self.assertEqual(plans.call_args_list, [
            mock.call(self.original_gui, self.state), mock.call(self.game, self.state)])
        verified.assert_called_once_with(self.packaged, self.identity)
        self.assertEqual(packaged['argv'][0], str(self.packaged))
        self.assertEqual(packaged['argv'][1:], original['argv'][1:])
        self.assertEqual({k: v for k, v in packaged.items() if k != 'argv'},
                         {k: v for k, v in original.items() if k != 'argv'})
        for flag, expected in (('--project-root', str(self.port)), ('--input-root', str(self.input)),
                               ('--game-executable', str(self.game)), ('--state-root', str(self.state))):
            self.assertEqual(packaged['argv'][packaged['argv'].index(flag) + 1], expected)
        self.assertEqual((self.gui_plan, self.game_plan), plans_before)
        self.assertFalse(self.state.exists(), 'planning must not create profile/lease files')

    def test_original_plan_failures_cannot_be_bypassed_by_packaged_copy(self):
        args = self.plan_fixture()
        args.packaged_launcher = self.packaged
        for outputs, calls in (([ValueError('original GUI receipt refused')], 1),
                               ([self.gui_plan, ValueError('original game receipt refused')], 2)):
            with self.subTest(calls=calls), \
                    mock.patch.object(launcher.run_linux, 'plan', side_effect=outputs) as plans, \
                    mock.patch.object(launcher, 'verified_packaged_launcher') as packaged:
                with self.assertRaisesRegex(ValueError, 'original .* receipt refused'):
                    launcher.prepare(args)
                self.assertEqual(plans.call_count, calls)
                packaged.assert_not_called()
        self.assertFalse(self.state.exists())

    def test_wrong_gui_kind_and_incomplete_game_are_rejected_before_substitution(self):
        args = self.plan_fixture()
        args.packaged_launcher = self.packaged
        for gui, game in (({'artifact_kind': 'not-launcher'}, self.game_receipt),
                          (self.gui_receipt, {'generated': {}}),
                          (self.gui_receipt, {'artifact_kind': 'linux-launcher', 'generated': {'main': 1}})):
            self.gui_receipt, self.game_receipt = gui, game
            self.save_receipts()
            with self.subTest(gui=gui, game=game), \
                    mock.patch.object(launcher.run_linux, 'plan', side_effect=[self.gui_plan, self.game_plan]), \
                    mock.patch.object(launcher, 'verified_packaged_launcher') as packaged:
                with self.assertRaises(ValueError):
                    launcher.prepare(args)
                packaged.assert_not_called()

    def test_dlss_namespace_uses_verified_workspace_gui_and_original_state_fields(self):
        args = self.plan_fixture(dlss=True)
        args.packaged_launcher = self.packaged
        seen = []

        def namespace(argv, state, *, pid_namespace):
            seen.append((list(argv), state, pid_namespace))
            return ['authored-bwrap-fixture', *argv], {'kind': 'authored-private-dlss'}

        with mock.patch.object(launcher.run_linux, 'plan', side_effect=[self.gui_plan, self.game_plan]), \
                mock.patch.object(launcher.run_linux, 'private_tmp_alias', return_value=False), \
                mock.patch.object(launcher.run_linux, 'dlss_namespace', side_effect=namespace) as wrapped, \
                mock.patch.object(launcher, 'verified_packaged_launcher', wraps=launcher.verified_packaged_launcher) as verified, \
                mock.patch.object(launcher, 'clean_environment', return_value={'AUTHORED': 'environment'}), \
                mock.patch.object(launcher.run_linux, 'private_runtime_environment',
                                  return_value={'AUTHORED': 'isolated'}) as runtime:
            result = launcher.prepare(args)
        wrapped.assert_called_once()
        self.assertEqual(seen, [(result['argv'], self.state, False)])
        self.assertEqual(seen[0][0][0], str(self.original_gui))
        verified.assert_called_once_with(self.packaged, self.identity)
        self.assertEqual(result['execution_argv'], ['authored-bwrap-fixture', *result['argv']])
        self.assertEqual(result['state'], str(self.state))
        self.assertEqual(result['input'], str(self.input))
        self.assertEqual(result['game_sha256'], self.game_plan['binary_sha256'])
        self.assertEqual(result['launcher_sha256'], self.gui_plan['binary_sha256'])
        runtime.assert_called_once_with({'AUTHORED': 'environment'}, self.state, planned_namespace=True)
        self.assertFalse(self.state.exists())

    def test_inherited_dlss_namespace_is_retained_without_new_bwrap(self):
        args = self.plan_fixture(dlss=True)
        args.packaged_launcher = self.packaged
        with mock.patch.object(launcher.run_linux, 'plan', side_effect=[self.gui_plan, self.game_plan]), \
                mock.patch.object(launcher.run_linux, 'private_tmp_alias', return_value=True), \
                mock.patch.object(launcher.run_linux, 'dlss_namespace') as wrapped, \
                mock.patch.object(launcher.run_linux, 'private_runtime_environment',
                                  return_value={'AUTHORED': 'inherited'}) as runtime:
            result = launcher.prepare(args)
        wrapped.assert_not_called()
        self.assertEqual(result['argv'][0], str(self.original_gui))
        self.assertEqual(result['namespace'], {'kind': 'inherited-private-dlss', 'private_tmp_alias': True})
        self.assertNotIn('execution_argv', result)
        self.assertFalse(runtime.call_args.kwargs['planned_namespace'])
        self.assertFalse(self.state.exists())

    def test_dlss_still_rejects_a_changed_packaged_copy_before_namespace(self):
        args = self.plan_fixture(dlss=True)
        args.packaged_launcher = self.packaged
        self.write_payload(self.packaged, self.bytes + b'changed untrusted payload')
        for inherited in (False, True):
            with self.subTest(inherited=inherited), \
                    mock.patch.object(launcher.run_linux, 'plan', side_effect=[self.gui_plan, self.game_plan]), \
                    mock.patch.object(launcher.run_linux, 'private_tmp_alias', return_value=inherited), \
                    mock.patch.object(launcher.run_linux, 'dlss_namespace') as namespace:
                with self.assertRaisesRegex(ValueError, 'differs from the verified native GUI build'):
                    launcher.prepare(args)
                namespace.assert_not_called()
        self.assertFalse(self.state.exists())

    def test_packaged_command_retains_the_original_inherited_session_lease(self):
        args = self.plan_fixture()
        args.packaged_launcher = self.packaged
        with mock.patch.object(launcher.run_linux, 'plan', side_effect=[self.gui_plan, self.game_plan]):
            command = launcher.prepare(args)['argv']
        lease_fd, log_fd = 31, 32  # Opaque fixture descriptors; no native call receives them.
        environment = {'AUTHORED': 'private session environment'}
        library = SimpleNamespace(prctl=mock.Mock(return_value=0))
        child = SimpleNamespace(wait=mock.Mock(return_value=0))
        with mock.patch.object(Path, 'read_text', return_value=''), \
                mock.patch.object(launcher.ctypes, 'CDLL', return_value=library), \
                mock.patch.object(launcher.signal, 'signal', return_value=0), \
                mock.patch.object(launcher.os, 'waitpid', side_effect=ChildProcessError), \
                mock.patch.object(launcher.os, 'close') as closed, \
                mock.patch.object(launcher.subprocess, 'Popen', return_value=child) as spawn:
            self.assertEqual(launcher.wait_session(command, environment, lease_fd, log_fd), 0)
        spawn.assert_called_once_with(
            [*command, '--lock-fd', str(lease_fd)], cwd=self.port, env=environment,
            stdin=launcher.subprocess.DEVNULL, stdout=log_fd, stderr=log_fd,
            start_new_session=True, pass_fds=(lease_fd,))
        self.assertEqual(spawn.call_args.args[0][0], str(self.packaged))
        child.wait.assert_called_once_with()
        closed.assert_not_called()
        self.assertFalse(self.state.exists())


@unittest.skipUnless(SUPPORTED, 'native Linux bwrap/user-namespace component; no Windows emulation')
class DlssPackagedNamespaceTests(unittest.TestCase):
    def test_workspace_executable_survives_private_tmp_but_packaged_tmp_copy_does_not(self):
        if not native_plan.BWRAP.is_file() or not os.access(native_plan.BWRAP, os.X_OK):
            self.skipTest('installed bwrap is unavailable')
        # Keep the synthetic source/build outside /tmp so the intentional state
        # bind does not hide the original executable along with the image mount.
        parent = (ROOT / 'Monado_Linux').resolve()
        if parent.is_relative_to(Path('/tmp').resolve()):
            self.skipTest('native namespace fixture requires its workspace outside /tmp')
        with tempfile.TemporaryDirectory(prefix='.dlss-namespace-fixture-', dir=parent) as directory, \
                tempfile.TemporaryDirectory(prefix='monado-packaged-tmp-') as temporary:
            workspace = Path(directory).resolve() / 'Monado_Linux'
            original = workspace / 'build/fixture/bin/true'
            state = workspace / 'play/isolated'
            packaged = Path(temporary).resolve() / 'usr/bin/monado-launcher'
            original.parent.mkdir(parents=True)
            packaged.parent.mkdir(parents=True)
            utility = Path('/usr/bin/true').resolve(strict=True)
            shutil.copyfile(utility, original)
            shutil.copyfile(utility, packaged)
            original.chmod(0o755)
            packaged.chmod(0o755)
            digest = hashlib.sha256(original.read_bytes()).hexdigest()
            self.assertEqual(launcher.verified_packaged_launcher(packaged, digest), packaged)
            self.assertNotEqual(original.stat().st_ino, packaged.stat().st_ino)
            environment = {'PATH': '/usr/bin:/bin', 'LANG': 'C.UTF-8', 'LC_ALL': 'C.UTF-8'}

            with mock.patch.object(native_plan, 'ROOT', workspace):
                command, namespace = native_plan.dlss_namespace([str(original)], state, pid_namespace=False)
                native_plan.prepare_namespace_state({'state': str(state), 'namespace': namespace})
                result = subprocess.run(command, capture_output=True, text=True,
                                        timeout=15, env=environment)
                if result.returncode:
                    denied = ('operation not permitted', 'permission denied', 'no permissions',
                              'user namespace', 'not supported', 'cannot create namespace')
                    if any(message in result.stderr.lower() for message in denied):
                        self.skipTest('bwrap/user namespaces unavailable in this execution sandbox: ' + result.stderr.strip())
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn('--unshare-net', command)
                self.assertEqual(command[command.index('--') + 1:], [str(original)])
                self.assertIn('--bind', command)
                # Negative control: the byte-identical /tmp payload really is
                # hidden by the same unchanged state namespace arrangement.
                hidden_command, _ = native_plan.dlss_namespace([str(packaged)], state, pid_namespace=False)
                hidden = subprocess.run(hidden_command, capture_output=True, text=True,
                                        timeout=15, env=environment)
                self.assertNotEqual(hidden.returncode, 0)
                self.assertTrue('no such file' in hidden.stderr.lower() or
                                'not found' in hidden.stderr.lower(), hidden.stderr)
                self.assertEqual(hashlib.sha256(original.read_bytes()).hexdigest(), digest)
                self.assertEqual(hashlib.sha256(packaged.read_bytes()).hexdigest(), digest)
                self.assertEqual(sorted(p.name for p in state.iterdir()),
                                 ['.X11-unix', 'runtime'] if namespace['x11_socket_bind'] else ['runtime'])


if __name__ == '__main__':
    unittest.main()
