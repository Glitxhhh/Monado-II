"""LINUX-099: execute actual drain/reset failures and WSI acquisition policy."""
from pathlib import Path
import hashlib
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import unittest
if sys.platform == 'linux':
    import resource
else:
    resource = None
ROOT = Path(__file__).resolve().parents[1]
PORT = ROOT / 'Monado_Linux'


def function(source, signature):
    start = source.index(signature)
    brace = source.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'


@unittest.skipUnless(sys.platform == 'linux', 'native Linux pthread presentation policy')
class FramegenDrainPolicy(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = os.environ.get('MONADO_TEST_CC') or shutil.which('clang') or shutil.which('cc')
        if not compiler: raise unittest.SkipTest('native C compiler required')
        marker = PORT / '.deps/graphics/active-sysroot.txt'
        include = Path(marker.read_text().strip()) / 'usr/include' if marker.is_file() else Path('/usr/include')
        if not (include / 'vulkan/vulkan.h').is_file(): raise unittest.SkipTest('Vulkan API headers required; no driver code is loaded')
        cls.temporary = tempfile.TemporaryDirectory(prefix='monado-drain-policy-')
        cls.addClassCleanup(cls.temporary.cleanup); cls.directory = Path(cls.temporary.name)
        path = PORT / 'runtime/nvn/vulkan/device.c'; source = path.read_text(); cls.source_hash = hashlib.sha256(path.read_bytes()).hexdigest()
        reset = function(source, 'static void fg_present_reset(void) {')
        require = function(source, 'static void fg_result_require(const FgPresentResult *r) {')
        drain = function(source, 'void vk_framegen_drain(void) {')
        begin = source[source.index('bool vk_frame_begin(const float clear[4]) {'):source.index('    cur_frame.ok = false;', source.index('bool vk_frame_begin(const float clear[4]) {'))]
        begin = begin.replace('bool vk_frame_begin(', 'static bool probe_frame_begin(') + '    (void)clear; probe_acquire(); return true;\n}\n'
        (cls.directory / 'begin.inc').write_text(begin)
        cls.binaries = {}
        for legacy in (False, True):
            fragment = cls.directory / ('legacy.inc' if legacy else 'actual.inc')
            # Retain the exact pre-correction wait-only drain as a failing
            # reproducer. All reset/error/header code remains current.
            legacy_drain = 'void vk_framegen_drain(void) { fg_presenter_collect(&fg_presenter,true,NULL); }\n'
            fragment.write_text(require + (legacy_drain if legacy else drain) + reset)
            binary = cls.directory / ('legacy' if legacy else 'actual')
            command = [compiler, '-std=c11', '-O1', '-UNDEBUG', '-DXCDE_LINUX_FRAMEGEN', '-pthread',
                       '-ffp-contract=off', '-fno-strict-aliasing', '-I' + str(include),
                       '-DFG_HEADER="' + str(PORT / 'runtime/nvn/vulkan/framegen_present_linux.h') + '"',
                       '-DPOLICY_HEADER="' + str(PORT / 'runtime/nvn/framegen_policy.h') + '"',
                       '-DDRAIN_FRAGMENT="' + str(fragment) + '"', '-DBEGIN_FRAGMENT="' + str(cls.directory / 'begin.inc') + '"',
                       str(ROOT / 'tests/native/test_framegen_drain_policy.c'), '-lm', '-o', str(binary)]
            if os.environ.get('MONADO_TEST_UBSAN') == '1': command[1:1] = ['-fsanitize=undefined', '-fno-sanitize-recover=all']
            result = subprocess.run(command, cwd=cls.directory, capture_output=True, text=True, timeout=120)
            if result.returncode: raise AssertionError(result.stdout + result.stderr)
            cls.binaries[legacy] = binary
        cls.addClassCleanup(lambda: cls.assert_source_unchanged())
    @classmethod
    def assert_source_unchanged(cls):
        if hashlib.sha256((PORT / 'runtime/nvn/vulkan/device.c').read_bytes()).hexdigest() != cls.source_hash:
            raise AssertionError('reviewed source changed during focused native policy tests')
    def scenario(self, name):
        result = subprocess.run([str(self.binaries[False]), name], cwd=self.directory, capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr); self.assertIn('PASS', result.stdout)
    def test_drain_preserves_successful_events_and_reset_has_no_ui_callbacks(self): self.scenario('safe')
    def test_drain_and_reset_reject_timeout_device_and_surface_failures_before_discard(self): self.scenario('errors')
    def test_actual_worker_busy_drain_and_queued_failures(self): self.scenario('worker')
    def test_real_acquire_budget_and_native_frame_begin_boundary(self): self.scenario('budget')
    def test_old_wait_only_drain_is_rejected_by_failure_oracle(self):
        def no_core(): resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
        result = subprocess.run([str(self.binaries[True]), 'errors'], cwd=self.directory, capture_output=True, text=True, timeout=15, preexec_fn=no_core)
        self.assertEqual(result.returncode, -signal.SIGABRT, result.stdout + result.stderr)
        self.assertIn('drain must validate failed status', result.stderr)

if __name__ == '__main__': unittest.main()
