"""Authored portability regressions; no game data, packages or host changes."""
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import types
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
SCRIPTS = ROOT / 'Monado_Linux/scripts'
sys.path.insert(0, str(SCRIPTS))
try:
    def load(name):
        spec = importlib.util.spec_from_file_location('_environment_test_' + name, SCRIPTS / (name + '.py'))
        value = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(value)
        return value
    dependencies = load('build_portable_dependencies')
    python = load('setup_portable_python')
    graph = load('setup_portable_runtime_graph')
finally:
    sys.path.pop(0)


@unittest.skipUnless(sys.platform == 'linux', 'Linux portable build tool environment')
class PortableEnvironment(unittest.TestCase):
    def test_private_target_metadata_does_not_inherit_host_overrides(self):
        with mock.patch.dict(os.environ, {'PATH': '/authored-untrusted-bin',
                'LD_LIBRARY_PATH': '/authored-untrusted-libs', 'PKG_CONFIG_PATH': '/authored-host-pc'}):
            environment = dependencies.build_environment(Path('/authored-target'))
        self.assertEqual(environment['PATH'], '/usr/bin:/bin')
        self.assertNotIn('LD_LIBRARY_PATH', environment)
        self.assertNotIn('PKG_CONFIG_PATH', environment)
        self.assertEqual(environment['PKG_CONFIG_SYSROOT_DIR'], '/authored-target')
        self.assertEqual(environment['PKG_CONFIG_LIBDIR'],
                         '/authored-target/usr/lib/x86_64-linux-gnu/pkgconfig:/authored-target/usr/share/pkgconfig')

    @unittest.skipUnless(sys.platform == 'linux', 'POSIX host wrapper dispatch')
    def test_fedora_style_wrapper_uses_host_helper_not_target_executable(self):
        with tempfile.TemporaryDirectory(prefix='monado-pkg-config-fixture-') as temporary:
            directory = Path(temporary)
            host = directory / 'host-bin'; host.mkdir()
            target = directory / 'sysroot'; target_bin = target / 'usr/bin'; target_bin.mkdir(parents=True)
            wrapper = directory / 'pkg-config'
            wrapper.write_text('#!/bin/sh\nexec pkgconf "$@"\n')
            (host / 'pkgconf').write_text('#!/bin/sh\nprintf "authored host helper\\n"\n')
            (target_bin / 'pkgconf').write_text('#!/bin/sh\nprintf "authored target loader mismatch\\n" >&2\nexit 127\n')
            for entry in (wrapper, host / 'pkgconf', target_bin / 'pkgconf'):
                entry.chmod(0o700)
            old = subprocess.run([wrapper, '--version'], capture_output=True, text=True,
                                 env={'PATH': str(target_bin) + ':' + str(host)})
            self.assertEqual(old.returncode, 127)
            self.assertIn('target loader mismatch', old.stderr)
            # Inject only a disposable host tool directory; exercise the real
            # production environment constructor and wrapper shell resolution.
            with mock.patch.object(dependencies, 'HOST_TOOL_PATH', str(host)):
                environment = dependencies.build_environment(target)
            result = subprocess.run([wrapper, '--version'], capture_output=True, text=True, env=environment)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout.strip(), 'authored host helper')

    def test_python_uses_the_same_environment_constructor(self):
        # The Python recipe imports this exact policy, not a second target-PATH
        # recipe which could reproduce the wrapper failure at packaging time.
        self.assertIs(python.build_environment,
                      sys.modules['build_portable_dependencies'].build_environment)
        environment = python.build_environment('/authored-python-target')
        self.assertEqual(environment['PATH'], '/usr/bin:/bin')


