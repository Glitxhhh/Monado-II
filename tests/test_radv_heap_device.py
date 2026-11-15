"""[XCDE] LINUX-070: real capability blocks with authored Vulkan responses.

No GPU, game input, device creation or profile is used. The temporary includes
come from device.c itself so the fixture exercises its actual driver gate and
enabled-feature chain rather than a second implementation of that policy.
"""
from pathlib import Path
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
PORT = ROOT / 'Monado_Linux'


def vulkan_include():
    explicit = os.environ.get('MONADO_TEST_VULKAN_INCLUDE')
    if explicit:
        return Path(explicit) if (Path(explicit) / 'vulkan/vulkan.h').is_file() else None
    marker = PORT / '.deps/graphics/active-sysroot.txt'
    if marker.is_file():
        include = Path(marker.read_text().strip()) / 'usr/include'
        if (include / 'vulkan/vulkan.h').is_file():
            return include
    include = Path('/usr/include')
    return include if (include / 'vulkan/vulkan.h').is_file() else None


def block_end(source, start):
    opening = source.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return end


def guarded_heap_features(source):
    marker = source.index('    // [XCDE] LINUX-070: map the complete')
    start = source.rfind('#ifdef __linux__\n', 0, marker)
    if start < 0 or source[start:marker].strip() != '#ifdef __linux__':
        raise AssertionError('heap feature block no longer has its expected Linux guard')
    depth = 0
    end = start
    for line in source[start:].splitlines(keepends=True):
        directive = line.strip().split(maxsplit=1)[0] if line.strip() else ''
        if directive in ('#if', '#ifdef', '#ifndef'):
            depth += 1
        elif directive == '#endif':
            depth -= 1
        end += len(line)
        if depth == 0:
            return source[start:end], end
    raise AssertionError('unterminated production heap feature guard')


@unittest.skipUnless(sys.platform == 'linux', 'Linux-only RADV device policy')
class RadvHeapDevice(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        local = PORT / '.deps/bin/clang-cc'
        compiler = os.environ.get('MONADO_TEST_CC') or (str(local) if local.is_file() else shutil.which('clang'))
        if not compiler:
            raise unittest.SkipTest('Clang is required for the native device-policy fixture')
        include = vulkan_include()
        if include is None:
            raise unittest.SkipTest('Vulkan development headers required; set MONADO_TEST_VULKAN_INCLUDE')
        source = (PORT / 'runtime/nvn/vulkan/device.c').read_text()
        start = source.index('    // [XCDE] LINUX-070: LASRecomp')
        end = source.index('    bool fault =', start)
        policy = source[start:end]
        feature, feature_end = guarded_heap_features(source)
        # Query and compose the adjacent optional feature from bounded real C
        # blocks, not a range ending at a later log line. Its availability must
        # neither erase the RADV heap feature nor leave an unmatched #endif.
        feedback_start = source.index('    VkPhysicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT feedback =')
        feedback_if = re.search(r'(?m)^    if\s*\(g_vk\.feedback_loop\)\s*\{', source[feedback_start:])
        if feedback_if is None:
            raise AssertionError('missing production feedback feature query')
        feedback_query = source[feedback_start:block_end(source, feedback_start + feedback_if.start())]
        feedback_if = re.search(r'(?m)^    if\s*\(g_vk\.feedback_loop\)\s*\{', source[feature_end:])
        if feedback_if is None:
            raise AssertionError('missing production feedback feature composition')
        feedback_start = feature_end + feedback_if.start()
        feedback_chain = source[feedback_start:block_end(source, feedback_start)]
        cls.temporary = tempfile.TemporaryDirectory(prefix='monado-radv-device-')
        cls.addClassCleanup(cls.temporary.cleanup)
        temporary = Path(cls.temporary.name)
        (temporary / 'radv_capability.inc').write_text(policy)
        (temporary / 'radv_features.inc').write_text(feature)
        (temporary / 'feedback_capability.inc').write_text(feedback_query)
        (temporary / 'feedback_chain.inc').write_text(feedback_chain)
        cls.binary = temporary / 'radv-device'
        command = [compiler, '-std=gnu11', '-O1', '-UNDEBUG', '-Wall', '-Wextra', '-Werror',
                   '-ffp-contract=off', '-fno-strict-aliasing', '-I', str(include),
                   '-I', str(temporary), str(PORT / 'tests/native/test_radv_heap_device.c'),
                   '-o', str(cls.binary)]
        cls.environment = dict(os.environ)
        for key in tuple(cls.environment):
            if key.startswith(('XCDE_', 'LARECOMP_', 'LASRECOMP_')):
                cls.environment.pop(key)
        if os.environ.get('MONADO_TEST_UBSAN') == '1':
            command[1:1] = ['-fsanitize=undefined', '-fno-sanitize-recover=all']
            cls.environment['UBSAN_OPTIONS'] = 'halt_on_error=1:print_stacktrace=1'
        compiled = subprocess.run(command, env=cls.environment, capture_output=True,
                                  text=True, timeout=120)
        if compiled.returncode:
            raise AssertionError(compiled.stdout + compiled.stderr)

    def scenario(self, name):
        result = subprocess.run([str(self.binary), name], cwd=self.temporary.name,
                                env=self.environment, capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('RADV device policy: PASS ' + name, result.stdout)
        self.assertEqual(result.stderr, '')

    def test_nvidia_does_not_query_or_enable_new_features(self): self.scenario('nvidia')
    def test_amd_proprietary_driver_stays_on_existing_path(self): self.scenario('amd-proprietary')
    def test_radv_enables_both_supported_paths(self): self.scenario('radv')
    def test_dma_buf_path_does_not_require_placed_mapping(self): self.scenario('dma-only')
    def test_placed_mapping_does_not_require_dma_buf(self): self.scenario('placed-only')
    def test_map_feature_is_required(self): self.scenario('no-map')
    def test_unmap_reservation_feature_is_required(self): self.scenario('no-reserve')
    def test_zero_placed_alignment_is_refused(self): self.scenario('zero-alignment')
    def test_non_power_of_two_placed_alignment_is_refused(self): self.scenario('bad-alignment')
    def test_radv_without_extensions_retains_fallback(self): self.scenario('no-extensions')
    def test_feedback_and_radv_features_compose(self): self.scenario('radv-feedback')
    def test_feedback_query_failure_preserves_radv_features(self): self.scenario('radv-feedback-unavailable')
    def test_nvidia_feedback_does_not_enable_radv_heap_features(self): self.scenario('nvidia-feedback')
    def test_feedback_preserves_dma_only_fallback(self): self.scenario('dma-feedback')


if __name__ == '__main__':
    unittest.main()
