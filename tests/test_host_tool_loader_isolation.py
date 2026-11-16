"""Actual native ELF inspection under a deliberately incompatible library path."""
from pathlib import Path
import importlib.util
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('host_isolated_runtime', ROOT / 'tools/portable_runtime.py')
runtime = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runtime)
HOST_ENV = {'PATH': '/usr/bin:/bin', 'LANG': 'C', 'LC_ALL': 'C'}


@unittest.skipUnless(sys.platform == 'linux' and Path('/usr/bin/readelf').is_file()
                     and Path('/usr/bin/nm').is_file(), 'Linux ELF inspection tools required')
class HostToolLoaderIsolation(unittest.TestCase):
    def setUp(self):
        compiler = os.environ.get('MONADO_TEST_CC') or shutil.which('clang') or shutil.which('cc')
        if not compiler:
            self.skipTest('C compiler required for authored ELF fixture')
        self.compiler = compiler
        self.temp = tempfile.TemporaryDirectory(prefix='monado-host-inspection-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.poison = self.root / 'private-python-libraries'
        self.poison.mkdir()
        # The interpreter is already running; this affects only newly started
        # native processes if they incorrectly inherit the Python library path.
        (self.poison / 'libc.so.6').write_bytes(b'authored incompatible shared-library sentinel\n')

    def compile_runtime(self, extra=''):
        source = self.root / 'authored.c'
        source.write_text(extra + '\nint main(void) { return 0; }\n')
        binary = self.root / 'authored-runtime'
        result = subprocess.run([self.compiler, str(source), '-o', str(binary)],
            env=HOST_ENV, capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stderr)
        receipt = {'status': 'passed', 'split_game': True,
            'gamecode_abi': {'id': 'a' * 64},
            'executable': {'sha256': runtime.digest(binary)},
            'sources_before': {'authored.c': runtime.digest(source)},
            'sources_after': {'authored.c': runtime.digest(source)},
            'runtime_link_objects': ['authored.o']}
        return binary, receipt

    def test_runtime_transport_inspection_survives_incompatible_parent_libraries(self):
        binary, receipt = self.compile_runtime()
        contaminated = {**HOST_ENV, 'LD_LIBRARY_PATH': str(self.poison)}
        negative = subprocess.run(['/usr/bin/readelf', '--version'], env=contaminated,
            capture_output=True, text=True, timeout=15)
        if negative.returncode == 0:
            self.skipTest('host readelf does not use the dynamic libc loader')
        self.assertIn('libc.so.6', negative.stderr)
        with mock.patch.dict(os.environ, contaminated, clear=True):
            self.assertEqual(runtime.inspect_runtime(binary, receipt), 'a' * 64)

    def test_inspection_failure_reports_native_tool_error(self):
        missing = self.root / 'missing-runtime'
        with mock.patch.dict(os.environ, {'LD_LIBRARY_PATH': str(self.poison)}, clear=True):
            with self.assertRaises(ValueError) as caught:
                runtime.inspect_with_host_tool(['/usr/bin/readelf', '-SW', str(missing)])
        message = str(caught.exception)
        self.assertIn('exit ', message)
        self.assertIn('stderr:', message)
        self.assertIn('missing-runtime', message)
        self.assertNotIn('private-python-libraries', message)

    def test_game_function_exclusion_stays_strict_in_contaminated_parent(self):
        synthetic_name = 'f_' + 'a' * 12
        binary, receipt = self.compile_runtime('int ' + synthetic_name + '(void) { return 7; }')
        with mock.patch.dict(os.environ, {'LD_LIBRARY_PATH': str(self.poison)}, clear=True):
            with self.assertRaisesRegex(ValueError, 'lifted game functions'):
                runtime.inspect_runtime(binary, receipt)


if __name__ == '__main__':
    unittest.main()
