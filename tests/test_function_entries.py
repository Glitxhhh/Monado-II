"""LINUX-041: authored partition/dispatch regressions, without game input."""
from collections import Counter
from contextlib import redirect_stderr
import importlib.util
import io
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
PORT = ROOT / 'Monado_Linux'
sys.path.insert(0, str(PORT / 'tools/lifter'))
import gen  # noqa: E402
from lifter import Lifter  # noqa: E402
from patchfmt import FunctionEntries, PatchMismatch, load_patch_set, load_patches  # noqa: E402
import unicorn as uc  # noqa: E402
from unicorn import arm64_const as A  # noqa: E402

BASE = 0x7000000000
BUILD_ID = '00112233445566778899aabbccddeeff'
NOP, RET = 0xd503201f, 0xd65f03c0


def fixture():
    # Authored four-case switch with a deliberately independent partition at
    # the first case's RET. Cases intentionally have a nonsequential order.
    def adr(rd, pc, target):
        imm = target - pc
        return 0x10000000 | ((imm & 3) << 29) | (((imm >> 2) & 0x7ffff) << 5) | rd
    words = [0x2a0003e8, 0x71000d1f, 0x54000008 | ((17 - 2) << 5),
             0x90000009 | (1 << 29), 0x91000129,
             adr(10, BASE + 20, BASE + 36), 0x3868692b, 0x8b0b094a, 0xd61f0140]
    # ADRP targets the next page; its ADD selects an ordinary data page.
    words[3] = 0xd0000009  # adrp x9, BASE+0x2000
    for value in range(0x40, 0x44):
        words += [0x52800000 | (value << 5), RET]
    words += [0x52801e00, RET]  # default result0xf0
    image = bytearray(0x3000)
    for i, word in enumerate(words):
        struct.pack_into('<I', image, i * 4, word)
    image[0x2000:0x2004] = bytes([0, 6, 2, 4])
    nso = type('SyntheticNso', (), {})()
    nso.image, nso.syms, nso.relocs, nso.build_id = image, [], [], BUILD_ID
    m = object.__new__(gen.Module)
    m.name, m.base, m.nso = 'main', BASE, nso
    m.lo, m.hi, m.words = BASE, BASE + len(words) * 4, tuple(words)
    m.lifter, m.cache, m.stats, m.mu = Lifter(BASE, 'c'), {}, Counter(), None
    m.boundaries = lambda: [BASE, BASE + 40]
    entries = {BASE + (9 + 2 * i) * 4: words[9 + 2 * i] for i in range(4)}
    guards = {BASE + 4: words[1], BASE + 32: words[8]}
    hints = FunctionEntries('main', BUILD_ID, entries, guards, 'LINUX-041')
    return m, hints


def generate(directory, hints):
    m, _ = fixture()
    with patch.object(gen, 'Module', return_value=m), \
            patch('patchfmt.load_patch_set', return_value=({}, hints)), \
            patch.dict(gen.PATCHES, {}, clear=True), patch.dict(gen.RANDO_CODE, {}, clear=True), \
            patch.object(sys, 'argv', ['gen.py', 'main', 'authored-fixture', hex(BASE), str(directory), '--simd', 'c']), \
            redirect_stderr(io.StringIO()):
        gen.main()
    return (directory / 'main_table.c').read_text()


