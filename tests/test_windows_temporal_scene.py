"""Authored production temporal camera/grid behavior; no game/session control."""
import os
import ast
import json
from pathlib import Path
import re
import subprocess
import unittest
import uuid
from tests.native_renderer_support import WINDOWS,PORT,compiler_command,compiler_environment,graphics_prefix,native_flags,require_native_graphics
from tests.test_windows_renderer import extract

@unittest.skipUnless(WINDOWS,'Actual native Windows temporal contracts')
class NativeTemporalScene(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        require_native_graphics()
        cls.directory=PORT/'runs'/('temporal-scene-'+uuid.uuid4().hex);cls.directory.mkdir(parents=True)
        cls.environment=compiler_environment();cls.prefix=graphics_prefix()
        cls.environment['PATH']=str(cls.prefix/'bin')+os.pathsep+cls.environment.get('PATH','')
        source=(PORT/'runtime/nvn/vulkan/exec.c').read_text()
        temporal=(PORT/'runtime/nvn/vulkan/temporal.c').read_text()
        names=['vk_exec_forget_texture','tmp_exec_reset','tmp_cam_check','tmp_scene_begin_frame','tmp_scene_same_camera','tmp_scene_like','tmp_scene_pass_end','tmp_scene_record_draw','tmp_draw','tmp_spatial_fallback','post_process']
        body='\n'.join([extract(temporal,n) for n in ['halton','tmp_jitter']]+[extract(source,n) for n in names])
        (cls.directory/'temporal_scene_production.h').write_bytes(body.encode())
        cls.binary=cls.directory/'scene.exe'
        command=compiler_command()+native_flags()+['-I'+str(PORT/'runtime'),'-I'+str(PORT/'third_party'),'-I'+str(cls.prefix/'include'),'-I'+str(cls.directory),str(PORT/'tests/native/test_temporal_scene_windows.c'),'-o',str(cls.binary)]
        result=subprocess.run(command,env=cls.environment,capture_output=True,text=True,timeout=120)
        if result.returncode:raise AssertionError(result.stdout+result.stderr)
    def test_production_scene_camera_shadow_sky_hud_and_drs_grid(self):
        for flag in ['0','1']:
            with self.subTest(nojitter=flag):
                result=subprocess.run([str(self.binary),flag],env=self.environment,capture_output=True,text=True,timeout=30)
                self.assertEqual(result.returncode,0,result.stdout+result.stderr)
                self.assertIn('PASS production camera/scene',result.stdout)
    def test_actual_gpu_native_drs_changed_input_world_sky_hud_oracle(self):
        binary=self.directory/'grid-gpu.exe'
        source=(PORT/'runtime/nvn/vulkan/shaders.c').read_text()
        block=source.split('if (!strcmp(name, "tmp_hud.frag")) {',1)[1].split('if (!strcmp(name, "post.vert")) {',1)[0]
        literals=re.findall(r'"(?:\\.|[^"\\])*"',block)
        glsl=''.join(ast.literal_eval(value) for value in literals)
        (self.directory/'temporal_hud_production.h').write_bytes(('static const char production_hud_glsl[]='+json.dumps(glsl)+';\n').encode())
        command=compiler_command()+native_flags()+['-I'+str(self.prefix/'include'),'-I'+str(self.directory),str(PORT/'tests/native/test_temporal_grid_gpu_windows.c'),str(self.prefix/'lib/libvulkan-1.dll.a'),str(self.prefix/'lib/libshaderc_shared.dll.a'),'-o',str(binary)]
        result=subprocess.run(command,env=self.environment,capture_output=True,text=True,timeout=120)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        result=subprocess.run([str(binary)],env=self.environment,capture_output=True,text=True,timeout=60)
        (self.directory/'gpu.stdout').write_bytes(result.stdout.encode());(self.directory/'gpu.stderr').write_bytes(result.stderr.encode())
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertIn('old unscaled jitter counterexample detected',result.stdout)
    def test_production_temporal_dispatch_history_failures_and_bounded_evidence(self):
        source=(PORT/'runtime/nvn/vulkan/temporal.c').read_text()
        names=['env_int','prep_pipeline','mat_from_rows','mat_mul','mat_inv','history_invalidate','history_prepare','history_commit','res','tmp_upscale','tmp_provider_status','tmp_note_spatial_frame','tmp_diagnostics_frame']
        (self.directory/'temporal_dispatch_production.h').write_bytes('\n'.join(extract(source,name) for name in names).encode())
        binary=self.directory/'dispatch.exe'
        command=compiler_command()+native_flags()+['-I'+str(PORT/'runtime'),'-I'+str(PORT/'third_party'),'-I'+str(self.prefix/'include'),'-I'+str(self.directory),str(PORT/'tests/native/test_temporal_dispatch_windows.c'),'-o',str(binary)]
        result=subprocess.run(command,env=self.environment,capture_output=True,text=True,timeout=120)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        result=subprocess.run([str(binary)],env=self.environment,capture_output=True,text=True,timeout=30)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertIn('PASS production temporal descriptors/history',result.stdout)

if __name__=='__main__':unittest.main()
