"""Checked native row/menu presentation boundaries, using authored CPU fixtures."""
from pathlib import Path
import importlib.util
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
PORT = ROOT / 'Monado_Linux'
sys.path.insert(0, str(PORT / 'tools/lifter'))
from lifter import Insn, Lifter  # noqa: E402
from patchfmt import PatchMismatch  # noqa: E402


class Generator:
    def __init__(self, site, word, instruction=None):
        self.site, self.value, self.instruction = site, word, instruction
        self.lifter, self.calls = Lifter(0x7000000000, simd='c'), 0

    def word(self, address):
        assert address == self.site
        return self.value

    def lift(self, address):
        assert address == self.site
        self.calls += 1
        return self.instruction or self.lifter.lift(self.value, address)


class EligibilityHooks(unittest.TestCase):
    def setUp(self):
        spec = importlib.util.spec_from_file_location('bonus_eligibility_patches', PORT / 'games/xcde/patches.py')
        self.module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.module)

    def emit(self, site):
        patch = self.module.PATCHES[site]
        return patch.apply(Generator(site, patch.expected), site)

    def test_input_words_reject_before_emission(self):
        for site, word in ((0x7000316604, 0x97fbd29b), (0x70002fa178, 0xf941f7e8),
                           (0x70002f8cd8, 0x37003ac0), (0x70002f9970, 0x910b63e9),
                           (0x70002f9a3c, 0x910a43e0)):
            patch = self.module.PATCHES[site]
            self.assertEqual(patch.expected, word)
            generator = Generator(site, word ^ 1)
            with self.assertRaises(PatchMismatch):
                patch.apply(generator, site)
            self.assertEqual(generator.calls, 0)

    def test_original_row_call_target_and_menu_instruction_are_required(self):
        patch = self.module.PATCHES[0x7000316604]
        for instruction in (Insn('b', target=0x700020b070), Insn('bl', target=0x700020b074)):
            with self.assertRaises(ValueError):
                patch.apply(Generator(0x7000316604, patch.expected, instruction), 0x7000316604)
        patch = self.module.PATCHES[0x70002fa178]
        with self.assertRaises(ValueError):
            patch.apply(Generator(0x70002fa178, patch.expected, Insn('bl', target=0x700020b070)), 0x70002fa178)

    @unittest.skipUnless(sys.platform == 'linux', 'native CPU header')
    def test_compiled_call_order_link_register_and_original_stack_load(self):
        local = PORT / '.deps/bin/clang-cc'
        compiler = os.environ.get('MONADO_TEST_CC') or (str(local) if local.is_file() else shutil.which('clang'))
        if not compiler:
            self.skipTest('Clang is required for the native CPU header')
        code = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <stdbool.h>
#include <string.h>
#include <sys/mman.h>
#include "cpu.h"
#define BASE 0x7200000000ull
#define STACK (BASE+0x8000)
#define f_700020b070 authored_row_style
static bool bonus;
static unsigned stage,called;
void quickstep_native_picker_row_style(Cpu*c) {
    assert(c->sp==STACK&&stage==0);stage=1;
    if(bonus)c->x[1]=0;
}
void authored_row_style(Cpu*c) {
    assert(c->sp==STACK&&stage==1&&c->x[30]==0x7000316608ull);
    assert(c->x[1]==(bonus?0:1)&&c->x[0]==BASE+0x100);
    stage=2;++called;c->x[0]=0x9876;
}
void quickstep_native_picker_menu_item(Cpu*c) {
    assert(c->sp==STACK&&stage==0);stage=1;
    // Actual eligibility/owner rules are tested by the production C fixture.
    if(bonus)ST8(c->sp+0x2d4,1);
}
void quickstep_native_picker_menu_gate(Cpu*c) {
    assert(c->sp==STACK&&stage==0);stage=1;
    if(bonus)c->x[0]=0;
}
void quickstep_native_picker_menu_opened(Cpu*c) {
    assert(c->sp==STACK&&stage==0);stage=1;
}
'''
        code += '\nstatic void row_hook(Cpu*c){' + self.emit(0x7000316604) + '}\n'
        code += '\nstatic void menu_hook(Cpu*c){' + self.emit(0x70002fa178) + '}\n'
        code += '\nstatic void list_item_hook(Cpu*c){' + self.emit(0x70002f9970) + '}\n'
        code += '\nstatic void opened_hook(Cpu*c){' + self.emit(0x70002f9a3c) + '}\n'
        code += '\nstatic void gate_hook(Cpu*c){' + self.emit(0x70002f8cd8) + r'''
    c->x[2]=10;return;
L_70002f9430: c->x[2]=20;
}
'''
        code += r'''
int main(void){
    assert(mmap((void*)BASE,0x10000,PROT_READ|PROT_WRITE,
        MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0)==(void*)BASE);
    for(unsigned enabled=0;enabled<2;++enabled){
        bonus=enabled;Cpu c;memset(&c,0x6b,sizeof c);c.sp=STACK;
        c.x[0]=BASE+0x100;c.x[1]=1;stage=0;row_hook(&c);
        assert(stage==2&&c.sp==STACK&&c.x[0]==0x9876&&called==enabled+1);
        memset(&c,0x6b,sizeof c);c.sp=STACK;Cpu before=c;
        ST64(STACK+0x3e8,0x123456789abcdef0ull);ST8(STACK+0x2d4,0);
        stage=0;menu_hook(&c);before.x[8]=0x123456789abcdef0ull;
        assert(stage==1&&!memcmp(&before,&c,sizeof c));
        memset(&c,0x6b,sizeof c);c.sp=STACK;before=c;
        stage=0;opened_hook(&c);before.x[0]=STACK+0x290;
        assert(stage==1&&!memcmp(&before,&c,sizeof c));
        assert(LD8(STACK+0x2d4)==enabled);
        memset(&c,0x6b,sizeof c);c.sp=STACK;before=c;
        ST8(STACK+0x2d4,0);stage=0;list_item_hook(&c);before.x[9]=STACK+0x2d8;
        assert(stage==1&&!memcmp(&before,&c,sizeof c));
        assert(LD8(STACK+0x2d4)==enabled);
        memset(&c,0x6b,sizeof c);c.sp=STACK;c.x[0]=1;before=c;
        stage=0;gate_hook(&c);before.x[0]=bonus?0:1;before.x[2]=bonus?10:20;
        assert(stage==1&&!memcmp(&before,&c,sizeof c));
    }
}
'''
        with tempfile.TemporaryDirectory(prefix='monado-eligibility-hooks-') as temporary:
            source, binary = Path(temporary) / 'test.c', Path(temporary) / 'test'
            source.write_text(code)
            result = subprocess.run([compiler, '-std=gnu11', '-O1', '-UNDEBUG', '-Wall', '-Wextra', '-Werror',
                                     '-ffp-contract=off', '-fno-strict-aliasing', '-pthread', '-I', str(PORT / 'runtime'),
                                     str(source), '-o', str(binary)], capture_output=True, text=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