class EntryContract(unittest.TestCase):
    def test_guards_before_returning_entries_and_other_module_ignored(self):
        m, hint = fixture()
        self.assertEqual(hint.apply(m), {BASE + i * 4 for i in (9, 11, 13, 15)})
        m.name = 'sdk'
        m.nso.build_id = 'f' * 32
        self.assertEqual(hint.apply(m), set())

    def test_wrong_build_or_any_changed_word_fails_closed(self):
        m, hint = fixture()
        m.nso.build_id = 'f' * 32
        with self.assertRaises(PatchMismatch):
            hint.apply(m)
        m.nso.build_id = BUILD_ID
        original = m.words
        for address, _ in hint.entries + hint.guards:
            words = list(original); words[(address - BASE) // 4] ^= 1; m.words = words
            with self.assertRaises(PatchMismatch):
                hint.apply(m)
        m.words = original
        self.assertEqual(len(hint.apply(m)), 4)

    def test_duplicate_misaligned_invalid_word_and_missing_contract_rejected(self):
        for entries, guards, req in [([(BASE, NOP), (BASE, NOP)], [(BASE + 4, RET)], 'T'),
                                    ([(BASE, NOP)], [(BASE, NOP)], 'T'),
                                    ([(BASE + 1, NOP)], [(BASE + 4, RET)], 'T'),
                                    ([(BASE, 1 << 32)], [(BASE + 4, RET)], 'T'),
                                    ([(BASE, NOP)], [(BASE + 4, RET)], ''),
                                    ([], [(BASE + 4, RET)], 'T'),
                                    ([(BASE, NOP)], [], 'T')]:
            with self.assertRaises(ValueError):
                FunctionEntries('main', BUILD_ID, entries, guards, req)
        for build_id in ['', 'abc', 'g' * 32, '0' * 66]:
            with self.assertRaises(ValueError):
                FunctionEntries('main', build_id, [(BASE, NOP)], [(BASE + 4, RET)], 'T')

    def test_outside_text_entry_or_guard_rejected(self):
        m, _ = fixture()
        for entries, guards in [({m.hi: RET}, {BASE: m.word(BASE)}),
                                ({BASE: m.word(BASE)}, {m.hi: RET})]:
            hint = FunctionEntries('main', BUILD_ID, entries, guards, 'T')
            with self.assertRaises(PatchMismatch):
                hint.apply(m)

    def test_patch_loader_backwards_compatible_and_entries_typed(self):
        self.assertEqual(load_patch_set(None), ({}, ()))
        with tempfile.TemporaryDirectory(prefix='monado-entry-loader-') as temporary:
            file = Path(temporary) / 'profile.py'
            file.write_text("from patchfmt import Patch, FunctionEntries\n"
                            "PATCHES={0:Patch(0,lambda g,a:'', 'T')}\n"
                            "FUNCTION_ENTRIES=(FunctionEntries('main','" + BUILD_ID + "',"
                            "{4:0},{8:0},'T'),)\n")
            patches, hints = load_patch_set(file)
            self.assertEqual(set(patches), {0}); self.assertEqual(len(hints), 1)
            self.assertEqual(set(load_patches(file)), {0})
            file.write_text('PATCHES={}\nFUNCTION_ENTRIES=(object(),)\n')
            with self.assertRaises(ValueError):
                load_patch_set(file)

    def test_whole_generator_emits_every_checked_dispatch_destination(self):
        _, hints = fixture()
        with tempfile.TemporaryDirectory(prefix='monado-partition-') as temporary:
            old = Path(temporary) / 'old'; new = Path(temporary) / 'new'
            old_table = generate(old, ())
            new_table = generate(new, (hints,))
            # The original generator omits three legitimate indirect targets.
            for address, _ in hints.entries[1:]:
                self.assertNotIn('f_' + format(address, 'x') + ',', old_table)
            for address, _ in hints.entries:
                self.assertIn('f_' + format(address, 'x') + ',', new_table)
            source = ''.join(p.read_text() for p in new.glob('main_0*.c'))
            self.assertIn('rt_dispatch(c)', source)
            self.assertNotIn('hle_', source)

    def test_generator_rejects_duplicate_sets_before_writing_output(self):
        _, hints = fixture()
        with tempfile.TemporaryDirectory(prefix='monado-duplicate-entry-') as temporary:
            directory = Path(temporary) / 'output'
            with self.assertRaises(ValueError):
                generate(directory, (hints, hints))
            self.assertFalse(directory.exists())

    @unittest.skipUnless(sys.platform == 'linux', 'native CPU-header differential fixture')
    def test_real_generated_native_dispatch_matches_unicorn_registers_lr_sp(self):
        compiler = os.environ.get('MONADO_TEST_CC')
        if not compiler:
            local = PORT / '.deps/bin/clang-cc'
            compiler = str(local) if local.is_file() else shutil.which('clang')
        if not compiler:
            self.skipTest('Clang is required for the native CPU header')
        m, hints = fixture()
        arguments = [0, 1, 2, 3, 4, 255, 0x100000002, 0xffffffffffffffff]
        registers = [A.UC_ARM64_REG_X0 + i for i in range(29)] + [A.UC_ARM64_REG_X29, A.UC_ARM64_REG_X30]
        expected = []
        for argument in arguments:
            mu = uc.Uc(uc.UC_ARCH_ARM64, uc.UC_MODE_ARM)
            mu.mem_map(BASE, len(m.nso.image)); mu.mem_write(BASE, bytes(m.nso.image))
            for i, register in enumerate(registers):
                mu.reg_write(register, 0x1111000000000000 + i)
            mu.reg_write(A.UC_ARM64_REG_X0, argument)
            mu.reg_write(A.UC_ARM64_REG_X30, BASE + 0x1000)
            mu.reg_write(A.UC_ARM64_REG_SP, 0x7300080000)
            mu.reg_write(A.UC_ARM64_REG_NZCV, 0x90000000)
            mu.emu_start(BASE, BASE + 0x1000, count=128)
            self.assertEqual(mu.reg_read(A.UC_ARM64_REG_PC), BASE + 0x1000)
            expected.append([mu.reg_read(r) for r in registers] + [mu.reg_read(A.UC_ARM64_REG_NZCV)])
        number = lambda value: hex(value) + 'ull'
        code = '''
#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include "cpu.h"
volatile int g_fnprof;
void fnprof_hit(uint64_t a,uint64_t b,uint64_t d){(void)a;(void)b;(void)d;abort();}
void rt_udf(Cpu*c,uint64_t p){(void)c;(void)p;abort();}
extern const uint64_t main_fn_addrs[],main_fn_count;
extern const GuestFn main_fn_ptrs[];
void rt_dispatch(Cpu*c){
 for(uint64_t i=0;i<main_fn_count;++i)if(main_fn_addrs[i]==c->pc){main_fn_ptrs[i](c);return;}
 fprintf(stderr,"missing authored dispatch %llx\\n",(unsigned long long)c->pc);exit(37);
}
'''
        code += 'extern void ' + 'f_' + format(BASE, 'x') + '(Cpu*c);\n'
        code += 'static const uint64_t args[]={' + ','.join(map(number, arguments)) + '};\n'
        code += 'static const uint64_t expected[][32]={' + ','.join('{' + ','.join(map(number, row)) + '}' for row in expected) + '};\n'
        code += '''
int main(void){
 void*p=mmap((void*)0x7000000000ull,0x3000,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);
 assert(p==(void*)0x7000000000ull);const unsigned char table[]={0,6,2,4};memcpy((char*)p+0x2000,table,4);
 for(unsigned a=0;a<sizeof(args)/sizeof(args[0]);++a){Cpu c;memset(&c,0xa5,sizeof(c));
  for(unsigned i=0;i<32;++i)c.x[i]=0x1111000000000000ull+i;
  c.x[0]=args[a];c.x[30]=0x7000001000ull;c.sp=0x7300080000ull;c.nzcv=0x90000000;
  c.fpcr=0x1234;c.fpsr=0x5678;
'''
        code += '  ' + 'f_' + format(BASE, 'x') + '(&c);\n'
        code += '''
  for(unsigned i=0;i<31;++i)assert(c.x[i]==expected[a][i]);
  assert(c.nzcv==expected[a][31]&&c.sp==0x7300080000ull&&c.x[30]==0x7000001000ull);
  assert(c.fpcr==0x1234&&c.fpsr==0x5678);
  for(unsigned i=0;i<sizeof(c.v);++i)assert(((unsigned char*)c.v)[i]==0xa5);
  assert(!memcmp((char*)p+0x2000,table,4));
 }
 puts("native entry/dispatch differential: PASS");return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='monado-native-entry-') as temporary:
            root = Path(temporary)
            harness = root / 'harness.c'; harness.write_text(code)
            for kind, entry_sets, returncode in [('old', (), 37), ('fixed', (hints,), 0)]:
                directory = root / kind
                generate(directory, entry_sets)
                binary = root / (kind + '-fixture')
                command = [compiler, '-std=gnu11', '-O1', '-UNDEBUG', '-ffp-contract=off',
                           '-fno-strict-aliasing', '-I', str(PORT / 'runtime'), str(harness)]
                if os.environ.get('MONADO_TEST_UBSAN') == '1':
                    command[1:1] = ['-fsanitize=undefined', '-fno-sanitize-recover=all']
                command += list(map(str, sorted(directory.glob('*.c')))) + ['-o', str(binary)]
                result = subprocess.run(command, capture_output=True, text=True, timeout=120)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
                self.assertEqual(result.returncode, returncode, result.stdout + result.stderr)
                self.assertIn('PASS' if returncode == 0 else 'missing authored dispatch', result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
