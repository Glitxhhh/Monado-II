"""Authored path fixtures for the source-built private packaging interpreter."""
from pathlib import Path
import contextlib
import hashlib
import io
import importlib.util
import json
import os
import stat
import subprocess
import sys
import tarfile
import tempfile
import time
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
SCRIPTS = ROOT / 'Monado_Linux/scripts'
sys.path.insert(0, str(SCRIPTS))
spec = importlib.util.spec_from_file_location('portable_python_paths', SCRIPTS / 'setup_portable_python.py')
recipe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(recipe)


class PortablePythonPaths(unittest.TestCase):
    @unittest.skipUnless(sys.platform == 'linux', 'Linux executable compiler wrapper and shell argument boundary')
    def test_compiler_command_preserves_path_and_arguments(self):
        with tempfile.TemporaryDirectory(prefix='monado-python-path-test-') as temporary:
            root = Path(temporary)
            source = root / "compiler's files $(unused)"
            source.mkdir()
            compiler = source / 'authored compiler'
            compiler.write_text('#!/usr/bin/python3\nimport json,sys\nprint(json.dumps(sys.argv[1:]))\n')
            compiler.chmod(0o700)
            entry = recipe.compiler_entry(root, 'cc', compiler)
            arguments = ['-Ipath with spaces', "quote'argument", '$HOME', 'semi;colon']
            result = subprocess.run([entry, *arguments], capture_output=True, text=True, check=True)
            self.assertEqual(json.loads(result.stdout), arguments)
            self.assertFalse((root / 'unused').exists())

    @unittest.skipUnless(sys.platform == 'linux', 'Linux private /tmp workspace and POSIX directory permissions')
    def test_build_uses_private_physical_tmp_despite_spaced_destination_and_tmpdir(self):
        with tempfile.TemporaryDirectory(prefix='monado-python-build-test-') as temporary:
            root = Path(temporary) / 'My Games' / 'Monado II'
            output = root / '.deps/portable/python/fresh'
            poisoned = Path(temporary) / 'TMP DIR'
            poisoned.mkdir()
            seen = []
            def build_private(out, workspace, *_):
                self.assertEqual(out, output)
                self.assertEqual(workspace.parent, Path('/tmp'))
                self.assertEqual(workspace, workspace.resolve())
                self.assertFalse(any(c.isspace() for c in str(workspace)))
                self.assertEqual(stat.S_IMODE(workspace.stat().st_mode), 0o700)
                seen.append(workspace)
                return {'status': 'authored fixture passed'}
            with mock.patch.object(recipe, 'ROOT', root), \
                 mock.patch.object(recipe.baseline, 'prepare', return_value={}), \
                 mock.patch.object(recipe, 'download', return_value=Path('/unused-archive')), \
                 mock.patch.object(recipe, 'download_development', return_value=[]), \
                 mock.patch.object(recipe, 'build_private', side_effect=build_private), \
                 mock.patch.dict(os.environ, {'TMPDIR': str(poisoned)}):
                result = recipe.build(output)
            self.assertEqual(result['status'], 'authored fixture passed')
            self.assertEqual(len(seen), 1)
            self.assertFalse(seen[0].exists())
            self.assertEqual(list(poisoned.iterdir()), [])

    @unittest.skipUnless(sys.platform == 'linux', 'owned Linux process group')
    def test_timeout_stops_child_even_when_parent_exits_before_child(self):
        with tempfile.TemporaryDirectory(prefix='monado-python-timeout-test-') as temporary:
            root=Path(temporary); pidfile=root/'child.pid'
            child='import os,pathlib,signal,time;signal.signal(signal.SIGTERM,signal.SIG_IGN);pathlib.Path('+repr(str(pidfile))+').write_text(str(os.getpid()));time.sleep(60)'
            parent='import subprocess,sys;subprocess.Popen([sys.executable,"-c",'+repr(child)+']);__import__("time").sleep(60)'
            with (root/'process.log').open('wb') as log:
                with self.assertRaises(subprocess.TimeoutExpired):
                    recipe.run_owned_process([sys.executable,'-I','-B','-c',parent],
                        cwd=root,env={'PATH':'/usr/bin:/bin'},stdout=log,timeout=1)
            self.assertTrue(pidfile.is_file(), 'owned child did not start')
            child_pid=int(pidfile.read_text())
            status=Path('/proc')/str(child_pid)/'stat'
            deadline=time.monotonic()+2
            while status.exists():
                # Signal delivery can precede the final scheduler transition.
                # An adopted zombie executes no more code and only awaits the
                # host's reaper; it cannot write the temporary build tree.
                state=status.read_text().split(') ',1)[1].split()[0]
                if state=='Z': break
                if time.monotonic()>=deadline: self.fail('owned child survived timeout: '+state)
                time.sleep(.01)


