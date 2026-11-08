"""Real pkg-config path fixtures; no game files, downloads or game builds."""
import contextlib
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('monado_graphics_paths_setup', ROOT / 'Monado_Linux/scripts/setup_linux_graphics.py')
setup = importlib.util.module_from_spec(spec)
spec.loader.exec_module(setup)


@unittest.skipUnless(sys.platform == 'linux' and Path('/usr/bin/pkg-config').is_file(),
                     'Linux host pkg-config required for path fragment fixtures')
class GraphicsPaths(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='monado-pkg-paths-')
        self.addCleanup(temporary.cleanup)
        self.base = Path(temporary.name).resolve()
        # Optional test-only backend selection exercises another pkgconf version
        # without replacing the host tool or changing production wrapper policy.
        self.backend = os.environ.get('MONADO_TEST_PKG_CONFIG', '/usr/bin/pkg-config')
        self.environment = {'PATH': '/usr/bin:/bin', 'LANG': 'C.UTF-8', 'LC_ALL': 'C.UTF-8'}
        library_path = os.environ.get('MONADO_TEST_PKG_CONFIG_LIBRARY_PATH')
        if library_path:
            self.environment['LD_LIBRARY_PATH'] = library_path

    def fixture(self, name='My Games/Monado II'):
        port = self.base / name / 'Monado_Linux'
        port.mkdir(parents=True)
        lock = port / 'graphics-deps-linux.lock.json'
        raw = json.dumps({'schema': 1, 'packages': []}).encode()
        lock.write_bytes(raw)
        target = port / '.deps/graphics/sysroots' / hashlib.sha256(raw).hexdigest()[:16]
        # Exercise the actual setup entry point offline. Empty authored package
        # metadata needs no extraction or download; it generates the real wrapper.
        with mock.patch.multiple(setup, ROOT=port, LOCK=lock), \
                mock.patch.object(sys, 'argv', ['setup_linux_graphics.py', '--offline']), \
                mock.patch.object(setup.urllib.request, 'urlopen', side_effect=AssertionError('network')), \
                mock.patch.object(setup.subprocess, 'run', side_effect=AssertionError('unexpected extraction')), \
                mock.patch.object(setup.subprocess, 'Popen', side_effect=AssertionError('unexpected extraction')), \
                contextlib.redirect_stdout(io.StringIO()):
            setup.main()
        pc = target / 'usr/lib/x86_64-linux-gnu/pkgconfig'
        pc.mkdir(parents=True)
        (target / 'usr/share/pkgconfig').mkdir(parents=True)
        includedir = target / 'usr/include/monado_fixture'
        includedir.mkdir(parents=True)
        (includedir / 'authored_path_fixture.h').write_text('#define MONADO_PATH_FIXTURE 37\n')
        (target / 'usr/lib').mkdir(exist_ok=True)
        (pc / 'monado-path-fixture.pc').write_text(
            'prefix=/usr\nlibdir=${prefix}/lib\nincludedir=${prefix}/include\n'
            'Name: Monado authored path fixture\nDescription: No game data\nVersion: 1.0.0\n'
            'Cflags: -I${includedir}/monado_fixture -DMONADO_PC_DEFINE=1\n'
            'Libs: -L${libdir} -lm\n')
        wrapper = port / '.deps/bin/pkg-config-graphics'
        if self.backend != '/usr/bin/pkg-config':
            wrapper.write_text(wrapper.read_text().replace('exec /usr/bin/pkg-config "$@"',
                'exec ' + shlex.quote(self.backend) + ' "$@"'))
        return port, target, wrapper

    def run_query(self, wrapper, *flags, environment=None):
        result = subprocess.run([str(wrapper), *flags, 'monado-path-fixture'],
            cwd=wrapper.parents[2], env=dict(self.environment, **(environment or {})),
            capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout

    def assert_flags(self, target, wrapper):
        include = '-I' + str(target / 'usr/include/monado_fixture')
        library = '-L' + str(target / 'usr/lib')
        self.assertEqual(shlex.split(self.run_query(wrapper, '--cflags')), [include, '-DMONADO_PC_DEFINE=1'])
        self.assertEqual(shlex.split(self.run_query(wrapper, '--cflags-only-I')), [include])
        self.assertEqual(shlex.split(self.run_query(wrapper, '--cflags-only-other')), ['-DMONADO_PC_DEFINE=1'])
        self.assertEqual(shlex.split(self.run_query(wrapper, '--libs')), [library, '-lm'])
        self.assertEqual(shlex.split(self.run_query(wrapper, '--libs-only-L')), [library])
        self.assertEqual(shlex.split(self.run_query(wrapper, '--libs-only-l')), ['-lm'])
        self.assertEqual(set(shlex.split(self.run_query(wrapper, '--static', '--cflags', '--libs'))),
                         {include, '-DMONADO_PC_DEFINE=1', library, '-lm'})
        self.assertTrue(Path(include[2:]).is_dir())
        self.assertTrue(Path(library[2:]).is_dir())

    def test_generated_wrapper_preserves_spaced_include_and_library_paths(self):
        _, target, wrapper = self.fixture()
        self.assert_flags(target, wrapper)

    def test_variable_and_metadata_queries_retain_raw_sysroot_contract(self):
        _, target, wrapper = self.fixture()
        self.assertEqual(self.run_query(wrapper, '--variable=pc_sysrootdir').strip(), str(target))
        self.assertEqual(self.run_query(wrapper, '--variable', 'pc_sysrootdir').strip(), str(target))
        self.assertEqual(self.run_query(wrapper, '--variable=includedir').strip(), str(target / 'usr/include'))
        self.assertEqual(self.run_query(wrapper, '--variable=libdir').strip(), str(target / 'usr/lib'))
        self.assertEqual(self.run_query(wrapper, '--modversion').strip(), '1.0.0')
        self.assertEqual(self.run_query(wrapper, '--exists'), '')

    def test_metadata_query_does_not_inherit_flag_only_sysroot_rules(self):
        _, target, wrapper = self.fixture()
        inherited = {'PKG_CONFIG_FDO_SYSROOT_RULES': '1'}
        self.assertEqual(self.run_query(wrapper, '--variable=pc_sysrootdir', environment=inherited).strip(), str(target))
        self.assertEqual(self.run_query(wrapper, '--variable=includedir', environment=inherited).strip(), str(target / 'usr/include'))

    def test_raw_sysroot_reproduces_fragment_corruption_on_affected_pkgconf(self):
        _, target, _ = self.fixture()
        result = subprocess.run([self.backend, '--cflags', '--libs', 'monado-path-fixture'],
            env=dict(self.environment, PKG_CONFIG_SYSROOT_DIR=str(target),
                     PKG_CONFIG_LIBDIR=str(target / 'usr/lib/x86_64-linux-gnu/pkgconfig')),
            capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr)
        expected = {'-I' + str(target / 'usr/include/monado_fixture'), '-DMONADO_PC_DEFINE=1',
                    '-L' + str(target / 'usr/lib'), '-lm'}
        tokens = shlex.split(result.stdout)
        if set(tokens) == expected:
            self.skipTest('host pkg-config already handles unquoted spaced sysroots; positive wrapper tests still run')
        self.assertNotEqual(set(tokens), expected)
        self.assertTrue(any(not token.startswith(('-I', '-L', '-D', '-l')) or
                            (token.startswith(('-I', '-L')) and not Path(token[2:]).is_dir()) for token in tokens),
                        result.stdout)

    def test_pkgconf_quoting_and_shell_escaping_preserve_literal_metacharacters(self):
        _, target, wrapper = self.fixture('My Games/Monado\'s "quoted" $(touch MONADO_PATH_INJECTION) ; $value \\')
        self.assert_flags(target, wrapper)
        self.assertEqual(self.run_query(wrapper, '--variable=pc_sysrootdir').strip(), str(target))
        self.assertFalse((wrapper.parents[2] / 'MONADO_PATH_INJECTION').exists())

    @unittest.skipUnless(shutil.which('cc'), 'host C compiler required for tiny authored include-path check')
    def test_tiny_authored_component_compiles_using_the_actual_wrapper_flags(self):
        port, _, wrapper = self.fixture()
        source, output = port / 'authored path test.c', port / 'authored path test.o'
        source.write_text('#include <authored_path_fixture.h>\n'
                          '#if MONADO_PC_DEFINE != 1\n#error missing pkg-config define\n#endif\n'
                          'int monado_path_fixture(void) { return MONADO_PATH_FIXTURE; }\n')
        flags = shlex.split(self.run_query(wrapper, '--cflags'))
        result = subprocess.run([shutil.which('cc'), '-Werror', *flags, '-c', str(source), '-o', str(output)],
            cwd=port, env={'PATH': '/usr/bin:/bin', 'LANG': 'C.UTF-8', 'LC_ALL': 'C.UTF-8'},
            capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue(output.is_file())
        self.assertGreater(output.stat().st_size, 0)


if __name__ == '__main__':
    unittest.main()
