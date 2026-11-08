"""Authored AppImage boundary fixtures; never use game input or execute a game."""
from pathlib import Path
import hashlib
import importlib.util
import json
import os
import shutil
import stat
import subprocess
import sys
import tempfile
import types
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


image = load('monado_appimage_test_module', ROOT / 'tools/build_launcher_appimage.py')
driver = load('monado_appimage_driver_test_module', ROOT / 'tools/appimage/driver.py')


def elf(*, runtime=False):
    data = bytearray(128)
    data[:6] = b'\x7fELF\x02\x01'
    data[18:20] = (62).to_bytes(2, 'little')
    if runtime:
        data[8:11] = b'AI\x02'
    return bytes(data)


class FixtureBase(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='monado-appimage-fixture-')
        self.addCleanup(temporary.cleanup)
        self.base = Path(temporary.name).resolve()
        self.root = self.base / 'workspace with spaces'
        self.port = self.root / 'Monado_Linux'
        self.launcher = self.port / 'build/gui/monado-launcher'
        self.game = self.port / 'build/game/xcde-linux'
        for path, content in ((self.launcher, elf()), (self.game, b'PRIVATE authored fixture, never copied')):
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(content)
            path.chmod(0o755)
        for name in ('LICENSE', 'CREDITS.md', 'NOTICE.md',
                'Monado_Linux/third_party/imgui/LICENSE.txt',
                'Monado_Linux/tools/launcher/assets/fonts/LICENSE-DejaVu.txt'):
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('Authored license fixture\n')
        self.templates = self.root / 'tools/appimage'
        shutil.copytree(ROOT / 'tools/appimage', self.templates)
        scripts = self.port / 'scripts'
        scripts.mkdir()
        shutil.copyfile(ROOT / 'Monado_Linux/scripts/install_layout.py', scripts / 'install_layout.py')
        (self.root / 'tools/update_linux.py').write_text('def dispatch_active(workspace, argv):\n    return None\n')
        patch = mock.patch.object(image, 'TEMPLATES', self.templates)
        patch.start()
        self.addCleanup(patch.stop)
        old_path = list(sys.path)
        self.addCleanup(lambda: sys.path.__setitem__(slice(None), old_path))

    def selection(self, launcher, game, root):
        self.assertEqual((launcher, game, root), (self.launcher, self.game, self.root))
        return {'launcher_sha256': hashlib.sha256(self.launcher.read_bytes()).hexdigest(),
                'game_sha256': hashlib.sha256(b'private fixture identity').hexdigest()}

    def package(self, name='result'):
        return image.package(self.launcher, self.game,
            self.port / 'build' / name / 'Monado-II-x86_64.AppImage', root=self.root,
            selection_provider=self.selection, runtime_provider=lambda port: elf(runtime=True))

    def appdir(self):
        directory = self.base / 'AppDir'
        files = image.source_files(self.root)
        files['usr/bin/monado-launcher'] = (elf(), 0o755)
        files['usr/share/monado-ii/selection.json'] = (image.encoded({
            'schema': 1, 'project': 'Monado II',
            'launcher': 'Monado_Linux/build/gui/monado-launcher',
            'game': 'Monado_Linux/build/game/xcde-linux'}), 0o644)
        files[image.MANIFEST] = (image.encoded({'schema': 1, 'project': 'Monado II',
                                               'files': image.identity_map(files)}), 0o644)
        for name, (data, mode) in files.items():
            path = directory / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
            path.chmod(mode)
        return directory, files


