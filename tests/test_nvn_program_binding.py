"""Authored native shader binding and constant lifetime regressions; no game/GPU."""
from pathlib import Path
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

PORT = Path(__file__).resolve().parents[1] / 'Monado_Linux'
CMDBUF = PORT / 'runtime/nvn/cmdbuf.c'
EXEC = PORT / 'runtime/nvn/vulkan/exec.c'


def function(source, name):
    match = re.search(r'(?m)^static [^\n]*\b' + re.escape(name) + r'\([^;]*?\)\s*\{', source)
    if not match:
        raise AssertionError('missing authored function ' + name)
    start = match.start()
    opening = source.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'


@unittest.skipUnless(sys.platform == 'linux', 'native Linux NVN fixtures')
class NvnProgramBindingTests(unittest.TestCase):
    def compile_run(self, native, headers):
        local = PORT / '.deps/bin/clang-cc'
        compiler = os.environ.get('MONADO_TEST_CC') or (str(local) if local.is_file() else shutil.which('clang'))
        if not compiler:
            self.skipTest('Clang is required for the native CPU header')
        with tempfile.TemporaryDirectory(prefix='monado-program-binding-') as temporary:
            folder = Path(temporary)
            for name, source in headers.items():
                (folder / name).write_text(source)
            binary = folder / 'contracts'
            command = [compiler, '-std=gnu11', '-O1', '-UNDEBUG', '-ffp-contract=off',
                       '-fno-strict-aliasing', '-Wall', '-Wextra', '-Werror',
                       '-Wno-unused-function', '-ffunction-sections', '-fdata-sections',
                       '-Wl,--gc-sections', '-pthread', '-I', str(PORT / 'runtime'),
                       '-I', str(PORT / 'runtime/nvn'), '-I', str(folder),
                       '-DMONADO_PROGRAM_CMDBUF_SOURCE="' + str(CMDBUF) + '"',
                       str(PORT / 'tests/native' / native), '-o', str(binary)]
            if os.environ.get('MONADO_TEST_UBSAN') == '1':
                command[1:1] = ['-fsanitize=undefined', '-fno-sanitize-recover=all']
            result = subprocess.run(command, capture_output=True, text=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(binary)], cwd=temporary, capture_output=True, text=True, timeout=45)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('contracts: PASS', result.stdout)
            self.assertEqual({p.name for p in folder.iterdir()}, {'contracts', *headers})

    def test_recording_and_independent_stage_bindings(self):
        source = EXEC.read_text()
        case = source[source.index('        case CMD_BIND_PROGRAM:'):source.index('        case CMD_BIND_BLEND:')]
        wrapper = 'static void execute_binding(CmdRec *r) { switch(r->op) {\n' + case + '\ndefault: abort(); } }\n'
        self.compile_run('test_program_binding.c', {'program_bind_case.h': wrapper})

    def test_pipeline_constants_and_cache_identity(self):
        source = EXEC.read_text()
        constants = function(source, 'cbuf_info')
        if 'const NvnShaderStage *shader' in constants:
            invocation = 'return cbuf_info(stage,si,shader,k,0,0,0);'
        else:  # Baseline regression runner: same expected contract, original source signature.
            invocation = '(void)shader; return cbuf_info(stage,si,k,0,0,0);'
        constants = function(source, 'ubo_span') + constants + '\nstatic VkDescriptorBufferInfo checked_cbuf(int stage, ShaderInfo *si, const NvnShaderStage *shader,int k){' + invocation + '}\n'
        cache = 'static Prog *get_program_(NvnProgram*,ShaderKey*,ShaderKey*);\n' + function(source, 'get_program') + function(source, 'get_program_')
        begin = source.index('static struct CsEnt')
        finish = source.index('g_cs_tab[256]', begin) + len('g_cs_tab[256];')
        compute = source[begin:finish] + '\n' + function(source, 'compute_program')
        self.compile_run('test_program_constants.c', {'program_cbuf_source.h': constants,
                         'program_cache_source.h': cache, 'program_compute_source.h': compute})


if __name__ == '__main__':
    unittest.main()
