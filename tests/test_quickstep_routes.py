"""Combined native Quickstep routing fixture with authored inventory/resources."""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
import struct
import zlib
PORT = Path(__file__).resolve().parents[1] / 'Monado_Linux'

@unittest.skipUnless(sys.platform == 'linux', 'native Linux control adapter')
class QuickstepRoutesTests(unittest.TestCase):
    def test_cancel_left_then_ordinary_chooser(self):
        local = PORT / '.deps/bin/clang-cc'
        compiler = os.environ.get('MONADO_TEST_CC') or (str(local) if local.is_file() else shutil.which('clang'))
        if not compiler:
            self.skipTest('Clang is required for the native CPU header')
        with tempfile.TemporaryDirectory(prefix='monado-quickstep-routes-') as temporary:
            binary = Path(temporary) / 'routes-test'
            # Authored synthetic resource, not copied game data. The runtime CRC
            # remains pinned in production; this translation unit pins this file.
            resource = bytearray(49152)
            struct.pack_into('<II', resource, 0, 0x4448414c, 0x2713)
            struct.pack_into('<I', resource, 0x14, 0x80)
            struct.pack_into('<I', resource, 0x80, 0x20)
            resource[0xa3] = 9
            asset = Path(temporary) / 'menu/image/common_window_white_c.wilay'
            asset.parent.mkdir(parents=True)
            asset.write_bytes(resource)
            command = [compiler, '-std=gnu11', '-O1', '-UNDEBUG', '-Wall', '-Wextra', '-Werror',
                       '-ffp-contract=off', '-DQUICKSTEP_NATIVE_RESOURCE_CRC='+hex(zlib.crc32(resource)), '-fno-strict-aliasing', '-pthread', '-I', str(PORT / 'runtime'),
                       str(PORT / 'tests/native/test_quickstep_routes.c'),
                       str(PORT / 'runtime/hle/quickstep_native.c'),
                       str(PORT / 'runtime/hle/quickstep_picker.c'), '-o', str(binary)]
            if os.environ.get('MONADO_TEST_UBSAN') == '1':
                command[1:1] = ['-fsanitize=undefined', '-fno-sanitize-recover=all']
            result = subprocess.run(command, capture_output=True, text=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(binary), temporary], capture_output=True, text=True, timeout=45)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('ordinary Confirm routes: PASS', result.stdout)
if __name__ == '__main__':
    unittest.main()