@unittest.skipUnless(sys.platform == 'linux', 'isolated Linux compiler/sysroot fixture')
class CMakeHeaderDiscovery(unittest.TestCase):
    def test_host_header_enables_broken_backend_until_target_search_is_isolated(self):
        cmake = os.environ.get('MONADO_TEST_CMAKE') or shutil.which('cmake')
        compiler = os.environ.get('MONADO_TEST_CC') or shutil.which('cc')
        make = os.environ.get('MONADO_TEST_MAKE') or shutil.which('make')
        if not cmake or not compiler or not make:
            self.skipTest('CMake and native C/make tools required for authored isolated-header fixture')
        with tempfile.TemporaryDirectory(prefix='monado-cmake-header-') as temporary:
            directory = Path(temporary)
            source = directory / 'source'; source.mkdir()
            target = directory / 'empty-sysroot'; target.mkdir()
            external = directory / 'external-host-prefix/include'; external.mkdir(parents=True)
            (external / 'liburing.h').write_text('/* authored external header, unavailable to target compiler */\n')
            (source / 'optional.c').write_text('#include <liburing.h>\nint optional_backend(void) { return 1; }\n')
            (source / 'fallback.c').write_text('int threaded_fallback(void) { return 1; }\n')
            (source / 'CMakeLists.txt').write_text('''cmake_minimum_required(VERSION 3.16)
project(authored_header_discovery C)
option(SDL_LIBURING "Authored optional backend" ON)
if(SDL_LIBURING)
  find_path(HAVE_LIBURING_H NAMES liburing.h)
endif()
if(SDL_LIBURING AND HAVE_LIBURING_H)
  add_library(backend STATIC optional.c)
else()
  add_library(backend STATIC fallback.c)
endif()
''')
            cases = [
                ('old-default', [], True),
                ('root-only', [x for x in dependencies.target_cmake_options() if 'SDL_LIBURING' not in x], False),
                ('portable-policy', dependencies.target_cmake_options(), False),
            ]
            for name, policy, failure in cases:
                with self.subTest(name=name):
                    output = directory / name
                    configure = subprocess.run([cmake, '-S', str(source), '-B', str(output), '-G', 'Unix Makefiles',
                        '-DCMAKE_MAKE_PROGRAM=' + make, '-DCMAKE_C_COMPILER=' + compiler,
                        '-DCMAKE_SYSROOT=' + str(target), '-DCMAKE_PREFIX_PATH=' + str(external.parent),
                        '-DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY', *policy],
                        capture_output=True, text=True, timeout=30)
                    self.assertEqual(configure.returncode, 0, configure.stdout + configure.stderr)
                    result = subprocess.run([cmake, '--build', str(output), '--parallel', '1'],
                                            capture_output=True, text=True, timeout=30)
                    cache = (output / 'CMakeCache.txt').read_text()
                    if failure:
                        self.assertIn('HAVE_LIBURING_H:PATH=' + str(external), cache)
                        self.assertNotEqual(result.returncode, 0)
                        self.assertIn('liburing.h', result.stdout + result.stderr)
                    else:
                        self.assertNotIn('HAVE_LIBURING_H:PATH=' + str(external), cache)
                        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


class PortableDiagnostics(unittest.TestCase):
    def test_failure_keeps_bounded_early_loader_and_final_compiler_diagnostics(self):
        with tempfile.TemporaryDirectory() as temporary:
            log = Path(temporary) / 'nested.log'
            log.write_bytes(b'pkgconf: authored loader failure\n' + b'x' * 100000 + b'\nfatal: liburing.h missing\n')
            error = dependencies.phase_failure('sdl-build', log, 7, display_path='/retained/logs/sdl-build.log')
            message = str(error)
            self.assertIn('exit 7', message)
            self.assertIn('/retained/logs/sdl-build.log', message)
            self.assertIn('authored loader failure', message)
            self.assertIn('liburing.h missing', message)
            self.assertIn('output omitted', message)
            self.assertLess(len(message.encode()), 16800)

    def test_progress_heartbeat_preserves_the_original_timeout(self):
        clock = [0.0]
        process = mock.Mock(args=['authored-build'])
        waits = []
        def wait(timeout):
            waits.append(timeout); clock[0] += timeout
            raise subprocess.TimeoutExpired(process.args, timeout)
        process.wait.side_effect = wait
        stdout = io.StringIO()
        with mock.patch.object(dependencies.time, 'monotonic', side_effect=lambda: clock[0]), \
                contextlib.redirect_stdout(stdout):
            with self.assertRaises(subprocess.TimeoutExpired) as failure:
                dependencies.wait_for_build(process, 'sdl-build', timeout=20, interval=15)
        self.assertEqual(waits, [15, 5])
        self.assertEqual(failure.exception.timeout, 20)
        self.assertIn('sdl-build: still working (15s)', stdout.getvalue())

    def test_completed_process_is_not_reported_as_timeout_after_a_heartbeat(self):
        clock = [0.0]
        process = mock.Mock(args=['authored-build'])
        def wait(timeout):
            clock[0] += timeout
            if clock[0] < 30:
                raise subprocess.TimeoutExpired(process.args, timeout)
            return 0
        process.wait.side_effect = wait
        with mock.patch.object(dependencies.time, 'monotonic', side_effect=lambda: clock[0]), \
                contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(dependencies.wait_for_build(process, 'compile', timeout=60), 0)

    def test_cold_marker_miss_is_not_exception_context_for_real_build_failure(self):
        portable = types.SimpleNamespace(
            verify_existing=mock.Mock(side_effect=FileNotFoundError('active-build.json')),
            baseline=types.SimpleNamespace(prepare=mock.Mock()),
            build=mock.Mock(side_effect=RuntimeError('authored compiler failure')))
        with mock.patch.dict(sys.modules, {'build_portable_dependencies': portable}), \
                contextlib.redirect_stdout(io.StringIO()) as stdout:
            with self.assertRaisesRegex(RuntimeError, 'authored compiler failure') as caught:
                graph.build_graph('/unused-base')
        self.assertIsNone(caught.exception.__context__)
        self.assertIn('no current verified provider', stdout.getvalue())
        portable.baseline.prepare.assert_called_once_with()


