"""Actual no-game GPU prep geometry oracle; compile-only mode never dispatches GPU."""
import ast
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import unittest
import uuid
from tests.native_renderer_support import WINDOWS,PORT,compiler_command,compiler_environment,graphics_prefix,native_flags
from tests.test_windows_renderer import extract

def shader_source(source,name,next_name):
    block=source.split('if (!strcmp(name, "'+name+'")) {',1)[1].split('if (!strcmp(name, "'+next_name+'")) {',1)[0]
    # GLSL is returned as concatenated C literals, interspersed with comments.
    block=re.sub(r'//[^\n]*','',block)
    return ''.join(ast.literal_eval(value) for value in re.findall(r'"(?:\\.|[^"\\])*"',block))

@unittest.skipUnless(WINDOWS,'Native Windows Vulkan geometry oracle')
class TemporalInputsNativeGPU(unittest.TestCase):
    def test_actual_production_prep_perspective_motion_jitter_depth_and_negative_viewport(self):
        output=PORT/'runs'/('temporal-inputs-gpu-'+uuid.uuid4().hex);output.mkdir()
        prefix=graphics_prefix();environment=compiler_environment();environment['PATH']=str(prefix/'bin')+os.pathsep+environment.get('PATH','')
        shaders=(PORT/'runtime/nvn/vulkan/shaders.c').read_text()
        # tmp_prep.frag is the last builtin before return NULL; remove the
        # terminator by taking its own brace-delimited production source block.
        prep=shaders.split('if (!strcmp(name, "tmp_prep.frag")) {',1)[1].split('\n    return NULL;',1)[0]
        prep=re.sub(r'//[^\n]*','',prep)
        prep=''.join(ast.literal_eval(value) for value in re.findall(r'"(?:\\.|[^"\\])*"',prep))
        post=shader_source(shaders,'post.vert','smaa_edge.frag') if 'if (!strcmp(name, "smaa_edge.frag")) {' in shaders else None
        if post is None:
            post_block=shaders.split('if (!strcmp(name, "post.vert")) {',1)[1].split('if (!strncmp(name, "smaa_", 5)) {',1)[0]
            post_block=re.sub(r'//[^\n]*','',post_block)
            post=''.join(ast.literal_eval(value) for value in re.findall(r'"(?:\\.|[^"\\])*"',post_block))
        gpu=(PORT/'tests/native/test_temporal_grid_gpu_windows.c').read_text().split('static void render(',1)[0]
        gpu=gpu.replace('#include "temporal_hud_production.h"','')
        # Reuse existing authored Vulkan allocation/compiler/graphics helpers;
        # fixture main/render/readback/oracles remain independent of that test.
        (output/'temporal_input_gpu_helpers.h').write_bytes(gpu.encode())
        viewport=extract((PORT/'runtime/nvn/vulkan/exec.c').read_text(),'set_viewport')
        production='static const char production_prep_glsl[]='+json.dumps(prep)+';\nstatic const char production_post_glsl[]='+json.dumps(post)+';\n'+viewport
        (output/'temporal_input_production.h').write_bytes(production.encode())
        binary=output/'temporal-inputs.exe'
        command=compiler_command()+native_flags()+['-I'+str(output),'-I'+str(PORT/'tests/native'),'-I'+str(prefix/'include'),
            str(PORT/'tests/native/test_temporal_inputs_gpu_windows.c'),str(prefix/'lib/libvulkan-1.dll.a'),str(prefix/'lib/libshaderc_shared.dll.a'),'-o',str(binary)]
        receipt={'command':command,'shader_sha256':hashlib.sha256(prep.encode()).hexdigest(),
            'post_vertex_sha256':hashlib.sha256(post.encode()).hexdigest(),'viewport_sha256':hashlib.sha256(viewport.encode()).hexdigest(),
            'fixture_sha256':hashlib.sha256((PORT/'tests/native/test_temporal_inputs_gpu_windows.c').read_bytes()).hexdigest(),
            'limits':['Authored perspective geometry only; no game, provider SDK or gameplay qualification.',
                'Float outputs preserve oracle precision; this checks production shader math rather than game allocation/view lifetime.',
                'No Vulkan validation-layer pass is claimed.'],'status':'compiling'}
        result=subprocess.run(command,env=environment,capture_output=True,text=True,timeout=120)
        (output/'compile.log').write_bytes((result.stdout+result.stderr).encode());receipt['compile_exit']=result.returncode
        if result.returncode:
            receipt['status']='failed-compile';(output/'receipt.json').write_bytes((json.dumps(receipt,indent=2)+'\n').encode())
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        if os.environ.get('MONADO_TEMPORAL_INPUT_COMPILE_ONLY')=='1':
            receipt['status']='compiled-GPU-not-run';(output/'receipt.json').write_bytes((json.dumps(receipt,indent=2)+'\n').encode())
            print('Native temporal inputs compiled only: '+str(output),flush=True)
            self.skipTest('GPU execution withheld while human game is active; compiled only')
        result=subprocess.run([str(binary)],env=environment,capture_output=True,text=True,timeout=90)
        (output/'native.log').write_bytes((result.stdout+result.stderr).encode());receipt['native_exit']=result.returncode
        receipt['status']='passed' if result.returncode==0 else 'failed';(output/'receipt.json').write_bytes((json.dumps(receipt,indent=2)+'\n').encode())
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertIn('PASS actual production temporal inputs:',result.stdout)
        print('Native temporal inputs evidence: '+str(output),flush=True)

if __name__=='__main__':unittest.main()
