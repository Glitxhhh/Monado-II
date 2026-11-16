"""Actual CMake/Zig link regression on authored source; no game or host changes."""
import hashlib
import importlib.util
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest


SCRIPTS = Path(__file__).resolve().parents[1] / 'Monado_Linux/scripts'
sys.path.insert(0, str(SCRIPTS))
try:
    spec = importlib.util.spec_from_file_location('_cmake_linker_policy', SCRIPTS / 'build_portable_dependencies.py')
    dependencies = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(dependencies)
finally:
    sys.path.pop(0)


@unittest.skipUnless(sys.platform == 'linux', 'Linux Zig/CMake linker capability fixture')
class PortableCMakeLinker(unittest.TestCase):
    def test_host_linker_capability_does_not_leak_into_zig_link_command(self):
        cmake = os.environ.get('MONADO_TEST_CMAKE') or shutil.which('cmake')
        ninja = os.environ.get('MONADO_TEST_NINJA') or shutil.which('ninja')
        python = os.environ.get('MONADO_TEST_ZIG_PYTHON') or sys.executable
        linker = shutil.which('ld')
        if not cmake or not ninja or not linker:
            self.skipTest('CMake, Ninja and native GNU linker required')
        env = {'PATH': '/usr/bin:/bin', 'LANG': 'C', 'LC_ALL': 'C'}
        def run(command, environment=env):
            return subprocess.run(list(map(str, command)), env=environment,
                capture_output=True, text=True, timeout=120)
        version = run([python, '-I', '-B', '-m', 'ziglang', 'version'])
        if version.returncode or version.stdout.strip() != '0.14.1':
            self.skipTest('Pinned Zig0.14.1 Python package required')
        version = run([cmake, '--version'])
        match = re.search(r'(\d+)\.(\d+)', version.stdout)
        if not match or tuple(map(int, match.groups())) < (3, 31):
            self.skipTest('CMake3.31+ linker capability inspection required')
        if '--dependency-file' not in run([linker, '--help']).stdout:
            self.skipTest('Native linker does not advertise dependency files')
        with tempfile.TemporaryDirectory(prefix='monado-linker-capability-') as temporary:
            directory = Path(temporary)
            source = directory / 'source'; source.mkdir()
            (source / 'CMakeLists.txt').write_text('cmake_minimum_required(VERSION 3.27)\nproject(authored_linker C)\nadd_library(authored SHARED authored.c)\n')
            header = source / 'authored.h'; header.write_text('#define AUTHORED_VALUE 7\n')
            (source / 'authored.c').write_text('#include "authored.h"\nint authored_value(void) { return AUTHORED_VALUE; }\n')
            native = directory / 'native'; native.mkdir()
            # Zig reports ld.lld to compiler introspection. Alias the actual
            # GNU tool privately to model a host advertising this capability;
            # CMake probes it, while Zig still links with its embedded driver.
            (native / 'ld.lld').symlink_to(linker)
            environment = dict(env, PATH=str(native) + ':/usr/bin:/bin')
            compiler = directory / 'cc'
            compiler.write_text('#!/bin/sh\nexport ZIG_GLOBAL_CACHE_DIR=' + shlex.quote(str(directory / 'zig-global'))
                + '\nexport ZIG_LOCAL_CACHE_DIR=' + shlex.quote(str(directory / 'zig-local'))
                + '\nexec ' + shlex.quote(python) + ' -I -B -m ziglang cc -target x86_64-linux-gnu "$@"\n')
            compiler.chmod(0o700)
            policy = dependencies.target_cmake_options()
            original = [option for option in policy if not option.startswith('-DCMAKE_LINK_DEPENDS_USE_LINKER=')]
            for name, options in [('old-policy', original), ('portable-policy', policy)]:
                with self.subTest(name=name):
                    build = directory / name
                    configured = run([cmake, '-S', source, '-B', build, '-G', 'Ninja',
                        '-DCMAKE_MAKE_PROGRAM=' + ninja, '-DCMAKE_C_COMPILER=' + str(compiler),
                        '-DCMAKE_C_FLAGS=-target x86_64-linux-gnu.2.39 -fPIC', *options], environment)
                    self.assertEqual(configured.returncode, 0, configured.stdout + configured.stderr)
                    result = run([cmake, '--build', build, '--verbose'], environment)
                    rules = (build / 'build.ninja').read_text() + (build / 'CMakeFiles/rules.ninja').read_text()
                    if name == 'old-policy':
                        self.assertIn('--dependency-file=', rules)
                        self.assertNotEqual(result.returncode, 0)
                        self.assertIn('unsupported linker arg: --dependency-file=', result.stdout + result.stderr)
                    else:
                        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                        self.assertNotIn('--dependency-file=', rules)
                        library = build / 'libauthored.so'
                        first = hashlib.sha256(library.read_bytes()).hexdigest()
                        header.write_text('#define AUTHORED_VALUE 8\n')
                        rebuilt = run([cmake, '--build', build, '--verbose'], environment)
                        self.assertEqual(rebuilt.returncode, 0, rebuilt.stdout + rebuilt.stderr)
                        self.assertNotEqual(hashlib.sha256(library.read_bytes()).hexdigest(), first)


if __name__ == '__main__':
    unittest.main()