class PortablePythonLicensesAndTrust(unittest.TestCase):
    def license_document(self, *, sections=range(1, 10), boundary='expat'):
        return ('Authored documentation fixture\nOpenSSL\n-------\nintro::\n\n'
                '   Apache License\n   Version 2.0, January 2004\n'
                + ''.join('   ' + str(i) + '. Authored section sentinel\n' for i in sections)
                + '   END OF TERMS AND CONDITIONS\n\n   APPENDIX: Authored retained appendix\n\n'
                + boundary + '\n-----\nnext unrelated notice\n')

    def test_pinned_source_license_keeps_all_terms_and_appendix_without_host_file(self):
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary); (source / 'Doc').mkdir()
            (source / 'Doc/license.rst').write_text(self.license_document())
            result = python.apache_license_text(source)
        self.assertTrue(result.startswith('Apache License\n'))
        self.assertIn('9. Authored section sentinel', result)
        self.assertIn('APPENDIX: Authored retained appendix', result)
        self.assertNotIn('unrelated notice', result)

    def test_missing_license_terms_or_changed_section_boundary_fail_closed(self):
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary); (source / 'Doc').mkdir()
            for text in [self.license_document(sections=range(1, 9)),
                         self.license_document(boundary='unexpected')]:
                with self.subTest(text=text):
                    (source / 'Doc/license.rst').write_text(text)
                    with self.assertRaisesRegex(ValueError, 'license'):
                        python.apache_license_text(source)

    @unittest.skipUnless(sys.platform == 'linux', 'POSIX native certificate-store symlink')
    def test_native_ca_selection_accepts_fedora_style_symlink_and_ignores_directories(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary); (root / 'not-a-file').mkdir()
            bundle = root / 'native-bundle.pem'; bundle.write_text('authored certificate path fixture, never parsed')
            alias = root / 'ca-certificates.crt'; alias.symlink_to(bundle)
            with mock.patch.object(python, 'SYSTEM_CA_FILES', (str(root / 'not-a-file'), str(alias))):
                self.assertEqual(python.system_ca_file(), str(alias))

    def test_missing_native_trust_does_not_silently_disable_tls_verification(self):
        with mock.patch.object(python, 'SYSTEM_CA_FILES', ('/authored-missing-ca-file',)):
            with self.assertRaisesRegex(ValueError, 'ca-certificates'):
                python.system_ca_file()

    def test_unavailable_first_standard_store_does_not_hide_a_readable_alternative(self):
        with mock.patch.object(python, 'SYSTEM_CA_FILES', ('/authored-store-one', '/authored-store-two')), \
                mock.patch.object(Path, 'is_file', side_effect=[PermissionError('authored inaccessible store'), True]), \
                mock.patch.object(python.os, 'access', return_value=True):
            self.assertEqual(python.system_ca_file(), '/authored-store-two')

    def test_environment_recipe_change_invalidates_python_provider(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            output = root / '.deps/portable/python/authored'; (output / 'prefix/bin').mkdir(parents=True)
            executable = output / 'prefix/bin/python3.14'; executable.write_bytes(b'authored identity fixture, not executed')
            environment_recipe = root / 'environment-recipe.py'; environment_recipe.write_text('# authored environment recipe\n')
            lock = root / 'python-lock'; lock.write_bytes(b'authored python lock')
            baseline_lock = root / 'baseline-lock'; baseline_lock.write_bytes(b'authored baseline lock')
            sha = python.baseline.sha
            compilers = {}
            for name in python.COMPILERS:
                path = root / name; path.parent.mkdir(parents=True, exist_ok=True); path.write_bytes(b'authored compiler identity')
                compilers[name] = sha(path)
            receipt = {'status': 'passed', 'lock_sha256': sha(lock), 'recipe_sha256': sha(python.__file__),
                'baseline_lock_sha256': sha(baseline_lock), 'environment_recipe_sha256': sha(environment_recipe),
                'compiler': compilers, 'outputs': {'bin/python3.14': {'bytes': executable.stat().st_size,
                                                                  'sha256': sha(executable)}}}
            receipt_path = output / 'receipt.json'; receipt_path.write_text(json.dumps(receipt))
            marker = root / '.deps/portable/active-python.json'
            marker.write_text(json.dumps({'receipt': str(receipt_path), 'sha256': sha(receipt_path)}))
            with mock.patch.object(python, 'ROOT', root), mock.patch.object(python, 'LOCK', lock), \
                    mock.patch.object(python.baseline, 'LOCK', baseline_lock), \
                    mock.patch.object(python, 'ENVIRONMENT_RECIPE', environment_recipe):
                self.assertEqual(python.verify_existing()[0], receipt_path)
                environment_recipe.write_text('# authored changed environment recipe\n')
                with self.assertRaisesRegex(ValueError, 'recipe or baseline differs'):
                    python.verify_existing()


@unittest.skipUnless(sys.platform == 'linux', 'Linux build-time ELF dependency resolution')
class PortablePythonBuildLibraries(unittest.TestCase):
    def test_target_soname_is_available_to_build_time_import_without_target_glibc(self):
        compiler = shutil.which('cc')
        if not compiler:
            self.skipTest('native C compiler required for authored ELF-loader fixture')
        with tempfile.TemporaryDirectory(prefix='monado-python-libs-') as temporary:
            directory = Path(temporary)
            libdir = directory / 'target-libraries'; libdir.mkdir()
            library = libdir / 'libbz2.so.1.0'
            source = directory / 'library.c'
            source.write_text('int monado_authored_bzip_fixture(void) { return 42; }\n')
            result = subprocess.run([compiler, '-shared', '-fPIC', '-nostdlib',
                '-Wl,-soname,libbz2.so.1.0', str(source), '-o', str(library)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            probe = directory / 'probe'
            source.write_text('extern int monado_authored_bzip_fixture(void);\n'
                              'int main(void) { return monado_authored_bzip_fixture() != 42; }\n')
            result = subprocess.run([compiler, str(source), '-L' + str(libdir),
                '-Wl,--no-as-needed', '-l:libbz2.so.1.0', '-o', str(probe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            # On Fedora the SONAME is absent; Ubuntu has it but not this
            # authored symbol. Both establish that host-only lookup is wrong.
            old = subprocess.run([probe], env={'PATH': '/usr/bin:/bin'}, capture_output=True, text=True)
            self.assertNotEqual(old.returncode, 0)
            private = directory / 'build-libraries'
            with mock.patch.object(python, 'LIBS', {'libbz2.so.1.0': 'authored fixture'}):
                rows = python.prepare_build_libraries(private, libdir)
                result = subprocess.run([probe], env={'PATH': '/usr/bin:/bin',
                    'LD_LIBRARY_PATH': str(private)}, capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                python.verify_build_libraries(private, rows)
            self.assertEqual({p.name for p in private.iterdir()}, {'libbz2.so.1.0'})
            self.assertEqual(rows['libbz2.so.1.0']['glibc_max'], '0')
            self.assertEqual((private / 'libbz2.so.1.0').read_bytes(), library.read_bytes())

    def test_glibc_or_loader_cannot_enter_the_build_library_directory(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            for name in ['libc.so.6', 'ld-linux-x86-64.so.2']:
                with self.subTest(name=name), mock.patch.object(python, 'LIBS', {name: 'authored rejected provider'}):
                    target = directory / name
                    with self.assertRaisesRegex(ValueError, 'exclude glibc and loaders'):
                        python.prepare_build_libraries(target, directory)
                    self.assertFalse(target.exists())

    def test_changed_unknown_or_linked_private_library_fails_identity_verification(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary); libdir = directory / 'sysroot'; libdir.mkdir()
            name = 'libbz2.so.1.0'; (libdir / name).write_bytes(b'authored inert identity fixture')
            with mock.patch.object(python, 'LIBS', {name: 'authored fixture'}), \
                    mock.patch.object(python, 'elf_floor', return_value='2.39'):
                private = directory / 'private'; rows = python.prepare_build_libraries(private, libdir)
                (private / name).write_bytes(b'authored changed identity fixture')
                with self.assertRaisesRegex(ValueError, 'library changed'):
                    python.verify_build_libraries(private, rows)
                (private / name).unlink(); (private / name).symlink_to(libdir / name)
                with self.assertRaisesRegex(ValueError, 'library changed'):
                    python.verify_build_libraries(private, rows)
                (private / name).unlink(); (private / name).write_bytes((libdir / name).read_bytes())
                (private / 'libc.so.6').write_bytes(b'authored extra provider')
                with self.assertRaisesRegex(ValueError, 'inventory changed'):
                    python.verify_build_libraries(private, rows)

    def test_target_library_link_cannot_escape_the_private_sysroot(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary); libdir = directory / 'sysroot'; libdir.mkdir()
            escaped = directory / 'outside'; escaped.write_bytes(b'authored outside library')
            (libdir / 'libbz2.so.1.0').symlink_to(escaped)
            with mock.patch.object(python, 'LIBS', {'libbz2.so.1.0': 'authored fixture'}):
                with self.assertRaisesRegex(ValueError, 'escaped its pinned sysroot'):
                    python.prepare_build_libraries(directory / 'private', libdir)


if __name__ == '__main__':
    unittest.main()
