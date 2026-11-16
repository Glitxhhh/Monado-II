"""Authored host-tool regressions; no game data or system modifications."""
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
SCRIPTS = ROOT / 'Monado_Linux/scripts'
sys.path.insert(0, str(SCRIPTS))
try:
    spec = importlib.util.spec_from_file_location(
        '_portable_elf_inspection_test', SCRIPTS / 'build_portable_dependencies.py')
    dependencies = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(dependencies)
finally:
    sys.path.pop(0)


@unittest.skipUnless(sys.platform == 'linux', 'Linux native ELF inspection')
class PortableElfInspection(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='monado ELF host tool ')
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.library = self.directory / 'authored SDL fixture.so.1'

    def tool(self, body):
        tool = self.directory / 'authored readelf'
        tool.write_text('#!/bin/sh\n' + body)
        tool.chmod(0o700)
        return tool

    def test_host_tool_rejects_private_python_loader_environment_until_isolated(self):
        tool = self.tool('''if [ -n "${LD_LIBRARY_PATH-}" ]; then
  printf "authored libssl.so.3: OPENSSL_3.5.0 not found\\n" >&2
  exit 1
fi
if [ -n "${LD_PRELOAD-}${DEBUGINFOD_URLS-}${PYTHONHOME-}" ]; then
  printf "ambient tool configuration escaped\\n" >&2
  exit 2
fi
printf 'Name: GLIBC_2.39\\n'
''')
        poisoned = {'LD_LIBRARY_PATH': '/authored/private-python/lib',
                    'LD_PRELOAD': '', 'DEBUGINFOD_URLS': 'https://authored.invalid',
                    'PYTHONHOME': '/authored/private-python'}
        old = subprocess.run([tool, '-VW', self.library], env=poisoned,
                             capture_output=True, text=True, timeout=5)
        self.assertEqual(old.returncode, 1)
        self.assertIn('OPENSSL_3.5.0', old.stderr)
        with mock.patch.dict(os.environ, poisoned), mock.patch.object(dependencies, 'READELF', str(tool)):
            self.assertEqual(dependencies.elf_floor(self.library), '2.39')
        self.assertIn('private-python/lib', poisoned['LD_LIBRARY_PATH'])

    def test_failure_retains_exit_stdout_stderr_and_exact_spaced_library_path(self):
        tool = self.tool("printf 'authored stdout diagnostic\\n'\n"
                         "printf 'libssl.so.3: OPENSSL_3.5.0 not found\\n' >&2\nexit 7\n")
        with mock.patch.object(dependencies, 'READELF', str(tool)):
            with self.assertRaises(ValueError) as raised:
                dependencies.elf_floor(self.library)
        error = str(raised.exception)
        for expected in ('exit 7', str(self.library), 'authored stdout diagnostic',
                         'STDOUT:', 'STDERR:', 'OPENSSL_3.5.0'):
            self.assertIn(expected, error)

    def test_actual_readelf_malformed_elf_remains_a_failure(self):
        if not Path(dependencies.READELF).is_file():
            self.skipTest('System readelf unavailable')
        self.library.write_bytes(b'authored invalid ELF\n')
        with self.assertRaises(ValueError) as raised:
            dependencies.elf_floor(self.library)
        error = str(raised.exception)
        self.assertIn('readelf failed (exit ', error)
        self.assertIn('STDERR:', error)
        self.assertIn(str(self.library), error)

    def test_timeout_is_finite_and_preserves_early_error_output(self):
        tool = self.tool("printf 'inspection started\\n' >&2\nexec /bin/sleep 5\n")
        with mock.patch.object(dependencies, 'READELF', str(tool)), \
                mock.patch.object(dependencies, 'ELF_INSPECTION_TIMEOUT', .05):
            with self.assertRaises(ValueError) as raised:
                dependencies.elf_floor(self.library)
        self.assertIsInstance(raised.exception.__cause__, subprocess.TimeoutExpired)
        self.assertIn('inspection started', str(raised.exception))
        self.assertIn('timed out', str(raised.exception))

    def test_oversized_successful_output_is_rejected_with_bounded_diagnostics(self):
        tool = self.tool("printf 'AUTHORED START\\n'\n"
                         "head -c 4096 /dev/zero | tr '\\000' x\n"
                         "printf '\\nAUTHORED END\\n'\n"
                         "printf 'stderr context\\n' >&2\n")
        with mock.patch.object(dependencies, 'READELF', str(tool)), \
                mock.patch.object(dependencies, 'ELF_INSPECTION_MAX_OUTPUT', 1024), \
                mock.patch.object(dependencies, 'ELF_INSPECTION_ERROR_LIMIT', 128):
            with self.assertRaises(ValueError) as raised:
                dependencies.elf_floor(self.library)
        error = str(raised.exception)
        self.assertIn('output exceeds 1024 bytes', error)
        self.assertIn('AUTHORED START', error)
        self.assertIn('AUTHORED END', error)
        self.assertIn('stderr context', error)
        self.assertIn('intermediate output omitted', error)
        self.assertLess(len(error), 1000)

    def test_actual_versioned_elf_keeps_glibc_239_boundary(self):
        compiler = shutil.which('cc')
        if not compiler or not Path(dependencies.READELF).is_file():
            self.skipTest('Native C compiler and readelf required for authored ELF')
        source = self.directory / 'authored.c'
        source.write_text('int authored_symbol(void) { return 1; }\n')
        for version, rejected in [('2.39', False), ('2.40', True)]:
            with self.subTest(version=version):
                script = self.directory / 'authored.map'
                script.write_text('GLIBC_' + version + ' { global: authored_symbol; local: *; };\n')
                result = subprocess.run([compiler, '-shared', '-fPIC', '-nostdlib', str(source),
                    '-Wl,--version-script=' + str(script), '-o', str(self.library)],
                    capture_output=True, text=True, timeout=30)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                if rejected:
                    with self.assertRaisesRegex(ValueError, 'exceeds GLIBC_2.39'):
                        dependencies.elf_floor(self.library)
                else:
                    self.assertEqual(dependencies.elf_floor(self.library), version)


if __name__ == '__main__':
    unittest.main()
