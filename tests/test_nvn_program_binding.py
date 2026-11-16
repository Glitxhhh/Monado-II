"""Authored native shader binding and constant lifetime regressions; no game/GPU."""
from pathlib import Path
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

from native_renderer_support import PORT, WINDOWS, compiler_command, compiler_environment, binary_name, native_flags
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


@unittest.skipUnless(sys.platform in ('linux', 'win32'), 'native Linux or Windows NVN fixtures')
class NvnProgramBindingTests(unittest.TestCase):
    def compile_run(self, native, headers):
        compiler = compiler_command()
        if not compiler:
            self.skipTest('Clang is required for the native CPU header')
        with tempfile.TemporaryDirectory(prefix='monado-program-binding-') as temporary:
            folder = Path(temporary)
            for name, source in headers.items():
                (folder / name).write_text(source)
            binary = folder / binary_name('contracts')
            command = compiler + native_flags() + [
                       '-I', str(PORT / 'runtime'), '-I', str(PORT / 'runtime/nvn'), '-I', str(folder),
                       '-DMONADO_PROGRAM_CMDBUF_SOURCE="' + CMDBUF.as_posix() + '"',
                       str(PORT / 'tests/native' / native), '-o', str(binary)]
            result = subprocess.run(command, env=compiler_environment(), capture_output=True, text=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(binary)], cwd=temporary, capture_output=True, text=True, timeout=45)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('contracts: PASS', result.stdout)
            expected = {binary_name('contracts'), *headers}
            if WINDOWS:
                expected.add('contracts.pdb')
            self.assertEqual({p.name for p in folder.iterdir()}, expected)

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
        cache_build = function(source, 'get_program_')
        # Derive the forward declaration from the real implementation. Its
        # frozen bloom-mode argument must not be dropped by this fixture.
        declaration = cache_build[:cache_build.index('{')].strip() + ';\n'
        cache = declaration + function(source, 'get_program') + cache_build
        begin = source.index('static struct CsEnt')
        finish = source.index('g_cs_tab[256]', begin) + len('g_cs_tab[256];')
        compute = source[begin:finish] + '\n' + function(source, 'compute_program')
        self.compile_run('test_program_constants.c', {'program_cbuf_source.h': constants,
                         'program_cache_source.h': cache, 'program_compute_source.h': compute})


if __name__ == '__main__':
    unittest.main()