class AppImageFixtures(FixtureBase):
    def test_runtime_lock_is_exact_reviewed_identity(self):
        self.assertEqual(image.runtime_lock()['sha256'], image.RUNTIME_SHA)
        lock = json.loads((ROOT / 'appimage-runtime.lock.json').read_bytes())
        lock['runtime']['commit'] = '0' * 40
        with mock.patch.object(image, 'regular', return_value=image.encoded(lock)):
            with self.assertRaises(image.AppImageError):
                image.runtime_lock()

    def test_download_accepts_only_official_https_hosts(self):
        image.safe_url(image.RUNTIME_URL)
        image.safe_url('https://release-assets.githubusercontent.com/a?token=download')
        for value in ('http://github.com/a', 'https://example.org/runtime',
                      'https://github.com.evil.invalid/a', 'https://user@github.com/a',
                      'https://github.com:444/a', 'https://github.com/a#fragment'):
            with self.subTest(value=value), self.assertRaises(image.AppImageError):
                image.safe_url(value)

    def test_download_size_and_hash_are_bounded_before_use(self):
        good = elf(runtime=True)
        identity = {'url': image.RUNTIME_URL, 'bytes': len(good), 'sha256': image.digest(good)}
        for payload, valid in ((good, True), (good + b'x', False), (good[:-1], False),
                               (good[:-1] + b'x', False)):
            response = mock.MagicMock()
            response.__enter__.return_value = response
            response.geturl.return_value = image.RUNTIME_URL
            response.read.side_effect = lambda maximum, value=payload: value[:maximum]
            client = mock.Mock()
            client.open.return_value = response
            if valid:
                self.assertEqual(image.fetch_runtime(identity, client), good)
            else:
                with self.assertRaises(image.AppImageError):
                    image.fetch_runtime(identity, client)
            response.read.assert_called_once_with(len(good) + 1)

    def test_redirect_outside_official_hosts_is_rejected(self):
        with self.assertRaises(image.AppImageError):
            image.OfficialRedirect().redirect_request(None, None, 302, 'Found', {}, 'https://example.org/data')

    def test_cached_runtime_is_checked_offline_and_bad_cache_preserved(self):
        good = elf(runtime=True)
        identity = {'url': image.RUNTIME_URL, 'bytes': len(good), 'sha256': image.digest(good)}
        cache = self.port / '.deps/appimage'
        cache.mkdir(parents=True)
        target = cache / ('runtime-x86_64-' + identity['sha256'])
        target.write_bytes(good)
        with mock.patch.object(image, 'runtime_lock', return_value=identity), mock.patch.object(image, 'fetch_runtime') as fetch:
            self.assertEqual(image.runtime_bytes(self.port), good)
            fetch.assert_not_called()
            target.write_bytes(b'bad cached runtime')
            with self.assertRaises(image.AppImageError):
                image.runtime_bytes(self.port)
            self.assertEqual(target.read_bytes(), b'bad cached runtime')
            fetch.assert_not_called()

    def test_native_architecture_and_appimage_marker_required(self):
        image.native(elf())
        image.native(elf(runtime=True), runtime=True)
        for value, runtime in ((b'MZ authored fixture', False), (elf(), True),
                               (elf()[:18] + b'\xb7\x00' + elf()[20:], False)):
            with self.assertRaises(image.AppImageError):
                image.native(value, runtime=runtime)

    def test_game_input_and_outside_build_selections_refused(self):
        self.assertEqual(image.relative_build(self.launcher, self.root), 'Monado_Linux/build/gui/monado-launcher')
        for path in (self.root / 'game_files/exefs/main', self.base / 'outside/launcher',
                     self.port / 'build/gui/../monado-launcher'):
            with self.assertRaises(image.AppImageError):
                image.relative_build(path, self.root)

    def test_existing_output_or_receipt_is_not_overwritten(self):
        output = self.port / 'build/result/Monado-II-x86_64.AppImage'
        output.parent.mkdir()
        output.write_bytes(b'preserved image')
        with self.assertRaises(image.AppImageError):
            self.package()
        self.assertEqual(output.read_bytes(), b'preserved image')
        output.unlink()
        receipt = output.with_name(output.name + '.receipt.json')
        receipt.write_text('preserved receipt')
        with self.assertRaises(image.AppImageError):
            self.package()
        self.assertEqual(receipt.read_text(), 'preserved receipt')

    def test_guarded_selection_failure_stops_before_output_or_runtime(self):
        provider = mock.Mock(side_effect=ValueError('bad original receipt'))
        runtime = mock.Mock()
        output = self.port / 'build/result/Monado-II-x86_64.AppImage'
        with mock.patch.object(image.shutil, 'which', return_value='/authored/tool'), \
                mock.patch.object(image.sys, 'platform', 'linux'), \
                mock.patch.object(image.platform, 'machine', return_value='x86_64'), \
                self.assertRaisesRegex(ValueError, 'bad original receipt'):
            image.package(self.launcher, self.game, output, root=self.root,
                          selection_provider=provider, runtime_provider=runtime)
        runtime.assert_not_called()
        self.assertFalse(output.parent.exists())

    @unittest.skipUnless(os.name == 'posix', 'POSIX file modes required for AppDir staging checks')
    def test_exact_appdir_allowlist_rejects_extra_files_and_empty_directories(self):
        directory, files = self.appdir()
        image.check_tree(directory, files)
        extra = directory / 'game-files-placeholder'
        extra.write_text('not allowed even if authored')
        with self.assertRaises(image.AppImageError):
            image.check_tree(directory, files)
        extra.unlink()
        extra.mkdir()
        with self.assertRaises(image.AppImageError):
            image.check_tree(directory, files)

    @unittest.skipUnless(os.name == 'posix', 'POSIX file modes required for AppDir staging checks')
    def test_appdir_changed_mode_and_missing_member_refused(self):
        directory, files = self.appdir()
        target = directory / 'AppRun'
        target.chmod(0o644)
        with self.assertRaises(image.AppImageError):
            image.check_tree(directory, files)
        target.chmod(0o755)
        target.unlink()
        with self.assertRaises(image.AppImageError):
            image.check_tree(directory, files)

    def test_driver_fixed_allowlist_rejects_added_rehashed_payload(self):
        directory, _ = self.appdir()
        driver.validate_image(directory)
        manifest_path = directory / image.MANIFEST
        manifest = json.loads(manifest_path.read_bytes())
        payload = b'authored disallowed payload'
        manifest['files']['extra-game'] = {'bytes': len(payload), 'sha256': image.digest(payload)}
        (directory / 'extra-game').write_bytes(payload)
        manifest_path.write_bytes(image.encoded(manifest))
        with self.assertRaises(ValueError):
            driver.validate_image(directory)

    def test_driver_selection_rejects_parent_traversal_and_external_paths(self):
        directory, _ = self.appdir()
        self.assertEqual(driver.selection_paths(directory, self.root)['game'], self.game)
        path = directory / 'usr/share/monado-ii/selection.json'
        for name in ('../other/game', '/absolute/game', 'game_files/exefs/main', 'Monado_Linux/build/../../outside'):
            path.write_bytes(image.encoded({'schema': 1, 'project': 'Monado II',
                'launcher': 'Monado_Linux/build/gui/monado-launcher', 'game': name}))
            with self.assertRaises(ValueError):
                driver.selection_paths(directory, self.root)

    def test_driver_dry_run_uses_bundled_gui_and_checked_external_paths(self):
        directory, _ = self.appdir()
        appimage = self.root / 'Monado-II-x86_64.AppImage'
        appimage.write_bytes(elf(runtime=True))
        called = []
        wrapper = types.SimpleNamespace(main=lambda args: called.append(args) or 0)
        with mock.patch.object(driver, '__file__', str(directory / 'usr/lib/monado-ii/driver.py')), \
                mock.patch.dict(os.environ, {'APPIMAGE': str(appimage)}), \
                mock.patch.object(driver.importlib, 'import_module', return_value=wrapper):
            self.assertEqual(driver.main(['--dry-run']), 0)
        self.assertEqual(called, [['--launcher-executable', str(self.launcher),
            '--game-executable', str(self.game), '--packaged-launcher', str(directory / 'usr/bin/monado-launcher'),
            '--state-root', str(self.port / 'play/launcher'), '--dry-run']])
        self.assertFalse((self.port / 'play').exists())

    def test_driver_has_no_automatic_play_or_test_input_flags(self):
        for flag in ('--play', '--ui-script', '--game-executable'):
            with self.subTest(flag=flag), mock.patch('sys.stderr'), self.assertRaises(SystemExit) as failure:
                driver.main([flag])
            self.assertEqual(failure.exception.code, 2)

    def test_driver_delegates_checked_active_version_and_preserves_explicit_profile(self):
        directory, _ = self.appdir()
        appimage = self.root / 'Monado-II-x86_64.AppImage'
        appimage.write_bytes(elf(runtime=True))
        active = self.port / 'updates/accepted/workspace/Monado-II-x86_64.AppImage'
        (self.root / 'tools/update_linux.py').write_text(
            'def dispatch_active(workspace, argv):\n'
            '    return [' + repr(str(active)) + ', "--appimage-extract-and-run", *argv]\n')
        state = self.port / 'runs/isolated/state'
        args = ['--dry-run', '--state-root', str(state)]
        with mock.patch.object(driver, '__file__', str(directory / 'usr/lib/monado-ii/driver.py')), \
                mock.patch.dict(os.environ, {'APPIMAGE': str(appimage)}), \
                mock.patch.object(driver.os, 'execv') as execute, \
                mock.patch.object(driver.importlib, 'import_module') as load_launcher:
            self.assertEqual(driver.main(args), 0)
        execute.assert_called_once_with(str(active), [str(active), '--appimage-extract-and-run', *args])
        load_launcher.assert_not_called()
        self.assertFalse(state.exists())

    def test_driver_rejects_external_profile_before_dispatch_or_launcher(self):
        directory, _ = self.appdir()
        appimage = self.root / 'Monado-II-x86_64.AppImage'
        appimage.write_bytes(elf(runtime=True))
        (self.root / 'tools/update_linux.py').write_text('raise AssertionError("must not load updater")\n')
        for state in (self.root / 'game_files/save', self.base / 'outside/save', self.port / 'play'):
            with self.subTest(state=state), \
                    mock.patch.object(driver, '__file__', str(directory / 'usr/lib/monado-ii/driver.py')), \
                    mock.patch.dict(os.environ, {'APPIMAGE': str(appimage)}), \
                    mock.patch('sys.stderr'), self.assertRaises(SystemExit) as failure:
                driver.main(['--dry-run', '--state-root', str(state)])
            self.assertEqual(failure.exception.code, 1)
        self.assertFalse((self.root / 'game_files').exists())

    def test_driver_explicit_original_profile_reaches_checked_launcher(self):
        directory, _ = self.appdir()
        appimage = self.root / 'Monado-II-x86_64.AppImage'
        appimage.write_bytes(elf(runtime=True))
        state = self.port / 'runs/private-validation/state'
        called = []
        wrapper = types.SimpleNamespace(main=lambda args: called.append(args) or 0)
        with mock.patch.object(driver, '__file__', str(directory / 'usr/lib/monado-ii/driver.py')), \
                mock.patch.dict(os.environ, {'APPIMAGE': str(appimage)}), \
                mock.patch.object(driver.importlib, 'import_module', return_value=wrapper):
            self.assertEqual(driver.main(['--state-root', str(state), '--dry-run']), 0)
        self.assertIn(str(state), called[0])
        self.assertFalse(state.exists())

    def test_driver_rejects_malformed_dispatch_command(self):
        directory, _ = self.appdir()
        appimage = self.root / 'Monado-II-x86_64.AppImage'
        appimage.write_bytes(elf(runtime=True))
        for command in ([], ['relative-image'], ['/' + 'authored', 42], ['/' + 'authored\x00']):
            (self.root / 'tools/update_linux.py').write_text(
                'def dispatch_active(workspace, argv):\n    return ' + repr(command) + '\n')
            with self.subTest(command=command), \
                    mock.patch.object(driver, '__file__', str(directory / 'usr/lib/monado-ii/driver.py')), \
                    mock.patch.dict(os.environ, {'APPIMAGE': str(appimage)}), \
                    mock.patch.object(driver.os, 'execv') as execute, \
                    mock.patch('sys.stderr'), self.assertRaises(SystemExit) as failure:
                driver.main(['--dry-run'])
            self.assertEqual(failure.exception.code, 1)
            execute.assert_not_called()

    def test_bound_game_selection_uses_only_the_marker_selected_port(self):
        workspace = self.port / 'updates/accepted/workspace'
        candidate = workspace / 'Monado_Linux'
        (candidate / 'scripts').mkdir(parents=True)
        shutil.copyfile(ROOT / 'Monado_Linux/scripts/install_layout.py', candidate / 'scripts/install_layout.py')
        (workspace / 'installation.json').write_text(json.dumps({'schema': 1,
            'owner': str(self.root), 'workspace': str(workspace), 'game_port': str(self.port)}))
        gui = candidate / 'build/new-gui/monado-launcher'
        gui.parent.mkdir(parents=True)
        gui.write_bytes(elf())
        directory, _ = self.appdir()
        selection = directory / 'usr/share/monado-ii/selection.json'
        selection.write_bytes(image.encoded({'schema': 1, 'project': 'Monado II',
            'launcher': 'Monado_Linux/build/new-gui/monado-launcher',
            'game': 'Monado_Linux/build/game/xcde-linux', 'game_port': 'bound'}))
        self.assertEqual(driver.selection_paths(directory, workspace), {'launcher': gui, 'game': self.game})
        (workspace / 'installation.json').unlink()
        with self.assertRaisesRegex(ValueError, 'separate bound'):
            driver.selection_paths(directory, workspace)

    def test_managed_driver_keeps_original_profile_and_bound_game(self):
        workspace = self.port / 'updates/accepted/workspace'
        candidate = workspace / 'Monado_Linux'
        (candidate / 'scripts').mkdir(parents=True)
        shutil.copyfile(ROOT / 'Monado_Linux/scripts/install_layout.py', candidate / 'scripts/install_layout.py')
        (workspace / 'installation.json').write_text(json.dumps({'schema': 1,
            'owner': str(self.root), 'workspace': str(workspace), 'game_port': str(self.port)}))
        (workspace / 'tools').mkdir()
        (workspace / 'tools/update_linux.py').write_text('def dispatch_active(workspace, argv):\n    return None\n')
        gui = candidate / 'build/new-gui/monado-launcher'
        gui.parent.mkdir(parents=True)
        gui.write_bytes(elf())
        appimage = workspace / 'Monado-II-x86_64.AppImage'
        appimage.write_bytes(elf(runtime=True))
        directory, files = self.appdir()
        selection = 'usr/share/monado-ii/selection.json'
        files[selection] = (image.encoded({'schema': 1, 'project': 'Monado II',
            'launcher': 'Monado_Linux/build/new-gui/monado-launcher',
            'game': 'Monado_Linux/build/game/xcde-linux', 'game_port': 'bound'}), 0o644)
        files[image.MANIFEST] = (image.encoded({'schema': 1, 'project': 'Monado II',
            'files': image.identity_map({name: value for name, value in files.items() if name != image.MANIFEST})}), 0o644)
        for name in (selection, image.MANIFEST):
            (directory / name).write_bytes(files[name][0])
        called = []
        wrapper = types.SimpleNamespace(main=lambda args: called.append(args) or 0)
        state = self.port / 'runs/preserved-profile/state'
        with mock.patch.object(driver, '__file__', str(directory / 'usr/lib/monado-ii/driver.py')), \
                mock.patch.dict(os.environ, {'APPIMAGE': str(appimage)}), \
                mock.patch.object(driver.importlib, 'import_module', return_value=wrapper):
            self.assertEqual(driver.main(['--dry-run', '--state-root', str(state)]), 0)
        self.assertEqual(called, [['--launcher-executable', str(gui), '--game-executable', str(self.game),
            '--packaged-launcher', str(directory / 'usr/bin/monado-launcher'), '--state-root', str(state), '--dry-run']])
        self.assertFalse(state.exists())
        self.assertFalse((candidate / 'play').exists())


