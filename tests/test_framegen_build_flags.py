"""LINUX-101: run the actual limiter with its native per-file feature contract."""
from pathlib import Path
import importlib.util
import os
import re
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import types
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
PORT = ROOT / 'Monado_Linux'
SPEC = importlib.util.spec_from_file_location('monado_framegen_flag_build', PORT / 'scripts/build_linux.py')
build = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(build)


@unittest.skipUnless(sys.platform == 'linux', 'native Linux limiter and authored memory mapping')
class FramegenBuildFlags(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = os.environ.get('MONADO_TEST_CC')
        if not compiler:
            private = PORT / '.deps/bin/clang-cc'
            compiler = str(private) if private.is_file() else shutil.which('clang')
        if not compiler:
            raise unittest.SkipTest('native Clang compiler required')
        cls.directory = tempfile.TemporaryDirectory(prefix='monado-framegen-flags-')
        cls.addClassCleanup(cls.directory.cleanup)
        cls.output = Path(cls.directory.name)
        # The real build recipe uses Zig's x86_64_v2 CPU spelling. Keep that
        # production argv intact in receipts while allowing source CI's stock
        # Clang to compile the same x86-64-v2 target via a fixture-only adapter.
        compiler = str(Path(shutil.which(compiler) or compiler).resolve())
        version = subprocess.run([compiler, '--version'], capture_output=True, text=True, timeout=60)
        if version.returncode:
            raise AssertionError(version.stdout + version.stderr)
        cpu = '-march=x86_64_v2' if 'ziglang' in version.stdout else '-march=x86-64-v2'
        adapter = cls.output / 'compiler.py'
        adapter.write_text('import os,sys\n'
            'args=[' + repr(cpu) + ' if arg=="-march=x86_64_v2" else arg for arg in sys.argv[1:]]\n'
            'os.execv(' + repr(compiler) + ',[' + repr(compiler) + ',*args])\n')
        driver = cls.output / 'clang-cc'
        driver.write_text('#!/bin/sh\nexec ' + shlex.quote(sys.executable) + ' ' +
                          shlex.quote(str(adapter)) + ' "$@"\n')
        driver.chmod(0o755)
        compiler = str(driver)
        cls.source = PORT / 'runtime/hle/framepace.c'
        cls.fixture = PORT / 'tests/native/test_framepace_linux.c'
        cls.binaries, cls.objects, cls.commands = {}, {}, {}
        cls.compiler = compiler
        for enabled in (False, True):
            directory = cls.output / ('framegen' if enabled else 'headless')
            directory.mkdir()
            for part in ('tmp', 'obj', 'logs'): (directory / part).mkdir()
            builder = build.Builder.__new__(build.Builder)
            builder.output = directory
            builder.args = types.SimpleNamespace(keep_going=False)
            builder.lock, builder.failed = threading.Lock(), threading.Event()
            builder.env = {'PATH': '/usr/bin:/bin', 'LANG': 'C.UTF-8', 'LC_ALL': 'C.UTF-8',
                           'TMPDIR': str(directory / 'tmp')}
            builder.cc = builder.cxx = compiler
            builder.receipt = {'commands': [], 'objects': {}, 'errors': [], 'dependencies': {}}
            builder.flags = ['-O1', '-UNDEBUG', '-march=x86_64_v2', '-ffp-contract=off',
                             '-fno-strict-aliasing', '-ffunction-sections', '-fdata-sections']
            if os.environ.get('MONADO_TEST_UBSAN') == '1':
                builder.flags += ['-fsanitize=undefined', '-fno-sanitize-recover=all']
            if enabled:
                # Exercise the real shared-feature setup and real per-TU compile
                # recipe. Only offline dependency verification/copying is replaced
                # by authored never-loaded resource metadata; no SDK is needed.
                provider = types.SimpleNamespace(verify_existing=lambda: {
                    'library_path': '/authored/never-loaded-provider', 'library_sha256': 'a' * 64,
                    'license_path': '/authored/license', 'license_sha256': 'b' * 64})
                fake_spec = types.SimpleNamespace(loader=types.SimpleNamespace(exec_module=lambda module: None))
                builder.copy_runtime_resource = mock.Mock()
                with mock.patch.object(build.importlib.util, 'spec_from_file_location', return_value=fake_spec), \
                     mock.patch.object(build.importlib.util, 'module_from_spec', return_value=provider):
                    builder.setup_framegen()
            source_object = builder.compile(cls.source)
            fixture_object = builder.compile(cls.fixture)
            if source_object is None or fixture_object is None:
                messages = '\n'.join(p.read_text() for p in (directory / 'logs').glob('*.stderr.log'))
                raise AssertionError(messages)
            binary = directory / 'limiter'
            command = [compiler, *builder.flags, fixture_object, '-Wl,--gc-sections', '-lm', '-o', binary]
            result = subprocess.run(list(map(str, command)), capture_output=True, text=True, timeout=120)
            if result.returncode: raise AssertionError(result.stdout + result.stderr)
            cls.binaries[enabled] = binary
            cls.objects[enabled] = source_object
            cls.commands[enabled] = builder.receipt['commands']
            cls.production_flags = builder.flags
        # Preserve the exact wrong entry-only guard as a negative control. Local
        # includes retain their original absolute source origin in this copy.
        old = cls.source.read_text().replace('defined(XCDE_LINUX_FRAMEGEN)', 'defined(XCDE_GFX)')
        old = re.sub(r'(^\s*#\s*include\s+")([^"]+)(")',
                     lambda m: m[1] + str((cls.source.parent / m[2]).resolve()) + m[3], old, flags=re.M)
        legacy_source = cls.output / 'legacy-framepace.c'
        legacy_source.write_text(old)
        legacy_fixture = cls.output / 'legacy-fixture.c'
        legacy_fixture.write_text(cls.fixture.read_text().replace('../../runtime/hle/framepace.c', str(legacy_source)))
        cls.legacy = cls.output / 'legacy'
        command = [compiler, *cls.production_flags, legacy_fixture, '-Wl,--gc-sections', '-lm', '-o', cls.legacy]
        result = subprocess.run(list(map(str, command)), capture_output=True, text=True, timeout=120)
        if result.returncode: raise AssertionError(result.stdout + result.stderr)

    def scenario(self, scenario, fps=120, overrides=None):
        env = {'PATH': '/usr/bin:/bin', 'LANG': 'C.UTF-8', 'LC_ALL': 'C.UTF-8', **(overrides or {})}
        result = subprocess.run([str(self.binaries[True]), scenario, str(fps)], env=env,
                                capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('PASS framepace', result.stdout)

    def test_real_framepace_compile_uses_shared_feature_not_entry_only_gfx(self):
        command = self.commands[True][0]['argv']
        self.assertIn('-march=x86_64_v2', command)
        self.assertIn('-DXCDE_LINUX_FRAMEGEN=1', command)
        self.assertNotIn('-DXCDE_GFX', command)
        symbols = subprocess.check_output(['nm', '-u', str(self.objects[True])], text=True)
        self.assertIn('vk_framegen_cap_fps', symbols)

    def test_headless_object_has_no_renderer_dependency_and_retains_native_limiter(self):
        symbols = subprocess.check_output(['nm', '-u', str(self.objects[False])], text=True)
        self.assertNotIn('vk_framegen_cap_fps', symbols)
        result = subprocess.run([str(self.binaries[False]), 'headless-cap', '120'],
                                env={'PATH': '/usr/bin:/bin', 'LANG': 'C.UTF-8', 'LC_ALL': 'C.UTF-8'},
                                capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_fractional_cap_restores_off_failure_smooth_match_and_cutscene_policies(self):
        for fps in (30, 60, 120, 165, 0):
            with self.subTest(fps=fps): self.scenario('refresh-cap', fps)

    def test_deterministic_and_explicit_timing_overrides_keep_priority(self):
        for env in ({'XCDE_DETERMINISTIC': '1'}, {'XCDE_FPS_PERIOD_SCALE': '.25'},
                    {'XCDE_FPS_INTERVAL': '3', 'XCDE_FPS_PERIOD_SCALE': '.5'}):
            with self.subTest(env=env): self.scenario('refresh-bypass', overrides=env)

    def test_old_entry_only_guard_fails_the_same_fractional_cap_oracle(self):
        def no_core():
            import resource
            resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
        result = subprocess.run([str(self.legacy), 'refresh-cap', '120'], capture_output=True,
                                text=True, timeout=15, preexec_fn=no_core,
                                env={'PATH': '/usr/bin:/bin', 'LANG': 'C.UTF-8', 'LC_ALL': 'C.UTF-8'})
        self.assertEqual(result.returncode, -signal.SIGABRT, result.stdout + result.stderr)
        self.assertIn('FAIL', result.stderr)


if __name__ == '__main__': unittest.main()
