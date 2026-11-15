"""Native Windows production Vulkan adapter selection with authored query results."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
PORT = ROOT / 'Monado_Windows'


@unittest.skipUnless(sys.platform == 'win32', 'native Windows GPU selection')
class WindowsGpuSelection(unittest.TestCase):
    def test_actual_selection_reorder_capabilities_and_surface_failures(self):
        python = PORT / '.venv/Scripts/python.exe'
        config = PORT / '.deps/windows-deps.json'
        if not python.is_file() or not config.is_file():
            self.skipTest('project-local native compiler and Vulkan/SDL headers required')
        prefix = Path(json.loads(config.read_text(encoding='utf-8'))['prefix'])
        env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(PORT / '.deps/zig-global-cache'),
                   ZIG_LOCAL_CACHE_DIR=str(PORT / 'build/zig-cache'))
        with tempfile.TemporaryDirectory(prefix='gpu-selection-', dir=PORT / 'build') as directory:
            out = Path(directory) / 'selection.exe'
            # Execute the exact production selection functions independently of
            # unrelated presentation entrypoints; COFF does not discard their
            # unresolved externals in the same way the Linux fixture's ELF does.
            source = (PORT / 'runtime/nvn/vulkan/device.c').read_text(encoding='utf-8')
            start = source.index('static bool has_ext(')
            end = source.index('\nstatic bool g_fault_ext;', start)
            (Path(directory) / 'production_device_selection.h').write_text(source[start:end], encoding='utf-8')
            command = [python, '-m', 'ziglang', 'cc', '-std=gnu11', '-O1', '-UNDEBUG',
                '-ffp-contract=off', '-fno-strict-aliasing', '-ffunction-sections', '-fdata-sections',
                '-I' + str(prefix / 'include'), '-I' + directory, '-I' + str(PORT / 'third_party/imgui'),
                '-I' + str(PORT / 'third_party/imgui/backends'),
                str(ROOT / 'tests/native/test_gpu_device_selection_linux.c'),
                '-Wl,--gc-sections', str(prefix / 'lib/libSDL3.dll.a'),
                str(prefix / 'lib/libvulkan-1.dll.a'), '-o', str(out)]
            built = subprocess.run(command, env=env, capture_output=True, text=True, timeout=120)
            self.assertEqual(built.returncode, 0, (built.stdout + built.stderr)[-6000:])
            env['PATH'] = str(prefix / 'bin') + os.pathsep + env['PATH']
            for scenario in range(18):
                with self.subTest(scenario=scenario):
                    ran = subprocess.run([out, str(scenario)], env=env, capture_output=True, text=True, timeout=10)
                    self.assertEqual(ran.returncode, 100 if 13 <= scenario <= 16 else 0, ran.stdout + ran.stderr)
