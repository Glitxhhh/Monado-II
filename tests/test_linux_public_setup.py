"""Authored bootstrap/resource fixtures; no network, compiler, game or user state."""
import contextlib
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
import struct
import tempfile
import types
import unittest
from unittest import mock
import zlib


ROOT = Path(__file__).resolve().parents[1]
LINUX = ROOT / 'Monado_Linux'
if LINUX.is_dir() and not (LINUX / 'requirements-linux.lock').is_file():
    raise RuntimeError('Linux source tree is present but its reviewed wheel lock is missing')


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


setup = load_module('monado_linux_public_setup', ROOT / 'tools/setup_linux_tools.py')
resource_path = ROOT / 'publication/overrides/linux/tools/launcher/theme_resource.py'
if not resource_path.is_file():
    resource_path = LINUX / 'tools/launcher/theme_resource.py'
if LINUX.is_dir() and not resource_path.is_file():
    raise RuntimeError('Linux source tree is present but its launcher resource module is missing')
resource = load_module('monado_public_theme_resource', resource_path) if resource_path.is_file() else None


def inventory(root):
    return {str(path.relative_to(root)): (path.read_bytes(), path.stat().st_mode)
            for path in root.rglob('*') if path.is_file()}


@unittest.skipUnless((LINUX / 'requirements-linux.lock').is_file(), 'Linux source lock required')
class LinuxPublicSetup(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='monado-public-setup-')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.port = self.root / 'port with spaces'
        self.port.mkdir()
        shutil.copyfile(LINUX / 'requirements-linux.lock', self.port / 'requirements-linux.lock')
        self.host = dict(implementation='CPython', version=[3, 14, 4], platform='linux', machine='x86_64')
        patch = mock.patch.object(setup, 'host_info', return_value=self.host)
        patch.start(); self.addCleanup(patch.stop)
        self.commands = []
        self.packages = dict(setup.PACKAGES)
        self.real_pip_wheel = setup.pip_wheel
        patch = mock.patch.object(setup, 'pip_wheel', side_effect=self.fake_pip_wheel)
        self.pip_download = patch.start(); self.addCleanup(patch.stop)

    def fake_pip_wheel(self, port):
        path = port / '.deps/setup-tmp' / setup.PIP_WHEEL['filename']
        path.write_bytes(b'AUTHORED NEVER EXECUTED BOOTSTRAP WHEEL FIXTURE')
        return path

    def venv(self):
        directory = self.port / '.venv'
        (directory / 'bin').mkdir(parents=True)
        (directory / 'pyvenv.cfg').write_text('home = /usr/bin\nversion = 3.14.4\n')
        (directory / 'bin/python').write_text('AUTHORED NEVER EXECUTED PYTHON FIXTURE\n')

    def wrappers(self):
        (self.port / '.deps/bin').mkdir(parents=True)
        for name, driver in (('clang-cc', 'cc'), ('clang-cxx', 'c++')):
            path = self.port / '.deps/bin' / name
            path.write_text(setup.wrapper_text(driver))
            path.chmod(0o755)

    def runner(self, argv, **kwargs):
        self.commands.append((argv, kwargs))
        self.assertEqual(kwargs['cwd'], str(self.port))
        self.assertNotIn('PYTHONPATH', kwargs['env'])
        self.assertNotIn('LD_PRELOAD', kwargs['env'])
        self.assertNotIn('PYTHONHOME', kwargs['env'])
        if '-m' in argv and argv[argv.index('-m') + 1] == 'venv':
            if '--without-pip' not in argv:
                return types.SimpleNamespace(returncode=1, stdout='ensurepip is unavailable; venv --without-pip required', stderr='')
            self.venv(); output = ''
            self.packages = {name: None for name in setup.PACKAGES}
        elif '-m' in argv and argv[argv.index('-m') + 1] == 'pip':
            output = 'AUTHORED PINNED INSTALL RESULT\n'
            self.packages = dict(setup.PACKAGES)
        elif setup.BOOTSTRAP_PIP in argv:
            self.packages['pip'] = setup.PACKAGES['pip']
            output = 'AUTHORED PIP WHEEL INSTALLED INTO PORT VENV\n'
        elif '-c' in argv:
            output = json.dumps(dict(self.host, prefix=str(self.port / '.venv'),
                                     base_prefix='/usr', packages=self.packages))
        elif argv[-3:] == ['-m', 'ziglang', 'version']:
            output = '0.14.1\n'
        else:
            self.fail('unexpected process request: ' + repr(argv))
        return types.SimpleNamespace(returncode=0, stdout=output, stderr='')

    def test_default_plan_creates_nothing_and_executes_nothing(self):
        before = inventory(self.port)
        with mock.patch.object(setup.subprocess, 'run', side_effect=AssertionError('plan executed a process')):
            with contextlib.redirect_stdout(io.StringIO()) as output:
                self.assertEqual(setup.main(['--port', str(self.port)]), 0)
        document = json.loads(output.getvalue())
        self.assertEqual(document['mode'], 'plan')
        self.assertEqual(before, inventory(self.port))
        self.assertEqual(sorted(path.name for path in self.port.iterdir()), ['requirements-linux.lock'])

    def test_existing_environment_check_is_read_only_and_offline(self):
        self.venv(); self.wrappers()
        before = inventory(self.port)
        with mock.patch.dict(os.environ, {'PYTHONPATH': '/untrusted', 'LD_PRELOAD': '/untrusted'}):
            result = setup.check(setup.checked_port(self.port), runner=self.runner)
        self.assertEqual(result['status'], 'passed')
        self.assertEqual(result['compiler_target'], 'x86_64-linux-gnu')
        self.assertEqual(before, inventory(self.port))
        self.assertEqual(len(self.commands), 2)
        self.assertTrue(all('pip' not in argv and 'venv' not in argv for argv, _ in self.commands))

    def test_explicit_install_uses_only_hash_locked_binary_wheels_and_port_local_outputs(self):
        result = setup.install(self.port, runner=self.runner)
        self.assertEqual(result['mode'], 'install')
        self.assertEqual(result['status'], 'passed')
        installs = [argv for argv, _ in self.commands if 'pip' in argv]
        self.assertEqual(len(installs), 1)
        for flag in ('--isolated', '--require-hashes', '--only-binary=:all:', '--no-deps', '--no-cache-dir', '--no-input'):
            self.assertIn(flag, installs[0])
        self.assertEqual(installs[0][0], str(self.port / '.venv/bin/python'))
        self.assertEqual(installs[0][-1], str(self.port / 'requirements-linux.lock'))
        venv = [argv for argv, _ in self.commands if 'venv' in argv]
        self.assertIn('--without-pip', venv[0])
        bootstraps = [argv for argv, _ in self.commands if setup.BOOTSTRAP_PIP in argv]
        self.assertEqual(len(bootstraps), 1)
        self.assertEqual(bootstraps[0][0], str(self.port / '.venv/bin/python'))
        for flag in ('--no-index', '--no-deps', '--no-cache-dir', '--ignore-installed', '--disable-pip-version-check'):
            self.assertIn(flag, setup.BOOTSTRAP_PIP)
        self.assertEqual(sorted(path.name for path in self.port.iterdir()), ['.deps', '.venv', 'requirements-linux.lock'])
        saved = json.loads((self.port / '.deps/metadata/linux-tools.json').read_text())
        self.assertEqual(saved, result)
        self.assertFalse((self.port / 'game_files').exists())
        self.assertFalse((self.port / '.deps/cache').exists())
        for name in ('clang-cc', 'clang-cxx'):
            text = (self.port / '.deps/bin' / name).read_text()
            self.assertIn('-target x86_64-linux-gnu "$@"', text)
            self.assertIn('ZIG_LOCAL_CACHE_DIR="$task_root/.deps/cache/zig-local"', text)
            self.assertTrue(os.access(self.port / '.deps/bin' / name, os.X_OK))
            if os.name == 'posix':
                self.assertTrue((self.port / '.deps/bin' / name).stat().st_mode & 0o100)

    def test_unrecognized_wrapper_is_preserved_before_any_install(self):
        (self.port / '.deps/bin').mkdir(parents=True)
        custom = self.port / '.deps/bin/clang-cc'
        custom.write_text('a user compiler wrapper that must survive\n')
        before = inventory(self.port)
        with self.assertRaisesRegex(ValueError, 'different existing compiler wrapper'):
            setup.install(self.port, runner=self.runner)
        self.assertEqual(before, inventory(self.port))
        self.assertFalse(self.commands)

    def test_lock_drift_is_rejected_before_processes_or_outputs(self):
        lock = self.port / 'requirements-linux.lock'
        lock.write_bytes(lock.read_bytes() + b'\n# unreviewed edit\n')
        with self.assertRaisesRegex(ValueError, 'reviewed CPython'):
            setup.install(self.port, runner=self.runner)
        self.assertFalse(self.commands)
        self.assertFalse((self.port / '.venv').exists())
        self.assertFalse((self.port / '.deps').exists())

    def test_missing_or_wrong_package_cannot_pass_check(self):
        self.venv(); self.wrappers()
        self.packages['ziglang'] = None
        before = inventory(self.port)
        with self.assertRaisesRegex(ValueError, 'package versions'):
            setup.check(self.port, runner=self.runner)
        self.assertEqual(before, inventory(self.port))

    def test_zig_binary_version_is_checked_separately(self):
        self.venv(); self.wrappers()
        def wrong_zig(argv, **kwargs):
            result = self.runner(argv, **kwargs)
            if argv[-3:] == ['-m', 'ziglang', 'version']:
                result.stdout = '0.15.0\n'
            return result
        with self.assertRaisesRegex(ValueError, 'Zig executable version'):
            setup.check(self.port, runner=wrong_zig)

    def test_incomplete_existing_venv_is_not_overwritten(self):
        (self.port / '.venv').mkdir()
        preserved = self.port / '.venv/unrelated-file'
        preserved.write_text('preserve')
        with self.assertRaisesRegex(ValueError, 'missing/incomplete'):
            setup.install(self.port, runner=self.runner)
        self.assertEqual(preserved.read_text(), 'preserve')
        self.assertFalse(self.commands)

    def test_unsupported_host_is_rejected_without_changes(self):
        self.host['version'] = [3, 13, 9]
        before = inventory(self.port)
        with contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit) as raised:
                setup.main(['--port', str(self.port), '--install'])
        self.assertEqual(raised.exception.code, 1)
        self.assertEqual(before, inventory(self.port))

    def test_dependency_directory_symlink_is_rejected(self):
        elsewhere = self.root / 'elsewhere'
        elsewhere.mkdir()
        try:
            (self.port / '.deps').symlink_to(elsewhere, target_is_directory=True)
        except OSError:
            self.skipTest('host disallows symlink fixtures')
        with self.assertRaisesRegex(ValueError, 'ordinary'):
            setup.install(self.port, runner=self.runner)
        self.assertFalse(list(elsewhere.iterdir()))
        self.assertFalse(self.commands)

    def test_compiler_cache_link_cannot_redirect_future_builds(self):
        (self.port / '.deps/cache').mkdir(parents=True)
        elsewhere = self.root / 'immutable-input-fixture'
        elsewhere.mkdir()
        try:
            (self.port / '.deps/cache/zig-local').symlink_to(elsewhere, target_is_directory=True)
        except OSError:
            self.skipTest('host disallows symlink fixtures')
        with self.assertRaisesRegex(ValueError, 'ordinary'):
            setup.install(self.port, runner=self.runner)
        self.assertFalse(list(elsewhere.iterdir()))
        self.assertFalse(self.commands)

    def test_existing_valid_venv_without_pip_is_repaired_without_recreation(self):
        self.venv(); self.wrappers()
        self.packages['pip'] = None
        source = (self.port / '.venv/bin/python').read_bytes()
        result = setup.install(self.port, runner=self.runner)
        self.assertEqual(result['status'], 'passed')
        self.assertFalse(any('venv' in argv for argv, _ in self.commands))
        self.assertEqual((self.port / '.venv/bin/python').read_bytes(), source)
        self.assertEqual(sum(setup.BOOTSTRAP_PIP in argv for argv, _ in self.commands), 1)

    def test_existing_pip_avoids_bootstrap_download_and_check_stays_offline(self):
        self.venv(); self.wrappers()
        setup.install(self.port, runner=self.runner)
        self.pip_download.assert_not_called()
        self.assertFalse(any(setup.BOOTSTRAP_PIP in argv for argv, _ in self.commands))
        before = inventory(self.port)
        setup.check(self.port, runner=self.runner)
        self.assertEqual(before, inventory(self.port))
        self.pip_download.assert_not_called()

    def test_foreign_venv_prefix_is_refused_before_install_or_download(self):
        self.venv()
        def foreign(argv, **kwargs):
            result = self.runner(argv, **kwargs)
            if setup.PROBE in argv:
                info = json.loads(result.stdout); info['prefix'] = str(self.root / 'unrelated')
                result.stdout = json.dumps(info)
            return result
        before = inventory(self.port)
        with self.assertRaisesRegex(ValueError, 'not using this port-local'):
            setup.install(self.port, runner=foreign)
        self.assertEqual(before, inventory(self.port))
        self.pip_download.assert_not_called()
        self.assertFalse(any('pip' in argv or setup.BOOTSTRAP_PIP in argv for argv, _ in self.commands))

    def test_bootstrap_must_really_install_pip_not_only_import_it_from_wheel(self):
        self.venv(); self.packages['pip'] = None
        def no_install(argv, **kwargs):
            if setup.BOOTSTRAP_PIP in argv:
                return types.SimpleNamespace(returncode=0, stdout='wheel visible but not installed', stderr='')
            return self.runner(argv, **kwargs)
        with self.assertRaisesRegex(ValueError, 'did not install pip into'):
            setup.install(self.port, runner=no_install)
        self.assertFalse(any('pip' in argv for argv, _ in self.commands))

    def test_failed_command_reports_stdout_when_stderr_is_empty(self):
        def failed(argv, **kwargs):
            return types.SimpleNamespace(returncode=1, stdout='ensurepip explanation on stdout', stderr='')
        with self.assertRaisesRegex(RuntimeError, 'ensurepip explanation on stdout'):
            setup.run(['authored-never-executed'], self.port, runner=failed)

    def wheel_fixture(self, data=b'AUTHORED VERIFIED WHEEL DOWNLOAD FIXTURE'):
        (self.port / '.deps/setup-tmp').mkdir(parents=True)
        identity = dict(setup.PIP_WHEEL, bytes=len(data), sha256=hashlib.sha256(data).hexdigest())
        response = io.BytesIO(data)
        response.geturl = lambda: identity['url']
        return identity, response

    def test_bootstrap_download_is_bounded_hashed_and_cached_only_locally(self):
        identity, response = self.wheel_fixture()
        requested = []
        def fetch(request, timeout):
            requested.append((request.full_url, timeout)); return response
        with mock.patch.object(setup, 'PIP_WHEEL', identity):
            wheel = self.real_pip_wheel(self.port, opener=fetch)
            self.assertEqual(wheel.read_bytes(), b'AUTHORED VERIFIED WHEEL DOWNLOAD FIXTURE')
            self.assertEqual(wheel.parent, self.port / '.deps/setup-tmp')
            with mock.patch.object(setup.urllib.request, 'build_opener', side_effect=AssertionError('cached wheel fetched again')):
                self.assertEqual(self.real_pip_wheel(self.port), wheel)
        self.assertEqual(requested, [(identity['url'], 30)])
        self.assertEqual(list(wheel.parent.glob('*.partial')), [])

    def test_bad_download_is_refused_without_publishing_a_wheel(self):
        identity, response = self.wheel_fixture()
        response = io.BytesIO(b'X' * (identity['bytes'] + 500))
        response.geturl = lambda: identity['url']
        with mock.patch.object(setup, 'PIP_WHEEL', identity):
            with self.assertRaisesRegex(ValueError, 'hash/size mismatch'):
                self.real_pip_wheel(self.port, opener=lambda *args, **kwargs: response)
        self.assertEqual(list((self.port / '.deps/setup-tmp').iterdir()), [])

    def test_cache_symlink_is_rejected_without_reading_or_writing_target(self):
        identity, _ = self.wheel_fixture()
        target = self.root / 'protected-authored-fixture'
        target.write_text('preserve')
        wheel = self.port / '.deps/setup-tmp' / identity['filename']
        try:
            wheel.symlink_to(target)
        except OSError:
            self.skipTest('host disallows symlink fixtures')
        with self.assertRaisesRegex(ValueError, 'ordinary single-link'):
            self.real_pip_wheel(self.port, opener=lambda *args, **kwargs: self.fail('downloaded'))
        self.assertEqual(target.read_text(), 'preserve')

    def test_bad_cached_hash_is_preserved_without_download_or_execution(self):
        identity, _ = self.wheel_fixture()
        wheel = self.port / '.deps/setup-tmp' / identity['filename']
        wheel.write_text('preserve bad cached bytes')
        with mock.patch.object(setup, 'PIP_WHEEL', identity):
            with self.assertRaisesRegex(ValueError, 'cached pip bootstrap wheel hash/size mismatch'):
                self.real_pip_wheel(self.port, opener=lambda *args, **kwargs: self.fail('downloaded'))
        self.assertEqual(wheel.read_text(), 'preserve bad cached bytes')

    def test_unexpected_or_redirected_origin_is_refused(self):
        identity, response = self.wheel_fixture()
        with mock.patch.object(setup, 'PIP_WHEEL', dict(identity, url='https://example.invalid/pip.whl')):
            with self.assertRaisesRegex(ValueError, 'unexpected pip bootstrap origin'):
                self.real_pip_wheel(self.port, opener=lambda *args, **kwargs: self.fail('contacted unexpected origin'))
        response.geturl = lambda: 'https://example.invalid/redirected'
        with mock.patch.object(setup, 'PIP_WHEEL', identity):
            with self.assertRaisesRegex(ValueError, 'changed URL'):
                self.real_pip_wheel(self.port, opener=lambda *args, **kwargs: response)


