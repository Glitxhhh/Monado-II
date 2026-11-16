"""Checked host imports: actual native reservations, production lifecycle and real GPU probe."""
import os
from pathlib import Path
import subprocess
import unittest
import uuid
from tests.native_renderer_support import WINDOWS,PORT,compiler_command,compiler_environment,graphics_prefix,native_flags,require_native_graphics

@unittest.skipUnless(WINDOWS,'Native guest host-memory import correctness')
class NativeHostImport(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        require_native_graphics()
        cls.directory=PORT/'runs'/('host-import-'+uuid.uuid4().hex);cls.directory.mkdir(parents=True)
        cls.environment=compiler_environment();cls.prefix=graphics_prefix()
        cls.environment['PATH']=str(cls.prefix/'bin')+os.pathsep+cls.environment.get('PATH','')
    def run_fixture(self,name,sources,libraries=(),args=(),allow_unavailable=False):
        binary=self.directory/(name+'.exe')
        command=compiler_command()+native_flags()+['-I'+str(self.prefix/'include'),*map(str,sources),*map(str,libraries),'-o',str(binary)]
        result=subprocess.run(command,env=self.environment,capture_output=True,text=True,timeout=120)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        result=subprocess.run([str(binary),*args],env=self.environment,capture_output=True,text=True,timeout=120)
        (self.directory/(name+'.stdout')).write_bytes(result.stdout.encode());(self.directory/(name+'.stderr')).write_bytes(result.stderr.encode())
        if result.returncode==77 and allow_unavailable:self.skipTest(result.stdout.strip())
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        return result.stdout
    def test_import_memory_selection_failures_and_cleanup(self):
        output=self.run_fixture('selection',[PORT/'tests/native/test_host_import.c',PORT/'runtime/nvn/vulkan/host_import.c'])
        self.assertIn('15 cases OK',output)
    def test_native_committed_extent_growth_reset_and_fail_closed(self):
        output=self.run_fixture('lifecycle',[PORT/'tests/native/test_host_import_windows.c'])
        self.assertIn('lifecycle/commit/failure checks PASS',output)
    def test_production_growth_boundary_precedes_command_state(self):
        source=(PORT/'runtime/nvn/vulkan/exec.c').read_text()
        self.assertEqual(source.count('gmem_prepare_command();'),1)
        body=source[source.index('static void exec_list('):]
        self.assertLess(body.index('gmem_prepare_command();'),body.index('CmdRec *r'))
        self.assertNotIn('gmem_prepare_command();',source[:source.index('static void exec_list(')])
        lifecycle=(PORT/'runtime/nvn/vulkan/host_import_windows.inc').read_text()
        prepare=lifecycle[lifecycle.index('static void gmem_prepare_command'):lifecycle.index('static void gmem_reset')]
        self.assertLess(prepare.index('renderer_flush();'),prepare.index('vk_host_import_destroy'))
        self.assertLess(prepare.index('vk_host_import_destroy'),prepare.index('gmem_import(want)'))
        info=lifecycle[lifecycle.index('static VkDescriptorBufferInfo gmem_info'):lifecycle.index('static void gmem_prepare_command')]
        self.assertNotIn('renderer_flush();',info);self.assertNotIn('vk_host_import_destroy',info)
        reset=source[source.index('void vk_exec_reset'):source.index('void vk_exec_reset')+1600]
        self.assertIn('gmem_reset();',reset)
    def test_actual_gpu_coherent_cpu_gpu_alias_growth_unpin_reset(self):
        output=self.run_fixture('gpu',[PORT/'tests/native/test_vulkan_import_windows.c',PORT/'runtime/nvn/vulkan/host_import.c'],
            [self.prefix/'lib/libvulkan-1.dll.a'],allow_unavailable=True)
        self.assertIn('real native Vulkan host import:',output)
    def test_production_renderer_drain_borrowed_lifetime_and_non_game_clear(self):
        output=self.run_fixture('device-lifecycle',[PORT/'tests/native/test_device_lifecycle_windows.c'])
        self.assertIn('queued ownership/reset/borrowed textures/non-game clear PASS',output)
        production=(PORT/'runtime/nvn/vulkan/device.c').read_text()
        self.assertEqual(production.count('#include "device_lifecycle_windows.inc"'),1)
    def test_accepted_bounded_aspect_transition_diagnostics(self):
        output=self.run_fixture('aspect-diagnostics',[PORT/'tests/native/test_aspect_diagnostics_windows.c'])
        self.assertIn('eligibility/change/bounds PASS',output)
        production=(PORT/'runtime/nvn/vulkan/device.c').read_text()
        self.assertIn('aspect_output_note(&os);',production)

if __name__=='__main__':unittest.main()
