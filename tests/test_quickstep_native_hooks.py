"""Authored hook ABI regressions, using synthetic words and the real C lifter.

No game files, native generated source, save data or graphical runtime is used.
"""
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


def patch_module():
    spec = importlib.util.spec_from_file_location('quickstep_native_hook_patches', PORT / 'games/xcde/patches.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class SyntheticGen:
    def __init__(self, site, word, instruction=None):
        self.site, self.value, self.instruction = site, word, instruction
        self.lifter = Lifter(0x7000000000, simd='c')
        self.lifted = 0

    def word(self, address):
        assert address == self.site
        return self.value

    def lift(self, address):
        assert address == self.site
        self.lifted += 1
        return self.instruction or self.lifter.lift(self.value, address)


class QuickstepNativeHookTests(unittest.TestCase):
    def setUp(self):
        self.module = patch_module()

    def emit(self, site):
        patch = self.module.PATCHES[site]
        return patch.apply(SyntheticGen(site, patch.expected), site)

    def test_all_native_hook_words_are_checked_before_emission(self):
        # Native entry, mid-frame, store and tail-call anchors are different
        # contracts. Any input-version mismatch must fail before helper emission.
        expected = {
            0x70002f67f4: 0xf90e2674, 0x7000208d30: 0xf81e0ff3,
            0x7000208530: 0xf81e0ff3, 0x7000208590: 0xf81e0ff3,
            0x70002f6130: 0xf81b0ff9, 0x70002f87ac: 0xb9400288,
            0x7000183a60: 0xb94fb808, 0x7000241b40: 0xb9002001,
            0x7000302640: 0xd10643ff, 0x70003020d0: 0xf81d0ff5,
            0x70003004e0: 0xf81d0ff5, 0x70002ff870: 0xf81c0ffc,
            0x70002fae60: 0xd10103ff, 0x70002fe000: 0xa9bb67fc,
            0x70002ff5fc: 0x17fffd0d, 0x70002ffd90: 0x52914288,
            0x70003035d0: 0xd10603ff, 0x7000303270: 0xa9bd57f6,
            0x70003022b0: 0xa9bd57f6, 0x700017b81c: 0xb8296913,
            0x7000237898: 0xb943be68, 0x70002378d0: 0x6b00035f,
            0x7000237940: 0x6b00035f,
        }
        self.assertTrue(set(expected).issubset(self.module.QUICKSTEP_PATCHES))
        for site, (word, _emitter, _note) in self.module.QUICKSTEP_PATCHES.items():
            with self.subTest(site=hex(site)):
                patch = self.module.PATCHES[site]
                self.assertEqual(patch.expected, expected.get(site, word))
                generator = SyntheticGen(site, word ^ 1)
                with self.assertRaises(PatchMismatch):
                    patch.apply(generator, site)
                self.assertEqual(generator.lifted, 0)
                self.assertTrue(self.emit(site))

    def test_favourite_is_not_an_equipment_mutation_hook(self):
        self.assertNotIn(0x70000bdee0, self.module.PATCHES)
        self.assertIn(0x700017b81c, self.module.PATCHES)
        self.assertIn('quickstep_slot_native_equip', self.emit(0x700017b81c))

    def test_native_cleanup_is_not_replaced_by_an_early_c_return(self):
        emitted = self.emit(0x70002f87ac)
        self.assertIn('goto L_70002f9430;', emitted)
        self.assertNotIn('return', emitted)
        self.assertIn('quickstep_native_input_event(c)', emitted)

    def test_callback_emits_require_only_the_generated_cpu_contract(self):
        for site in (0x7000303270, 0x70003022b0):
            emitted = self.emit(site)
            self.assertIn('quickstep_native_picker_cancel_callback(c)', emitted)
            self.assertNotIn('GUEST_LO', emitted)
            self.assertNotIn('GUEST_HI', emitted)
            self.assertNotIn('LD64', emitted)

    def test_hook_kind_and_native_tail_target_are_required(self):
        for site in (0x70002f67f4, 0x70002f87ac, 0x7000303270,
                     0x70003022b0, 0x7000302640):
            patch = self.module.PATCHES[site]
            generator = SyntheticGen(site, patch.expected, Insn('bl', target=0x7000001000))
            with self.subTest(site=hex(site)), self.assertRaises(ValueError):
                patch.apply(generator, site)
        site = 0x70002ff5fc
        patch = self.module.PATCHES[site]
        for instruction in (Insn('bl', target=0x70002fea30),
                            Insn('b', target=0x70002fea34)):
            with self.assertRaises(ValueError):
                patch.apply(SyntheticGen(site, patch.expected, instruction), site)

    @unittest.skipUnless(sys.platform == 'linux', 'execute emitted Linux C bridge')
    def test_compiled_emits_preserve_frames_stores_bounds_and_tail_lr(self):
        local = PORT / '.deps/bin/clang-cc'
        compiler = os.environ.get('MONADO_TEST_CC') or (str(local) if local.is_file() else shutil.which('clang'))
        if not compiler:
            self.skipTest('Clang is required for the native CPU header')
        wrappers = {
            'character': 0x7000303270, 'pane': 0x70003022b0,
            'construct': 0x70002f67f4, 'destroy': 0x7000208530,
            'deleting_destroy': 0x7000208590, 'sequence_destroy': 0x70002f6130,
            'socket_write': 0x700017b81c, 'close_tail': 0x70002ff5fc,
            'ui_tick': 0x7000208d30, 'panel_destroy': 0x700023e700,
        }
        code = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "cpu.h"
#define AUTHORED_LOW 0x7000000000ull
#define AUTHORED_HIGH 0x7320000000ull
#define BASE 0x7200000000ull
#define SEQ (BASE+0x1000)
#define UI (BASE+0xa000)
#define CALLBACK (BASE+0xb000)
#define PANEL (BASE+0xe000)
#define STACK (BASE+0x1f000)
#define f_70002fea30 authored_close_tail
static unsigned stage,cancel_calls,construct_calls,destroy_calls,sequence_calls,tail_calls;
static unsigned close_calls,after_tail;
static unsigned cleanup_stage;
static bool consumed;
static uint64_t expected_pointer;
void quickstep_native_picker_cancel_callback(Cpu*c) {
    // The real runtime adapter's bounds are tested by test_quickstep_picker.c.
    // This spy verifies generated code always reaches it before frame mutation.
    assert(c->sp==STACK&&stage==0);stage=1;++cancel_calls;
}
void quickstep_native_construct(Cpu*c) {
    assert(c->x[19]==SEQ&&c->x[20]==UI&&LD64(SEQ+0x1c48)==UI);++construct_calls;
}
void quickstep_native_release(Cpu*c) {
    assert(c->sp==STACK&&c->x[0]==UI&&cleanup_stage==1);cleanup_stage=2;++destroy_calls;
}
void quickstep_native_release_sequence(Cpu*c) {
    assert(c->sp==STACK&&c->x[0]==SEQ&&cleanup_stage==1);cleanup_stage=2;++sequence_calls;
}
void quickstep_stats_ui_release_ui(Cpu*c) {
    assert(c->sp==STACK&&c->x[0]==UI&&cleanup_stage==0);cleanup_stage=1;
}
void quickstep_stats_ui_release_sequence(Cpu*c) {
    assert(c->sp==STACK&&c->x[0]==SEQ&&cleanup_stage==0);cleanup_stage=1;
}
void quickstep_stats_ui_release_panel(Cpu*c) {
    assert(c->sp==STACK&&c->x[0]==PANEL&&cleanup_stage==0);cleanup_stage=1;
}
void quickstep_native_tick(Cpu*c) {
    assert(c->sp==STACK&&c->x[0]==UI&&cleanup_stage==0);cleanup_stage=1;
}
void quickstep_stats_ui_tick(Cpu*c) {
    assert(c->sp==STACK&&c->x[0]==UI&&cleanup_stage==1);cleanup_stage=2;
}
void quickstep_slot_native_equip(Cpu*c) {
    assert(c->x[8]+c->x[9]==expected_pointer&&LD32(expected_pointer)==0x12345678);
    assert(stage==0);stage=1;
}
bool quickstep_native_input_event(Cpu*c) {
    assert(c->sp==STACK-0x4b0);return consumed;
}
void quickstep_native_picker_closed(Cpu*c) {
    assert(c->sp==STACK&&c->x[0]==SEQ&&c->x[30]==0x1122334455667788ull);
    assert(stage==0);stage=1;++close_calls;
}
void authored_close_tail(Cpu*c) {
    assert(stage==1&&c->sp==STACK&&c->x[0]==SEQ);
    assert(c->x[30]==0x1122334455667788ull);stage=2;++tail_calls;
}
'''
        for name, site in wrappers.items():
            suffix = '++after_tail;' if name == 'close_tail' else ''
            code += f'\nstatic void {name}(Cpu*c) {{ {self.emit(site)} {suffix} }}\n'
        code += f'''
static void native_input_frame(Cpu*c) {{
    {self.emit(0x70002f87ac)}
    c->x[0]=0;
    goto L_cleanup;
L_70002f9430:
    c->x[0]=1;
L_cleanup:
    c->sp+=0x4b0;
}}
'''
        code += r'''
static Cpu cpu(void) {Cpu c;memset(&c,0x67,sizeof c);c.sp=STACK;cleanup_stage=0;return c;}
int main(void) {
    assert(mmap((void*)BASE,0x20000,PROT_READ|PROT_WRITE,
        MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0)==(void*)BASE);
    assert(mmap((void*)AUTHORED_LOW,0x1000,PROT_READ|PROT_WRITE,
        MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0)==(void*)AUTHORED_LOW);
    assert(mmap((void*)(AUTHORED_HIGH-0x1000),0x1000,PROT_READ|PROT_WRITE,
        MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0)==(void*)(AUTHORED_HIGH-0x1000));
    ST64(CALLBACK+0x18,SEQ);ST64(AUTHORED_LOW+0x18,SEQ);ST64(AUTHORED_HIGH-8,SEQ);
    // Delegate every raw callback pointer to the checked runtime adapter before
    // either callback's real STP prologue; generated code has only cpu.h.
    void(*callbacks[])(Cpu*)={character,pane};
    for(unsigned i=0;i<2;i++) {
        uint64_t pointers[]={CALLBACK,AUTHORED_LOW,AUTHORED_HIGH-0x20,0,AUTHORED_LOW-1,AUTHORED_HIGH-0x1f,AUTHORED_HIGH,UINT64_MAX};
        for(unsigned p=0;p<sizeof pointers/sizeof pointers[0];p++) {
            Cpu c=cpu();c.x[0]=pointers[p];stage=0;unsigned old=cancel_calls;
            callbacks[i](&c);
            assert(c.sp==STACK-0x30&&LD64(c.sp)==c.x[22]&&LD64(c.sp+8)==c.x[21]);
            assert(cancel_calls-old==1&&stage==1);
        }
    }
    // Constructor hook follows the original publication store. Destruction
    // hooks run while native resource and caller frame identities still exist.
    Cpu c=cpu();c.x[19]=SEQ;c.x[20]=UI;construct(&c);assert(construct_calls==1);
    c=cpu();c.x[0]=UI;destroy(&c);assert(c.sp==STACK-0x20);
    c=cpu();c.x[0]=UI;deleting_destroy(&c);assert(c.sp==STACK-0x20&&destroy_calls==2);
    c=cpu();c.x[0]=SEQ;sequence_destroy(&c);assert(c.sp==STACK-0x50&&sequence_calls==1);
    c=cpu();c.x[0]=UI;ui_tick(&c);assert(c.sp==STACK-0x20&&cleanup_stage==2);
    c=cpu();c.x[0]=PANEL;panel_destroy(&c);assert(c.sp==STACK-0x20&&cleanup_stage==1);
    // The accepted socket mutation retains its actual native indexed STR;
    // favourite, quantity and all adjacent bytes are unchanged.
    c=cpu();expected_pointer=BASE+0xc000;c.x[8]=expected_pointer-16;c.x[9]=16;c.x[19]=0x3002a;
    ST32(expected_pointer-4,0xabcdef01);ST32(expected_pointer,0x12345678);ST32(expected_pointer+4,0x23456789);
    Cpu before=c;stage=0;socket_write(&c);assert(stage==1&&!memcmp(&c,&before,sizeof c));
    assert(LD32(expected_pointer)==0x3002a&&LD32(expected_pointer-4)==0xabcdef01&&LD32(expected_pointer+4)==0x23456789);
    // A consumed input bypasses the replaced LDR but still reaches the handled
    // native epilogue. A C return would leave SP at STACK-0x4b0 and fail here.
    for(unsigned take=0;take<2;take++) {
        c=cpu();c.sp-=0x4b0;c.x[20]=BASE+0xd000;ST32(c.x[20],0x81234567);c.x[8]=0xdeadbeef;
        consumed=take;native_input_frame(&c);assert(c.sp==STACK&&c.x[0]==take);
        assert(c.x[8]==(take?0xdeadbeef:0x81234567));
    }
    // The close hook is after the native frame restoration and tail-calls with
    // its original caller LR, rather than synthesizing a BL return address.
    c=cpu();c.x[0]=SEQ;c.x[30]=0x1122334455667788ull;before=c;stage=0;close_tail(&c);
    assert(!memcmp(&c,&before,sizeof c)&&stage==2&&close_calls==1&&tail_calls==1&&!after_tail);
    puts("checked native hook frames, stores, bounds and tail LR: PASS");
}
'''
        with tempfile.TemporaryDirectory(prefix='monado-quickstep-hooks-') as temporary:
            source = Path(temporary) / 'hooks.c'
            binary = Path(temporary) / 'hooks-test'
            source.write_text(code)
            command = [compiler, '-std=gnu11', '-O1', '-UNDEBUG', '-ffp-contract=off',
                       '-fno-strict-aliasing', '-Wall', '-Wextra', '-Werror', '-pthread',
                       '-I', str(PORT / 'runtime'), str(source), '-o', str(binary)]
            result = subprocess.run(command, capture_output=True, text=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('tail LR: PASS', result.stdout)


if __name__ == '__main__':
    unittest.main()