@unittest.skipIf(resource is None, 'Linux source resources required')
class OptionalPublicArtwork(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='monado-public-art-')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.assets = self.root / 'assets'
        (self.assets / 'fonts').mkdir(parents=True)
        for name, symbol in resource.GROUPS['launcher_fonts.inc']:
            (self.assets / name).write_bytes(b'\x00\x01\x00\x00' + symbol.encode())
        (self.assets / resource.FONT_LICENSE).write_text('Authored fixture font license.\n')

    def test_fonts_embed_without_scenic_art_and_no_art_include_is_emitted(self):
        out = self.root / 'includes'
        result = resource.generate(self.assets, out)
        self.assertEqual(result['features'], {'fonts': True, 'artwork': False})
        self.assertEqual(set(result['generated']), {'launcher_fonts.inc', 'DejaVu-font-license.txt'})
        self.assertFalse((out / 'launcher_art.inc').exists())
        for _, symbol in resource.GROUPS['launcher_fonts.inc']:
            self.assertIn(symbol, (out / 'launcher_fonts.inc').read_text())
        self.assertEqual((out / 'DejaVu-font-license.txt').read_bytes(),
                         (self.assets / resource.FONT_LICENSE).read_bytes())

    def test_existing_valid_local_art_is_still_embedded_and_declared(self):
        def chunk(kind, data):
            body = kind + data
            return struct.pack('>I', len(data)) + body + struct.pack('>I', zlib.crc32(body))
        data = (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 1, 1, 8, 2, 0, 0, 0)) +
                chunk(b'IDAT', zlib.compress(b'\0\x12\x34\x56')) + chunk(b'IEND', b''))
        (self.assets / 'gaur_plain.png').write_bytes(data)
        result = resource.generate(self.assets, self.root / 'includes')
        self.assertTrue(result['features']['artwork'])
        self.assertIn('gaur_plain.png', result['assets'])
        self.assertIn('monado_art_png', (self.root / 'includes/launcher_art.inc').read_text())


if __name__ == '__main__':
    unittest.main()
