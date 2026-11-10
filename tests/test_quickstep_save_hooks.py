"""Checked save lifecycle hooks with authored CPU states; no game/save data."""
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


class Synthetic:
    def __init__(self, address, word):
        self.address, self.value, self.lifts = address, word, 0
        self.lifter = Lifter(0x7000000000, simd='c')

    def word(self, address):
        assert address == self.address
        return self.value

    def lift(self, address):
        assert address == self.address
        self.lifts += 1
        return self.lifter.lift(self.value, address)


class SaveHooks(unittest.TestCase):
    def setUp(self):
        spec = importlib.util.spec_from_file_location('save_hooks', PORT / 'games/xcde/patches.py')
        self.module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.module)

    def test_all_save_hooks_check_original_words_before_emission(self):
        for address, (word, _emit, _note) in self.module.QUICKSTEP_SAVE_PATCHES.items():
            with self.subTest(address=hex(address)):
                patch = self.module.PATCHES[address]
                bad = Synthetic(address, word ^ 1)
                with self.assertRaises(PatchMismatch):
                    patch.apply(bad, address)
                self.assertEqual(bad.lifts, 0)
                self.assertTrue(patch.apply(Synthetic(address, word), address))

    def test_read_contract_is_at_callee_for_all_native_callers(self):
        # Load, Continue and speculative scans share the native read function.
        # Capture per-read identity there; adoption still belongs to decode.
        self.assertNotIn(0x7000398760, self.module.QUICKSTEP_SAVE_PATCHES)
        patch = self.module.PATCHES[0x7000087040]
        self.assertEqual(patch.expected, 0xd10503ff)
        emitted = patch.apply(Synthetic(0x7000087040, patch.expected), 0x7000087040)
        self.assertIn('quickstep_save_read_intent(c)', emitted)
        self.assertLess(emitted.index('quickstep_save_read_intent(c)'), emitted.index('c->sp ='))
        self.assertNotIn('return', emitted)

    def test_split_read_and_copy_sites_keep_actual_native_instructions(self):
        expected = {0x70005c4234: (0xb9402a88, 'quickstep_save_header_copy'),
                    0x700039a424: (0xaa1303e0, 'quickstep_save_cache_copy'),
                    0x70003992b4: (0xb9400a68, 'quickstep_save_selected_copy')}
        for address, (word, helper) in expected.items():
            with self.subTest(address=hex(address)):
                patch = self.module.PATCHES[address]
                self.assertEqual(patch.expected, word)
                emitted = patch.apply(Synthetic(address, word), address)
                self.assertIn(helper + '(c)', emitted)
                self.assertNotIn('return', emitted)
                self.assertNotIn('goto', emitted)

    def test_native_read_frame_keeps_pre_frame_arguments(self):
        cc = os.environ.get('MONADO_TEST_CC') or str(PORT / '.deps/bin/clang-cc')
        if not Path(cc).is_file():
            cc = shutil.which('clang')
        if not cc:
            self.skipTest('Clang is required for the native CPU header')
        patch = self.module.PATCHES[0x7000087040]
        emitted = patch.apply(Synthetic(0x7000087040, patch.expected), 0x7000087040)
        source = '''#include <assert.h>
#include <string.h>
#include "cpu.h"
static unsigned calls;
void quickstep_save_read_intent(Cpu*c) {
 assert(c->sp==0x7200004000ull); assert(c->x[0]==0x7200010000ull);
 assert(c->x[1]==98 && c->x[2]==0); ++calls;
}
static void read_entry(Cpu*c) { EMIT }
int main(void) { Cpu c; memset(&c,0,sizeof c);
 c.sp=0x7200004000ull;c.x[0]=0x7200010000ull;c.x[1]=98;c.x[2]=0;
 read_entry(&c);assert(calls==1&&c.sp==0x7200003ec0ull);
 assert(c.x[0]==0x7200010000ull&&c.x[1]==98&&c.x[2]==0);
 return 0; }
'''.replace('EMIT', emitted)
        with tempfile.TemporaryDirectory(prefix='monado-save-hook-') as temporary:
            path = Path(temporary)
            (path / 'fixture.c').write_text(source)
            built = subprocess.run([cc, '-std=gnu11', '-O1', '-UNDEBUG', '-ffp-contract=off',
                                    '-fno-strict-aliasing', '-I', str(PORT / 'runtime'),
                                    str(path / 'fixture.c'), '-o', str(path / 'fixture')],
                                   capture_output=True, text=True, timeout=120)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            result = subprocess.run([str(path / 'fixture')], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
