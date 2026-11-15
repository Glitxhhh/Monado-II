"""WINPARITY-002: native PE libraries through actual Windows production entry."""
import os
import hashlib
import ctypes
import re
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
PORT = ROOT / 'Monado_Windows'


@unittest.skipUnless(sys.platform == 'win32', 'native Windows production entry')
class WindowsGameLibrary(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.python = PORT / '.venv/Scripts/python.exe'
        if not cls.python.is_file():
            raise unittest.SkipTest('project-local native Zig compiler required')
        cls.temp = tempfile.TemporaryDirectory(prefix='monado-native-boot-', dir=PORT / 'build')
        cls.addClassCleanup(cls.temp.cleanup)
        cls.out = Path(cls.temp.name)
        cls.identity = 'a' * 64
        cls.env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(PORT / '.deps/zig-global-cache'),
                       ZIG_LOCAL_CACHE_DIR=str(PORT / 'build/zig-cache'))
        cls.flags = ['-std=gnu11', '-O1', '-UNDEBUG', '-march=x86_64_v2', '-ffp-contract=off',
                     '-fno-strict-aliasing', '-I' + str(PORT / 'runtime'), '-DGAMELIB_ID="' + cls.identity + '"']
        authored = '''#include "gamelib.h"
#include <windows.h>
BOOL WINAPI DllMain(HINSTANCE instance,DWORD reason,LPVOID reserved){
 (void)instance;(void)reserved;
 if(reason==DLL_PROCESS_ATTACH){const char text[]="AUTHORED DLL mapped\\n";DWORD written;
 WriteFile(GetStdHandle(STD_ERROR_HANDLE),text,sizeof text-1,&written,NULL);}
 return TRUE;
}
static void authored_guest(Cpu*c){c->x[0]=42;}
const uint64_t main_fn_addrs[]={1},sdk_fn_addrs[]={2},main_fn_count=1,sdk_fn_count=1;
const GuestFn main_fn_ptrs[]={authored_guest},sdk_fn_ptrs[]={authored_guest};
'''
        (cls.out / 'code.c').write_text(authored, encoding='utf-8')
        (cls.out / 'wrong.c').write_text('''#include "gamelib.h"
static const GameLibTables tables={.abi=GAMELIB_ABI+1,.id=GAMELIB_ID};
GAMELIB_EXPORT const GameLibTables *lasgame_tables(void){return &tables;}
''', encoding='utf-8')
        cls.compile(['-DXCDE_GFX', '-Dmain=authored_boot_main', '-c', PORT / 'runtime/host/main_boot.c',
                     '-o', cls.out / 'boot.o'])
        cls.compile(['-shared', cls.out / 'code.c', PORT / 'runtime/host/gamelib_entry_windows.c', '-o', cls.out / 'valid.dll'])
        cls.compile(['-shared', cls.out / 'wrong.c', '-o', cls.out / 'wrong.dll'])
        cls.resource=cls.out/'process-manifest.res'
        built=subprocess.run([cls.python,'-m','ziglang','rc','/I',str(PORT/'tools'),'/fo',str(cls.resource),str(PORT/'tools/windows_utf8.rc')],env=cls.env,capture_output=True,text=True,timeout=30)
        if built.returncode:raise AssertionError(built.stdout+built.stderr)
        common = [cls.out / 'boot.o', PORT / 'tests/native/test_boot_game_library.c',
                  PORT / 'runtime/host/run_diagnostics_windows.c',cls.resource]
        cls.compile([*common, PORT / 'runtime/gamelib.c', '-lbcrypt', '-ladvapi32', '-o', cls.out / 'dynamic.exe'])
        cls.compile([*common, PORT / 'runtime/host/gamelib_static.c', cls.out / 'code.c',
                     '-lbcrypt', '-ladvapi32', '-o', cls.out / 'static.exe'])
        cls.compile(['-DAUTHORED_DIRECT_ENTRY',*common,PORT/'runtime/gamelib.c','-lbcrypt','-ladvapi32','-o',cls.out/'direct.exe'])
        built=subprocess.run([cls.python,'-m','ziglang','c++','-std=c++17','-O1','-UNDEBUG','-municode',
            str(PORT/'tests/native/test_launcher_profile_spawn_windows.cpp'),str(cls.resource),'-o',str(cls.out/'spawn.exe')],
            env=cls.env,capture_output=True,text=True,timeout=120)
        if built.returncode:raise AssertionError(built.stdout+built.stderr)
        cls.compile(['-DXCDE_GFX','-Dmain=authored_boot_main','-include',PORT/'tests/native/test_boot_relaunch_hooks.h',
                     '-c',PORT/'runtime/host/main_boot.c','-o',cls.out/'relaunch-boot.o'])
        cls.compile([cls.out/'relaunch-boot.o',PORT/'tests/native/test_boot_game_library.c',
                     PORT/'runtime/host/run_diagnostics_windows.c',PORT/'runtime/host/gamelib_static.c',cls.out/'code.c',
                     cls.resource,'-lbcrypt','-ladvapi32','-o',cls.out/'relaunch.exe'])

    def test_explicit_long_unicode_profile_applies_before_precompile_static_and_missing_library(self):
        with tempfile.TemporaryDirectory(dir=self.out,prefix='profile-start-') as temporary:
            base=Path(temporary);profile=base/('雪 literal $() ` '+('x'*70))/('nested '+('y'*70))/('profile '+('z'*70))
            profile.mkdir(parents=True);(profile/'profile-marker').write_bytes(b'authored')
            self.assertGreater(len(str(profile)),300)
            source=base/'input';(source/'exefs').mkdir(parents=True);(source/'romfs').mkdir()
            env=dict(self.env,AUTHORED_LIBRARY_ROOT=str(source),AUTHORED_PROFILE_ROOT=str(profile));env.pop('XCDE_WATCHDOG',None)
            for binary,mode,code,text in [('dynamic.exe','profile-precompile',23,'AUTHORED precompile boundary'),
                                           ('static.exe','profile-game',0,'stopped before guest execution'),
                                           ('dynamic.exe','profile-game',1,'FATAL: cannot load game code')]:
                with self.subTest(binary=binary,mode=mode):
                    result=subprocess.run([str(self.out/binary),mode,str(source)],env=env,cwd=self.out,capture_output=True,text=True,timeout=20)
                    self.assertEqual(result.returncode,code,result.stderr);self.assertIn(text,result.stderr)
            for mode in ['missing','empty','relative','duplicate']:
                result=subprocess.run([str(self.out/'dynamic.exe'),'profile-'+mode,str(source)],env=env,cwd=self.out,capture_output=True,text=True,timeout=20)
                self.assertEqual(result.returncode,2,result.stderr);self.assertNotIn('AUTHORED',result.stderr)

    def test_production_launcher_spawn_long_runtime_and_profile_preserves_all_arguments(self):
        with tempfile.TemporaryDirectory(dir=self.out,prefix='long-launch-') as temporary:
            base=Path(temporary)/('雪 literal $() ` '+('a'*70))/('nested '+('b'*70))
            runtime=base/('runtime '+('c'*60));(runtime/'build').mkdir(parents=True)
            profile=base/('profile '+('d'*60));(profile/'build').mkdir(parents=True)
            (profile/'profile-marker').write_bytes(b'authored');shutil.copyfile(self.out/'direct.exe',runtime/'build/xcde.exe')
            self.assertGreater(len(str(runtime)),300);self.assertGreater(len(str(profile)),300)
            env=dict(self.env,AUTHORED_LIBRARY_ROOT=str(runtime),AUTHORED_PROFILE_ROOT=str(profile),
                     AUTHORED_PRECOMPILE_TARGET='authored target 雪 $() ` quote" slash\\');env.pop('XCDE_WATCHDOG',None)
            result=subprocess.run([str(self.out/'spawn.exe'),str(runtime),str(profile)],env=env,cwd=self.out,capture_output=True,text=True,timeout=30)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            self.assertIn('PASS production launcher short-CWD spawn',result.stdout)
            self.assertIn('AUTHORED precompile boundary',(profile/'build/authored-start.log').read_text())

    @classmethod
    def compile(cls, args):
        built = subprocess.run([cls.python, '-m', 'ziglang', 'cc', *cls.flags, *map(str, args)],
                               env=cls.env, capture_output=True, text=True, timeout=120)
        if built.returncode:
            raise AssertionError(built.stdout + built.stderr)

    def scenario(self, kind='valid', static=False, precompile=False, present=False, fsr4=False, fsr4_missing=False,bad_precompile=None):
        with tempfile.TemporaryDirectory(dir=self.out, prefix='space é ') as directory:
            root = Path(directory)
            (root / 'exefs').mkdir()
            (root / 'romfs').mkdir()
            if kind != 'missing':
                dll = root / 'lasgame.dll'
                if kind == 'bad':
                    dll.write_bytes(b'authored non-PE library')
                else:
                    shutil.copyfile(self.out / ('wrong.dll' if kind == 'abi' else 'valid.dll'), dll)
                (root / 'lasgame.dll.id').write_bytes((
                    ('b' * 64 if kind == 'identity' else self.identity) + ('junk' if kind == 'trailing' else '\n')
                ).encode('ascii'))
                sha = hashlib.sha256(dll.read_bytes()).hexdigest()
                if kind != 'no-hash':
                    (root / 'lasgame.dll.sha256').write_bytes((
                        ('0' * 64 if kind == 'hash' else sha) + ('junk' if kind == 'hash-trailing' else '\n')
                    ).encode('ascii'))
                if kind == 'modified':
                    with dll.open('ab') as changed:
                        changed.write(b'changed after receipt')
                if kind == 'hardlink':
                    os.link(dll, root / 'second.dll')
                if kind == 'hash-hardlink':
                    os.link(root / 'lasgame.dll.sha256', root / 'second-hash')
            env = dict(self.env, AUTHORED_LIBRARY_ROOT=str(root))
            env.pop('XCDE_WATCHDOG', None)
            return subprocess.run([self.out / ('static.exe' if static else 'dynamic.exe'),
                                   bad_precompile or ('fsr4-missing' if fsr4_missing else 'fsr4' if fsr4 else 'present' if present else 'precompile' if precompile else 'game'), str(root)],
                                  env=env, cwd=root, capture_output=True, text=True, timeout=15)

    def test_matching_companion_precedes_all_workers_and_dispatch(self):
        result = self.scenario()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertLess(result.stderr.index('[gamelib]'), result.stderr.index('AUTHORED module-loader'))
        self.assertIn('stopped before guest execution', result.stderr)

    def test_invalid_companions_fail_before_workers_modules_or_dispatch(self):
        for kind in ('missing', 'identity', 'trailing', 'bad', 'abi', 'no-hash', 'hash', 'hash-trailing', 'modified', 'hardlink', 'hash-hardlink'):
            with self.subTest(kind=kind):
                result = self.scenario(kind)
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertIn('FATAL: cannot load game code:', result.stderr)
                self.assertNotIn('AUTHORED', result.stderr)

    def test_existence_checks_hash_companion_without_mapping_code(self):
        for kind in ('valid', 'missing', 'no-hash', 'hash', 'modified', 'hardlink', 'hash-hardlink'):
            with self.subTest(kind=kind):
                result = self.scenario(kind, present=True)
                self.assertEqual(result.returncode, 0 if kind == 'valid' else 1, result.stderr)
                self.assertNotIn('AUTHORED DLL mapped', result.stderr)

    def test_precompile_bypasses_game_code(self):
        result = self.scenario('missing', precompile=True)
        self.assertEqual(result.returncode, 23, result.stderr)
        self.assertIn('AUTHORED precompile boundary', result.stderr)

    def test_native_launch_monitor_prefix_is_checked_before_entry(self):
        result=self.scenario('missing',bad_precompile='monitor-valid')
        self.assertEqual(result.returncode,23,result.stderr)
        for mode in ('invalid','missing','duplicate'):
            with self.subTest(mode=mode):
                result=self.scenario('missing',bad_precompile='monitor-'+mode)
                self.assertEqual(result.returncode,2,result.stderr)
                self.assertNotIn('AUTHORED',result.stderr)

    def test_precompile_rejects_missing_duplicate_extra_or_invalid_session_tokens(self):
        for mode in ('missing','duplicate','extra','session','identity'):
            with self.subTest(mode=mode):
                result=self.scenario('missing',bad_precompile='precompile-'+mode)
                self.assertEqual(result.returncode,2,result.stderr)
                self.assertIn('--session HEX32 --identity HEX64',result.stderr)
                self.assertNotIn('AUTHORED',result.stderr)

    def test_static_fallback_uses_same_production_entry(self):
        result = self.scenario('missing', static=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('stopped before guest execution', result.stderr)

    def test_explicit_fsr4_unavailable_keeps_boot_fallback(self):
        result=self.scenario(fsr4=True)
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertIn('AUTHORED FSR4 configuration boundary',result.stderr)
        self.assertIn('authored local models unavailable',result.stderr)
        self.assertLess(result.stderr.index('FSR4 configuration'),result.stderr.index('AUTHORED module-loader'))

    def test_incomplete_fsr4_configuration_rejected_before_boot(self):
        result=self.scenario('missing',fsr4_missing=True)
        self.assertEqual(result.returncode,2,result.stderr)
        self.assertIn('requires both',result.stderr)
        self.assertNotIn('AUTHORED',result.stderr)

    def relaunch(self,fault=None,always=False):
        directory=self.out/('Unicode 雪 literal $() ` relaunch '+('x'*70))/('nested '+('y'*70))
        directory.mkdir(parents=True,exist_ok=True)
        binary=directory/'authored long executable 雪.exe';shutil.copyfile(self.out/'relaunch.exe',binary)
        self.assertGreater(len(str(binary)),260)
        env=dict(self.env,AUTHORED_RELAUNCH='always' if always else 'once',AUTHORED_RELAUNCH_EXPECT=str(directory),AUTHORED_LIBRARY_ROOT=str(directory))
        env.pop('XCDE_RELAUNCH',None)
        if fault:env['AUTHORED_RELAUNCH_FAULT']=fault
        else:env.pop('AUTHORED_RELAUNCH_FAULT',None)
        # Win32's process-current-directory limit is separate from the relaunch
        # executable/command contract; keep this authored fixture CWD short.
        return subprocess.run([str(binary),'precompile',str(directory)],executable='\\\\?\\'+str(binary),
                              cwd=self.out,env=env,capture_output=True,text=True,timeout=20)

    def test_constructor_relaunch_preserves_long_unicode_executable_and_command(self):
        result=self.relaunch()
        self.assertEqual(result.returncode,23,result.stderr)
        self.assertEqual(result.stderr.count('[boot] guest address range'),1)
        self.assertIn('AUTHORED precompile boundary',result.stderr)
        self.assertNotIn('AUTHORED module-loader',result.stderr)

    def test_constructor_relaunch_faults_never_resume_unowned_children(self):
        api=ctypes.WinDLL('kernel32',use_last_error=True)
        api.OpenProcess.argtypes=[ctypes.c_ulong,ctypes.c_int,ctypes.c_ulong];api.OpenProcess.restype=ctypes.c_void_p
        api.WaitForSingleObject.argtypes=[ctypes.c_void_p,ctypes.c_ulong]
        api.CloseHandle.argtypes=[ctypes.c_void_p]
        for fault in ('create','configure','assign','resume'):
            with self.subTest(fault=fault):
                result=self.relaunch(fault)
                self.assertEqual(result.returncode,1,result.stderr)
                self.assertIn('cannot safely relaunch owned process',result.stderr)
                self.assertNotIn('AUTHORED precompile boundary',result.stderr)
                child=re.search(r'AUTHORED relaunch child PID ([0-9]+)',result.stderr)
                self.assertEqual(child is not None,fault in ('assign','resume'))
                if child:
                    handle=api.OpenProcess(0x100000,False,int(child[1]))
                    if handle:
                        try:self.assertEqual(api.WaitForSingleObject(handle,0),0,'owned suspended child leaked')
                        finally:api.CloseHandle(handle)
                    else:self.assertEqual(ctypes.get_last_error(),87)

    def test_constructor_relaunch_remains_bounded_to_eight_children(self):
        result=self.relaunch(always=True)
        self.assertEqual(result.returncode,1,result.stderr)
        self.assertEqual(result.stderr.count('[boot] guest address range'),9)
        self.assertEqual(result.stderr.count('AUTHORED relaunch child PID '),8)
        self.assertNotIn('AUTHORED precompile boundary',result.stderr)
