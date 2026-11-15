"""LINUX-099: actual own-process CUDA metadata Landlock scope, no CUDA/GPU/game."""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
PORT = ROOT / 'Monado_Linux'


@unittest.skipUnless(sys.platform == 'linux', 'Linux native process metadata policy')
class CudaTaskMetadata(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        local = PORT / '.deps/bin/clang-cc'
        compiler = os.environ.get('MONADO_TEST_CC') or (str(local) if local.is_file() else shutil.which('clang'))
        if not compiler:
            raise unittest.SkipTest('Clang required for actual native state guard')
        cls.temporary = tempfile.TemporaryDirectory(prefix='monado-cuda-task-metadata-')
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.folder = Path(cls.temporary.name).resolve()
        cls.binary = cls.folder / 'probe'
        cls.environment = {'PATH': '/usr/bin:/bin', 'LANG': 'C.UTF-8', 'LC_ALL': 'C.UTF-8'}
        command = [compiler, '-std=gnu11', '-O1', '-UNDEBUG', '-Wall', '-Wextra', '-Werror',
                   '-I' + str(PORT / 'runtime/platform'), str(ROOT / 'tests/native/test_cuda_task_metadata.c'),
                   str(PORT / 'runtime/platform/linux_state_guard.c'), '-pthread', '-o', str(cls.binary)]
        if os.environ.get('MONADO_TEST_UBSAN') == '1':
            command[1:1] = ['-fsanitize=undefined', '-fno-sanitize-recover=all']
            cls.environment['UBSAN_OPTIONS'] = 'halt_on_error=1:print_stacktrace=1'
        result = subprocess.run(command, env=cls.environment, capture_output=True, text=True, timeout=120)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def scenario(self, flags, enabled):
        bwrap = shutil.which('bwrap')
        if not bwrap:
            self.skipTest('bwrap required for private procfs test')
        with tempfile.TemporaryDirectory(prefix='case-', dir=self.folder) as temporary:
            case = Path(temporary); state = case / 'state'; state.mkdir()
            outside = case / 'outside'; outside.mkdir()
            sentinel = outside / 'sentinel'; sentinel.write_bytes(b'owned-sentinel\n')
            command = [bwrap, '--unshare-user', '--unshare-net', '--die-with-parent', '--new-session',
                       '--ro-bind', '/', '/', '--dev', '/dev', '--proc', '/proc',
                       '--bind', str(case), str(case), '--', str(self.binary), str(state),
                       str(sentinel), 'enabled' if enabled else 'disabled']
            result = subprocess.run(command, env={**self.environment, **flags}, capture_output=True,
                                    text=True, timeout=20)
            if result.returncode and ('No permissions to create a new namespace' in result.stderr or
                                      'Creating new namespace failed' in result.stderr):
                if os.environ.get('MONADO_REQUIRE_CUDA_TASK_NAMESPACE') == '1':
                    self.fail(result.stdout + result.stderr)
                self.skipTest('host sandbox denies nested user namespaces')
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('PASS metadata enabled=' + str(int(enabled)), result.stdout)
            self.assertEqual(sentinel.read_bytes(), b'owned-sentinel\n')
            self.assertEqual(list(state.iterdir()), [])

    def test_explicit_smooth_and_internal_optin_allow_future_own_thread_only(self):
        self.scenario({'NVPRESENT_ENABLE_SMOOTH_MOTION': '1', 'MONADO_CUDA_TASK_METADATA': '1'}, True)

    def test_off_retains_write_denial(self):
        self.scenario({}, False)

    def test_smooth_flag_alone_does_not_widen_guard(self):
        self.scenario({'NVPRESENT_ENABLE_SMOOTH_MOTION': '1'}, False)

    def test_internal_flag_alone_does_not_widen_guard(self):
        self.scenario({'MONADO_CUDA_TASK_METADATA': '1'}, False)

    def test_invalid_driver_boolean_does_not_widen_guard(self):
        self.scenario({'NVPRESENT_ENABLE_SMOOTH_MOTION': 'true', 'MONADO_CUDA_TASK_METADATA': '1'}, False)

    def test_invalid_internal_boolean_does_not_widen_guard(self):
        self.scenario({'NVPRESENT_ENABLE_SMOOTH_MOTION': '1', 'MONADO_CUDA_TASK_METADATA': '1 '}, False)


if __name__ == '__main__':
    unittest.main()
