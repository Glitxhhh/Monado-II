"""Authored native game-library ABI fixtures; no game inputs or game execution."""
import hashlib
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
PORT = ROOT / 'Monado_Linux'
spec = importlib.util.spec_from_file_location('monado_game_identity_test', PORT / 'scripts/gamecode_identity.py')
identity = importlib.util.module_from_spec(spec)
spec.loader.exec_module(identity)


class Identity(unittest.TestCase):
    def test_content_and_compiler_policy_invalidate_but_renderer_does_not(self):
        with tempfile.TemporaryDirectory() as directory:
            p = Path(directory)
            for name in identity.FIXED:
                path = p / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text('authored input ' + name)
            first = identity.identity(p)
            (p / 'runtime/renderer.c').write_text('an unrelated renderer change')
            self.assertEqual(identity.identity(p), first)
            (p / 'runtime/cpu.h').write_text('changed guest ABI')
            self.assertNotEqual(identity.identity(p)['id'], first['id'])
            self.assertNotEqual(identity.identity(p, {'compiler': 'changed'})['id'], identity.identity(p)['id'])

    def test_linked_identity_source_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            p = Path(directory)
            (p / 'runtime').mkdir()
            try:
                (p / 'runtime/cpu.h').symlink_to('/dev/null')
            except OSError as error:
                if os.name == 'nt' and error.winerror == 1314:
                    self.skipTest('Windows symbolic-link privilege unavailable; native Windows link guards have separate fixtures')
                raise
            with self.assertRaisesRegex(ValueError, 'ordinary'):
                identity.identity(p)


@unittest.skipUnless(sys.platform == 'linux', 'Linux shared-library loader fixture')
class NativeLibrary(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cc = os.environ.get('MONADO_TEST_CC') or str(PORT / '.deps/bin/clang-cc')
        if not Path(cls.cc).is_file():
            raise unittest.SkipTest('native Clang compiler is required')
        cls.temp = tempfile.TemporaryDirectory(prefix='monado-gamelib-test-')
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
        (cls.out / 'host.c').write_text('''#include "gamelib.h"
#include <stdio.h>
#include <string.h>
static const char *directory;
int authored_host_add(int x){return x+1;}
bool rt_resource(const char*n,const char*d,char*out,size_t size){(void)d;snprintf(out,size,"%s/%s",directory,n);return true;}
int main(int argc,char**argv){directory=argv[1];char why[512];if(!gamelib_load("unused",why,sizeof why)){fprintf(stderr,"%s",why);return 3;}
Cpu c={0};gamelib_tables()->main_fns[0](&c);return c.x[0]==42?0:4;}
''')
        (cls.out / 'exports.map').write_text('{ global: lasgame_tables; local: *; };\n')
        cls.run_compile([*cls.flags, '-fPIC', '-fvisibility=hidden', '-shared', cls.out / 'code.c',
            PORT / 'runtime/host/gamelib_entry_linux.c', '-Wl,--version-script,' + str(cls.out / 'exports.map'), '-o', cls.out / 'master.so'])
        cls.run_compile([*cls.flags, '-rdynamic', PORT / 'runtime/gamelib.c', cls.out / 'host.c', '-ldl', '-o', cls.out / 'host'])

    @classmethod
    def run_compile(cls, args):
        result = subprocess.run([cls.cc, *map(str, args)], capture_output=True, text=True, timeout=120)
        if result.returncode:
            raise AssertionError(result.stderr)

    def scenario(self, *, wrong=False, linked=False, missing=False):
        with tempfile.TemporaryDirectory(dir=self.out) as directory:
            p = Path(directory)
            if linked:
                (p / 'lasgame.so').symlink_to(self.out / 'master.so')
            elif not missing:
                shutil.copyfile(self.out / 'master.so', p / 'lasgame.so')
            (p / 'lasgame.so.id').write_text(('b' * 64 if wrong else self.abi) + '\n')
            result = subprocess.run([self.out / 'host', p], env={'PATH': '/usr/bin:/bin',
                'LARECOMP_GAME_LIB': '/does/not/control/the/linux/library'}, capture_output=True, text=True, timeout=10)
            return result.returncode, result.stderr

    def test_host_imports_resolve_through_real_table_getter(self):
        code, log = self.scenario()
        self.assertEqual(code, 0, log)

    def test_mismatched_missing_and_linked_library_refuse_before_dispatch(self):
        for args in ({'wrong': True}, {'linked': True}, {'missing': True}):
            with self.subTest(args=args):
                self.assertEqual(self.scenario(**args)[0], 3)

    def test_only_authored_table_getter_is_exported(self):
        result = subprocess.run(['/usr/bin/nm', '-D', '--defined-only', self.out / 'master.so'], capture_output=True, text=True, check=True)
        self.assertEqual({x.split()[-1] for x in result.stdout.splitlines()}, {'lasgame_tables'})
