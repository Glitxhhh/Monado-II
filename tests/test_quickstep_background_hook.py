"""Private background batch filter keeps the real native load/cleanup contract."""
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
    def __init__(self, word, instruction=None):
        self.value, self.instruction, self.calls = word, instruction, 0
        self.lifter = Lifter(0x7000000000, simd='c')

    def word(self, address):
        assert address == 0x700075e868
        return self.value

    def lift(self, address):
        self.calls += 1
        return self.instruction or self.lifter.lift(self.value, address)


class BackgroundHook(unittest.TestCase):
    def setUp(self):
        spec = importlib.util.spec_from_file_location('bonus_background_patch', PORT / 'games/xcde/patches.py')
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        self.patch = module.PATCHES[0x700075e868]

    def emit(self):
        return self.patch.apply(Generator(self.patch.expected), 0x700075e868)

    def test_word_and_plain_instruction_guard(self):
        self.assertEqual(self.patch.expected, 0xf941d268)
        generator = Generator(self.patch.expected ^ 1)
        with self.assertRaises(PatchMismatch):
            self.patch.apply(generator, 0x700075e868)
        self.assertEqual(generator.calls, 0)
        with self.assertRaises(ValueError):
            self.patch.apply(Generator(self.patch.expected, Insn('bl', target=0x700075e99c)), 0x700075e868)

    def test_existing_native_cleanup_is_used_without_early_return(self):
        code = self.emit()
        self.assertIn('goto L_700075e99c;', code)
        self.assertNotIn('return', code)
        self.assertNotIn('GUEST_', code)

    @unittest.skipUnless(sys.platform == 'linux', 'native CPU header')
    def test_foreign_batch_load_and_owned_batch_both_reach_cleanup(self):
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
static bool owned;
static unsigned cleanup,checked;
bool quickstep_native_filter_background(Cpu*c){
    assert(c->sp==BASE+0x8000&&c->x[19]==BASE+0x1000);++checked;return owned;
}
'''
        code += '\nstatic void draw(Cpu*c){' + self.emit() + r'''
    c->x[2]=1;
L_700075e99c:
    ++cleanup;c->sp+=0x40;
}
int main(void){
    assert(mmap((void*)BASE,0x10000,PROT_READ|PROT_WRITE,
        MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0)==(void*)BASE);
    ST64(BASE+0x1000+0x3a0,0xabcdef);
    for(unsigned i=0;i<2;++i){
        owned=i;Cpu c;memset(&c,0x67,sizeof c);c.sp=BASE+0x8000;c.x[19]=BASE+0x1000;
        Cpu expected=c;expected.sp+=0x40;
        if(!owned){expected.x[8]=0xabcdef;expected.x[2]=1;}
        draw(&c);assert(!memcmp(&c,&expected,sizeof c)&&cleanup==i+1&&checked==i+1);
    }
}
'''
        with tempfile.TemporaryDirectory(prefix='monado-private-background-') as temporary:
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