@unittest.skipUnless(sys.platform == 'linux' and shutil.which('mksquashfs') and shutil.which('unsquashfs'),
                     'native Linux squashfs-tools required for archive readback fixtures')
class ActualSquashfsFixtures(FixtureBase):
    def test_private_caller_umask_roundtrip_preserves_caller_and_parent_masks(self):
        def current_mask():
            return int(next(line.split(':', 1)[1].strip() for line in
                Path('/proc/self/status').read_text().splitlines() if line.startswith('Umask:')), 8)
        before = current_mask()
        script = '''import importlib.util,json,pathlib,sys
root=pathlib.Path(sys.argv[1])
spec=importlib.util.spec_from_file_location("monado_private_umask_fixture",root/"tests/test_launcher_appimage.py")
fixture_module=importlib.util.module_from_spec(spec)
spec.loader.exec_module(fixture_module)
def mask():
    return int(next(line.split(":",1)[1].strip() for line in pathlib.Path("/proc/self/status").read_text().splitlines() if line.startswith("Umask:")),8)
fixture=fixture_module.ActualSquashfsFixtures("test_actual_archive_bytes_deterministic_for_same_local_selection")
fixture.setUp()
try:
    before=mask()
    assert before==0o077,before
    receipt=fixture.package("private-caller-mask")
    after=mask()
    assert after==before,(before,after)
    print(json.dumps({"before":before,"after":after,"readback":receipt["archive_readback_verified"]}))
finally:
    fixture.doCleanups()
'''
        result = subprocess.run([sys.executable, '-I', '-B', '-c', script, str(ROOT)],
            capture_output=True, text=True, timeout=180, umask=0o077)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(json.loads(result.stdout), {'before': 0o077, 'after': 0o077, 'readback': True})
        self.assertEqual(current_mask(), before)

    def test_actual_archive_readback_contains_only_launcher_and_authored_metadata(self):
        receipt = self.package()
        output = Path(receipt['output'])
        self.assertTrue(receipt['archive_readback_verified'])
        self.assertEqual(receipt['sha256'], image.digest(output.read_bytes()))
        self.assertEqual(output.stat().st_nlink, 1)
        self.assertFalse(receipt['contains_game_files'])
        self.assertFalse(receipt['contains_game_executable'])
        self.assertEqual(set(receipt['appdir_files']), driver.EXPECTED | {image.MANIFEST})
        self.assertFalse(any('game_files' in name or 'xcde-linux' in name for name in receipt['appdir_files']))
        self.assertNotIn(b'PRIVATE authored fixture', output.read_bytes())
        saved = json.loads(output.with_name(output.name + '.receipt.json').read_bytes())
        self.assertEqual(saved, receipt)

    def test_actual_archive_bytes_deterministic_for_same_local_selection(self):
        first = self.package('first')
        second = self.package('second')
        self.assertEqual(first['sha256'], second['sha256'])

    def test_readback_extra_file_rejected_before_final_output(self):
        original = image.command
        def corrupt(argv, commands):
            result = original(argv, commands)
            if Path(argv[0]).name == 'unsquashfs':
                root = Path(argv[argv.index('-dest') + 1])
                (root / 'unallowed-extra').write_text('authored extra')
            return result
        with mock.patch.object(image, 'command', side_effect=corrupt), self.assertRaises(image.AppImageError):
            self.package()
        self.assertFalse((self.port / 'build/result/Monado-II-x86_64.AppImage').exists())

    def test_managed_archive_records_bound_game_without_copying_its_bytes(self):
        workspace = self.port / 'updates/reuse/workspace'
        candidate = workspace / 'Monado_Linux'
        candidate.mkdir(parents=True)
        (workspace / 'installation.json').write_text(json.dumps({'schema': 1,
            'owner': str(self.root), 'workspace': str(workspace), 'game_port': str(self.port)}))
        for name in ('LICENSE', 'CREDITS.md', 'NOTICE.md',
                'Monado_Linux/third_party/imgui/LICENSE.txt',
                'Monado_Linux/tools/launcher/assets/fonts/LICENSE-DejaVu.txt'):
            target = workspace / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(self.root / name, target)
        gui = candidate / 'build/new-gui/monado-launcher'
        gui.parent.mkdir(parents=True)
        gui.write_bytes(elf())
        gui.chmod(0o755)
        provider = lambda launcher, game, root: {'launcher_sha256': image.digest(gui.read_bytes()),
                                                'game_sha256': image.digest(self.game.read_bytes())}
        output = candidate / 'build/image/Monado-II-x86_64.AppImage'
        original = self.game.read_bytes()
        receipt = image.package(gui, self.game, output, root=workspace,
            selection_provider=provider, runtime_provider=lambda port: elf(runtime=True))
        self.assertEqual(receipt['game_port'], 'bound')
        self.assertEqual(receipt['game'], 'Monado_Linux/build/game/xcde-linux')
        self.assertTrue(receipt['archive_readback_verified'])
        self.assertNotIn(original, output.read_bytes())
        self.assertEqual(self.game.read_bytes(), original)


if __name__ == '__main__':
    unittest.main()
