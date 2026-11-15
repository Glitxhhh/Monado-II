"""Production boot ordering with authored libraries and no game inputs or UI."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
PORT = ROOT / 'Monado_Linux'


@unittest.skipUnless(sys.platform == 'linux', 'Linux production boot fixture')
class BootGameLibrary(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cc = os.environ.get('MONADO_TEST_CC') or str(PORT / '.deps/bin/clang-cc')
        if not Path(cls.cc).is_file():
            raise unittest.SkipTest('native Clang compiler is required')
        cls.temp = tempfile.TemporaryDirectory(prefix='monado-boot-gamelib-')
        cls.addClassCleanup(cls.temp.cleanup)
        cls.out = Path(cls.temp.name)
        cls.abi = 'a' * 64
        cls.flags = ['-O1', '-UNDEBUG', '-march=x86_64_v2', '-ffp-contract=off', '-fno-strict-aliasing',
                     '-I' + str(PORT / 'runtime'), '-DGAMELIB_ID="' + cls.abi + '"']
        if os.environ.get('MONADO_TEST_UBSAN') == '1':
            cls.flags += ['-fsanitize=undefined', '-fno-sanitize-recover=all']
        (cls.out / 'code.c').write_text('''#include "gamelib.h"
extern int authored_host_add(int);
static void authored_guest(Cpu*c){c->x[0]=authored_host_add(41);}
const uint64_t main_fn_addrs[]={1},sdk_fn_addrs[]={2},main_fn_count=1,sdk_fn_count=1;
const GuestFn main_fn_ptrs[]={authored_guest},sdk_fn_ptrs[]={authored_guest};
''')
        (cls.out / 'wrong-abi.c').write_text('''#include "gamelib.h"
static const GameLibTables tables={.abi=GAMELIB_ABI+1,.id=GAMELIB_ID};
GAMELIB_EXPORT const GameLibTables *lasgame_tables(void){return &tables;}
''')
        (cls.out / 'exports.map').write_text('{ global: lasgame_tables; local: *; };\n')
        cls.compile(['-DXCDE_GFX', '-Dmain=authored_boot_main', '-c',
                     PORT / 'runtime/host/main_boot.c', '-o', cls.out / 'boot.o'])
        for name, source in (('valid', cls.out / 'code.c'), ('wrong-abi', cls.out / 'wrong-abi.c')):
            entry = [PORT / 'runtime/host/gamelib_entry_linux.c'] if name == 'valid' else []
            cls.compile(['-fPIC', '-fvisibility=hidden', '-shared', source, *entry,
                         '-Wl,--version-script,' + str(cls.out / 'exports.map'), '-o', cls.out / (name + '.so')])
        common = [cls.out / 'boot.o', ROOT / 'tests/native/test_boot_game_library.c',
                  PORT / 'runtime/platform/linux_state_guard.c', '-ldl', '-pthread', '-rdynamic']
        cls.compile([*common, PORT / 'runtime/gamelib.c', '-o', cls.out / 'dynamic-host'])
        cls.compile([*common, PORT / 'runtime/host/gamelib_static.c', cls.out / 'code.c',
                     '-o', cls.out / 'static-host'])

    @classmethod
    def compile(cls, args):
        result = subprocess.run([cls.cc, *cls.flags, *map(str, args)],
                                capture_output=True, text=True, timeout=120)
        if result.returncode:
            raise AssertionError(result.stderr)

    def scenario(self, kind='valid', *, static=False, precompile=False):
        with tempfile.TemporaryDirectory(dir=self.out) as directory:
            root = Path(directory)
            library, state, inputs = (root / name for name in ('library', 'state', 'authored-input'))
            for path in (library, state, inputs):
                path.mkdir()
            if kind != 'missing':
                if kind == 'unloadable':
                    (library / 'lasgame.so').write_text('authored non-ELF companion')
                else:
                    shutil.copyfile(self.out / ('wrong-abi.so' if kind == 'wrong-abi' else 'valid.so'), library / 'lasgame.so')
                (library / 'lasgame.so.id').write_text(('b' * 64 if kind == 'wrong-id' else self.abi) + '\n')
            host = self.out / ('static-host' if static else 'dynamic-host')
            return subprocess.run([host, 'precompile' if precompile else 'game', library, state, inputs],
                                  env={'PATH': '/usr/bin:/bin'}, capture_output=True, text=True, timeout=10)

    def test_companion_is_loaded_before_module_kernel_and_dispatch_startup(self):
        result = self.scenario()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('state write guard active', result.stderr)
        self.assertLess(result.stderr.index('[gamelib]'), result.stderr.index('AUTHORED module-loader boundary'))
        self.assertIn('AUTHORED dispatch boundary; stopped before guest execution', result.stderr)
        self.assertNotIn('FORBIDDEN', result.stderr)

    def test_missing_mismatched_unloadable_and_wrong_abi_refuse_before_modules(self):
        for kind in ('missing', 'wrong-id', 'unloadable', 'wrong-abi'):
            with self.subTest(kind=kind):
                result = self.scenario(kind)
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertIn('FATAL: cannot load game code:', result.stderr)
                self.assertNotIn('AUTHORED', result.stderr)
                self.assertNotIn('FORBIDDEN', result.stderr)

    def test_precompile_still_bypasses_game_library(self):
        result = self.scenario('missing', precompile=True)
        self.assertEqual(result.returncode, 23, result.stderr)
        self.assertIn('AUTHORED precompile boundary', result.stderr)
        self.assertNotIn('cannot load game code', result.stderr)

    def test_static_game_code_shim_retains_boot_behavior(self):
        result = self.scenario('missing', static=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('AUTHORED dispatch boundary; stopped before guest execution', result.stderr)
        self.assertNotIn('FORBIDDEN', result.stderr)
