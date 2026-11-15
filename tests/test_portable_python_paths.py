"""Authored path fixtures for the source-built private packaging interpreter."""
from pathlib import Path
import importlib.util
import json
import os
import stat
import subprocess
import sys
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


if __name__ == '__main__':
    unittest.main()