@unittest.skipUnless(sys.platform == 'linux', 'Linux portable source preparation and private build directory')
class PortablePythonBaselinePreparation(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='monado-python-baseline-')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name) / 'My Games' / 'Monado II'
        self.root.mkdir(parents=True)
        self.lock_path = self.root / 'portable-baseline-linux.lock.json'
        self.lock = {'packages': [], 'sources': []}
        self.payloads = {}
        for name, directory, url in (
                ('SDL3', 'SDL3-3.4.8', 'https://github.com/libsdl-org/SDL/releases/download/authored/sdl.tar.gz'),
                ('FFmpeg', 'ffmpeg-8.0.1', 'https://ffmpeg.org/releases/authored-ffmpeg.tar.gz')):
            stream = io.BytesIO()
            with tarfile.open(fileobj=stream, mode='w:gz') as archive:
                content = ('authored ' + name + ' source fixture\n').encode()
                entry = tarfile.TarInfo(directory + '/file.c')
                entry.size = len(content); entry.mode = 0o644
                archive.addfile(entry, io.BytesIO(content))
            data = stream.getvalue()
            self.payloads[url] = data
            self.lock['sources'].append({'name': name, 'filename': name + '.tar.gz',
                'url': url, 'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()})
        self.lock_path.write_text(json.dumps(self.lock))
        self.requests = []

    def opener(self, url, timeout):
        self.assertEqual(timeout, 60)
        self.requests.append(url)
        # Intercept transport only. The actual baseline downloader, size/hash
        # guards, atomic publication and source inventory checks all execute.
        return io.BytesIO(self.payloads[url])

    def build(self, name, opener=None):
        self.private_build = mock.Mock(side_effect=lambda out, workspace, prepared, *_:
                                       {'status': 'authored fixture passed', 'prepared': prepared})
        self.python_download = mock.Mock(return_value=Path('/unused-python-archive'))
        self.development_download = mock.Mock(return_value=[])
        with contextlib.ExitStack() as stack:
            for obj, attr, value in (
                    (recipe, 'ROOT', self.root), (recipe.baseline, 'ROOT', self.root),
                    (recipe.baseline, 'LOCK', self.lock_path),
                    (recipe, 'download', self.python_download),
                    (recipe, 'download_development', self.development_download),
                    (recipe, 'build_private', self.private_build),
                    (recipe.baseline.urllib.request, 'urlopen', opener or self.opener)):
                stack.enter_context(mock.patch.object(obj, attr, value))
            return recipe.build(self.root / '.deps/portable/python' / name)

    def test_empty_baseline_cache_is_prepared_before_explicit_python_build(self):
        self.assertFalse((self.root / '.deps/portable').exists())
        result = self.build('fresh')
        self.assertEqual(result['status'], 'authored fixture passed')
        self.assertCountEqual(self.requests, self.payloads)
        for row in self.lock['sources']:
            cached = self.root / '.deps/portable/cache' / row['filename']
            self.assertEqual(cached.read_bytes(), self.payloads[row['url']])
        prepared = result['prepared']
        self.assertTrue((Path(prepared['sysroot']) / '.monado-portable.json').is_file())
        self.assertTrue((Path(prepared['sources']) / recipe.baseline.SOURCE_MANIFEST).is_file())
        self.python_download.assert_called_once_with()
        self.development_download.assert_called_once_with()
        self.private_build.assert_called_once()

    def test_verified_baseline_cache_is_reused_without_network(self):
        first = self.build('first')
        cached = self.root / '.deps/portable/cache'
        before = {p.name: (p.read_bytes(), p.stat().st_mtime_ns) for p in cached.iterdir()}
        blocked = mock.Mock(side_effect=AssertionError('authored offline transport guard'))
        second = self.build('second', blocked)
        blocked.assert_not_called()
        self.assertEqual(first['prepared'], second['prepared'])
        self.assertEqual(before, {p.name: (p.read_bytes(), p.stat().st_mtime_ns) for p in cached.iterdir()})

    def test_changed_cached_archive_is_preserved_and_stops_before_python_build(self):
        self.build('first')
        cached = self.root / '.deps/portable/cache' / self.lock['sources'][0]['filename']
        cached.write_bytes(b'authored corrupt archive')
        blocked = mock.Mock(side_effect=AssertionError('corrupt cache must not be downloaded again'))
        with self.assertRaisesRegex(ValueError, 'cached portable dependency identity differs'):
            self.build('rejected', blocked)
        blocked.assert_not_called()
        self.assertEqual(cached.read_bytes(), b'authored corrupt archive')
        self.assertFalse((self.root / '.deps/portable/python/rejected').exists())
        self.python_download.assert_not_called()
        self.development_download.assert_not_called()
        self.private_build.assert_not_called()

    def test_changed_verified_source_is_preserved_and_stops_before_python_build(self):
        first = self.build('first')
        source = Path(first['prepared']['sources']) / 'SDL3-3.4.8/file.c'
        source.write_bytes(b'authored changed source')
        blocked = mock.Mock(side_effect=AssertionError('source cache must not be replaced'))
        with self.assertRaisesRegex(ValueError, 'portable source cache is incomplete or changed'):
            self.build('rejected', blocked)
        blocked.assert_not_called()
        self.assertEqual(source.read_bytes(), b'authored changed source')
        self.assertFalse((self.root / '.deps/portable/python/rejected').exists())
        self.python_download.assert_not_called()
        self.development_download.assert_not_called()
        self.private_build.assert_not_called()


if __name__ == '__main__':
    unittest.main()
