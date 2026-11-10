"""Authored native summary-hook contracts; no game input or rendered assets."""
from pathlib import Path
import importlib.util
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

PORT = Path(__file__).resolve().parents[1] / 'Monado_Linux'
sys.path.insert(0, str(PORT / 'tools/lifter'))
from lifter import Lifter  # noqa: E402
from patchfmt import PatchMismatch  # noqa: E402


def patches():
    spec = importlib.util.spec_from_file_location('summary_hooks', PORT / 'games/xcde/patches.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class Synthetic:
    def __init__(self, address, word):
        self.address, self.value, self.lifts = address, word, 0
        self.lifter = Lifter(0x7000000000, simd='c')

    def word(self, address):
        assert address == self.address
        return self.value

    def lift(self, address):
        self.lifts += 1
        return self.lifter.lift(self.value, address)


class SummaryHooks(unittest.TestCase):
    def setUp(self):
        self.module = patches()

    def emit(self, address):
        patch = self.module.PATCHES[address]
        return patch.apply(Synthetic(address, patch.expected), address)

    def test_all_anchors_reject_input_mismatch_before_lifting(self):
        expected = {0x70002391f0: 0xfc190fe8, 0x70002393a4: 0xf940baa8,
                    0x70002393b0: 0x52800508, 0x70002393cc: 0xf940bac9,
                    0x7000239420: 0x3945c3e8, 0x70002395d8: 0x910903ff}
        self.assertEqual(set(expected), set(self.module.QUICKSTEP_STATS_PATCHES))
        for address, word in expected.items():
            with self.subTest(address=hex(address)):
                patch = self.module.PATCHES[address]
                self.assertEqual(patch.expected, word)
                bad = Synthetic(address, word ^ 1)
                with self.assertRaises(PatchMismatch):
                    patch.apply(bad, address)
                self.assertEqual(bad.lifts, 0)
        self.assertEqual(set(self.module.QUICKSTEP_STATS_UI_PATCHES), {0x700023e700})
        cleanup = self.module.PATCHES[0x700023e700]
        self.assertEqual(cleanup.expected, 0xa9be4ff4)
        bad = Synthetic(0x700023e700, cleanup.expected ^ 1)
        with self.assertRaises(PatchMismatch):
            cleanup.apply(bad, 0x700023e700)
        self.assertEqual(bad.lifts, 0)

    def test_explicit_resume_and_native_cleanup_keep_generated_header_contract(self):
        row = self.emit(0x70002393b0)
        self.assertIn('goto L_70002393cc', row)
        self.assertIn('goto L_7000239508', row)
        self.assertNotIn('return', row)
        self.assertTrue(self.emit(0x70002393cc).startswith('L_70002393cc:;'))
        for address in self.module.QUICKSTEP_STATS_PATCHES:
            emitted = self.emit(address)
            self.assertNotIn('GUEST_LO', emitted)
            self.assertNotIn('GUEST_HI', emitted)

    @unittest.skipUnless(sys.platform == 'linux', 'native CPU-header C fixture')
    def test_emitted_code_count_order_frame_boundaries_and_row_dispatch(self):
        local = PORT / '.deps/bin/clang-cc'
        cc = os.environ.get('MONADO_TEST_CC') or (str(local) if local.is_file() else shutil.which('clang'))
        if not cc:
            self.skipTest('Clang is required for the native CPU header')
        begin, count, row, resume, compare, end = [self.emit(a) for a in
            (0x70002391f0, 0x70002393a4, 0x70002393b0, 0x70002393cc,
             0x7000239420, 0x70002395d8)]
        # All wrappers are authored. The emitted snippets contain only the
        # checked synthetic single instructions and runtime hook calls.
        source = r'''
#include "cpu.h"
#include <assert.h>
#include <string.h>
#include <stdint.h>
static uint64_t observed_begin, observed_end;
static int choice, original_copies, resumed, stopped;
void quickstep_stats_begin(Cpu *c) { observed_begin=c->sp; }
void quickstep_stats_end(Cpu *c) { observed_end=c->sp; }
void quickstep_stats_count(Cpu *c) { assert(c->x[8]==8); c->x[8]=9; }
int quickstep_stats_row(Cpu *c) { (void)c; return choice; }
void quickstep_stats_compare(Cpu *c) { c->x[24]=0; c->x[26]=0; }
static void entry(Cpu *c) { @BEGIN@ }
static void count_at_load(Cpu *c) { @COUNT@ }
static void compare_at_type(Cpu *c) { @COMPARE@ }
static void leave(Cpu *c) { @END@ }
static void dispatch(Cpu *c) {
  @ROW@
  ++original_copies;
  @RESUME@
  ++resumed;
  return;
L_7000239508:;
  ++stopped;
}
int main(void) {
  _Alignas(16) uint8_t memory[0x800]={0}, summary[0x200]={0};
  Cpu c={0}; uint64_t base=(uint64_t)(uintptr_t)memory;
  c.sp=base+0x400; c.v[8]=UINT64_C(0x3ff0000000000000);
  entry(&c);
  assert(observed_begin==base+0x400 && c.sp==base+0x390);
  assert(LD64(c.sp)==UINT64_C(0x3ff0000000000000));
  c.x[21]=(uint64_t)(uintptr_t)summary; ST64(c.x[21]+0x170,8);
  c.x[8]=123; count_at_load(&c);
  assert(c.x[8]==9 && LD64(c.x[21]+0x170)==8);
  c.x[22]=(uint64_t)(uintptr_t)summary;
  for(choice=-1;choice<=1;choice++) {
    original_copies=resumed=stopped=0; c.x[8]=777;
    uint64_t before=c.sp; dispatch(&c); assert(c.sp==before);
    assert(stopped==(choice<0)); assert(resumed==(choice>=0));
    assert(original_copies==(choice==0));
    if(choice==0)assert(c.x[8]==0x28);
    if(choice>=0)assert(c.x[9]==8);
  }
  ST8(c.sp+0x170,3); c.x[8]=999; c.x[24]=99; c.x[26]=99;
  compare_at_type(&c); assert(c.x[8]==3 && c.x[24]==0 && c.x[26]==0);
  uint64_t before=c.sp; leave(&c);
  assert(observed_end==before && c.sp==before+0x240);
  return 0;
}
'''
        for key, value in [('BEGIN', begin), ('COUNT', count), ('ROW', row),
                           ('RESUME', resume), ('COMPARE', compare), ('END', end)]:
            source = source.replace('@' + key + '@', value)
        with tempfile.TemporaryDirectory(prefix='monado-summary-hooks-') as temporary:
            path = Path(temporary) / 'authored.c'
            binary = Path(temporary) / 'authored'
            path.write_text(source)
            result = subprocess.run([cc, '-std=gnu11', '-O1', '-UNDEBUG',
                '-Wall', '-Wextra', '-Werror', '-ffp-contract=off',
                '-I', str(PORT / 'runtime'), str(path), '-o', str(binary)],
                capture_output=True, text=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
