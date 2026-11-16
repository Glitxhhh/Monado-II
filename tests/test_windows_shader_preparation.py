"""Native authored shader discovery/orchestration and storage failures; no guest/GPU work."""
import os
from pathlib import Path
import subprocess
import sys
import unittest
import uuid
from tests.native_renderer_support import PORT,WINDOWS,compiler_command,compiler_environment,graphics_prefix,native_flags,require_native_graphics

@unittest.skipUnless(WINDOWS,'Native Windows cache transactions')
class NativeShaderPreparation(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        require_native_graphics()
        cls.root=PORT/'runs'/('shader-native-'+uuid.uuid4().hex);cls.root.mkdir(parents=True)
        cls.environment=compiler_environment();cls.prefix=graphics_prefix()
        cls.environment['PATH']=str(cls.prefix/'bin')+os.pathsep+cls.environment.get('PATH','')
        resource=cls.root/'process-manifest.res';compiler=compiler_command()
        result=subprocess.run(compiler[:-1]+['rc','/I',str(PORT/'tools'),'/fo',str(resource),str(PORT/'tools/windows_utf8.rc')],env=cls.environment,capture_output=True,text=True,timeout=30)
        if result.returncode:raise AssertionError(result.stdout+result.stderr)
        cls.binaries={}
        for name,extra in [('precompile',['-lz']),('shader_cache',[str(cls.prefix/'lib/libshaderc_shared.dll.a'),str(cls.prefix/'lib/libvulkan-1.dll.a')]),('cache_commit',[])]:
            binary=cls.root/(name+'.exe');source=PORT/'tests/native'/('test_'+name+'_windows.c')
            command=compiler_command()+native_flags()+['-municode','-I'+str(cls.prefix/'include'),'-I'+str(PORT/'runtime'),str(source),str(resource),'-o',str(binary),'-L'+str(cls.prefix/'lib'),*extra,'-ladvapi32']
            result=subprocess.run(command,env=cls.environment,capture_output=True,text=True,timeout=120)
            if result.returncode:raise AssertionError(result.stdout+result.stderr)
            cls.binaries[name]=binary
    def execute(self,name,scenario):
        state=self.root/(scenario+'-'+uuid.uuid4().hex);state.mkdir()
        result=subprocess.run([str(self.binaries[name]),scenario,str(state)],env=self.environment,capture_output=True,text=True,encoding='utf8',errors='replace',timeout=30)
        (state/'fixture.stdout').write_bytes(result.stdout.encode());(state/'fixture.stderr').write_bytes(result.stderr.encode())
        return result,state
    def test_authored_discovery_bounds_metadata_and_mutations(self):
        for scenario in ['parser','allocator-fill','incidental-magic','recognized-bad-sections','blob-bounds','constants-overflow','metadata-overflow','constants-fallback','constants-short','unsupported-version','fuzz']:
            with self.subTest(scenario=scenario):
                result,_=self.execute('precompile',scenario);self.assertEqual(result.returncode,0,result.stdout+result.stderr)
    def test_real_threads_session_commit_and_failure_accounting(self):
        cases={'success':(0,'complete'),'compressed':(0,'complete'),'no-seed':(0,'complete'),'unsupported':(0,'complete'),
            'extra-failure':(2,'partial'),'seed-failure':(2,'partial'),'variant-failure':(2,'partial'),'cache-failure':(2,'partial'),
            'bad-variants':(2,'partial'),'thread-fail':(1,'failed'),'no-programs':(1,'failed'),
            'cancel':(130,'cancelled'),'early-cancel':(130,'cancelled'),'stale-cancel':(0,'complete')}
        for scenario,(code,status) in cases.items():
            with self.subTest(scenario=scenario):
                result,state=self.execute('precompile',scenario);self.assertEqual(result.returncode,code,result.stdout+result.stderr)
                raw=(state/'cache/precompile_work.txt').read_text(encoding='ascii')
                self.assertIn('state '+status+'\n',raw);self.assertIn('phase done\n',raw)
                self.assertIn('identity '+'a'*64+'\n',raw);self.assertIn('session '+'b'*32+'\n',raw)
                self.assertFalse((state/'cache/precompile_done.txt').exists());self.assertFalse((state/'cache/precompile_skip.txt').exists())
                if status=='complete':self.assertIn('failed 0\n',raw);self.assertIn('errors 0\n',raw)
    def test_unknown_status_and_invalid_identity_preserved(self):
        result,state=self.execute('precompile','unknown-status');self.assertEqual(result.returncode,1,result.stderr)
        self.assertEqual((state/'cache/precompile_work.txt').read_bytes(),b'private unknown')
        result,state=self.execute('precompile','invalid-identity');self.assertEqual(result.returncode,1,result.stderr)
        self.assertFalse((state/'cache').exists())
    def test_actual_shaderc_cold_warm_repair_storage_boundaries(self):
        for scenario in ['cold-warm','corrupt-repair','structure-repair','huge-record','sparse64','lock-failure','flush-failure','write-failure-render','loose-locked','loose-repair']:
            with self.subTest(scenario=scenario):
                result,_=self.execute('shader_cache',scenario);self.assertEqual(result.returncode,0,result.stdout+result.stderr)
    def test_atomic_pipeline_variant_commit_and_preservation(self):
        for scenario in ['normal','incomplete','get-failure','get-bound','flush-failure','write-failure','replace-failure','locked','hardlink','variants','variant-malformed','status-parser','pipeline-unknown','present-policy','variant-dimensions']:
            with self.subTest(scenario=scenario):
                result,_=self.execute('cache_commit',scenario);self.assertEqual(result.returncode,0,result.stdout+result.stderr)
    def test_unicode_long_literal_paths_status_cancel_commit(self):
        base=self.root/('雪 漢 $() `literal` '+uuid.uuid4().hex)
        state=base/('long-'+('x'*70))/('long-'+('y'*70))/('long-'+('z'*70));state.mkdir(parents=True)
        self.assertGreater(len(str(state)),260)
        for name,scenario in [('precompile','success'),('precompile','early-cancel'),('cache_commit','normal'),('shader_cache','cold-warm')]:
            leaf=state/(name+'-'+scenario);leaf.mkdir()
            result=subprocess.run([str(self.binaries[name]),scenario,str(leaf)],env=self.environment,capture_output=True,text=True,encoding='utf8',errors='replace',timeout=30)
            self.assertEqual(result.returncode,130 if scenario=='early-cancel' else 0,result.stdout+result.stderr)
    def test_descendant_junction_rejected_and_target_preserved(self):
        state=self.root/('junction-'+uuid.uuid4().hex);state.mkdir();target=self.root/('target-'+uuid.uuid4().hex);target.mkdir()
        (target/'sentinel').write_bytes(b'private preserve')
        linked=subprocess.run(['cmd','/d','/c','mklink','/J',str(state/'cache'),str(target)],capture_output=True,text=True)
        self.assertEqual(linked.returncode,0,linked.stdout+linked.stderr)
        result=subprocess.run([str(self.binaries['cache_commit']),'reparse',str(state)],env=self.environment,capture_output=True,text=True,timeout=30)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr);self.assertEqual((target/'sentinel').read_bytes(),b'private preserve')

if __name__=='__main__':unittest.main()
