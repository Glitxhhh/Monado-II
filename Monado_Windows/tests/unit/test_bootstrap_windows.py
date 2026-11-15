"""Authored portable-builder contracts; never use game input or human profiles.

Native PE companions are compiled from the tiny source below. Build orchestration
fixtures replace the expensive game compiler boundary, while exercising actual
manifests, native file guards, PE closure checks, input checks, junction recovery,
registry activation, process jobs and progress. They are component evidence only.
"""
from __future__ import annotations
import argparse
import copy
import ctypes
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import threading
import time
import types
import unittest
from unittest.mock import patch
import uuid

PORT=Path(__file__).resolve().parents[2]
ROOT=PORT.parent
spec=importlib.util.spec_from_file_location('bootstrap_contract',PORT/'tools/bootstrap_windows.py')
B=importlib.util.module_from_spec(spec);spec.loader.exec_module(B)


def sha(data): return hashlib.sha256(data).hexdigest()

def shader_work(current_session,current_identity,**changes):
    fields=dict(session=current_session,identity=current_identity,phase='done',state='complete',gpu_cache_uuid='c'*32,
        files_total=1,files_done=1,shaders=1,programs=1,programs_done=1,seed_total=0,seed_done=0,variants_total=0,variants_done=0,
        attempted=1,built=1,failed=0,skipped=0,unsupported=0,errors=0,gpu_vendor=1,gpu_device=2,gpu_driver=3)
    fields.update(changes)
    return ('monado_precompile_work 1\n'+''.join(str(k)+' '+str(v)+'\n' for k,v in fields.items())).encode('ascii')


class PureContracts(unittest.TestCase):
    def test_manifest_paths_and_aliases(self):
        for path in ('../x','/x','a//b','a/../b','a\\b','a:b','a.','a ','CON.txt','x/NUL','x\x00y'):
            with self.subTest(path=path),self.assertRaises(B.BootstrapError): B.relative(path)
        self.assertEqual(str(B.relative('Unicode 測試/file.py')),'Unicode 測試/file.py')
        value={'schema':1,'platform':'windows-amd64','files':{'a':{'bytes':1,'sha256':'a'*64},'A':{'bytes':1,'sha256':'b'*64}}}
        with self.assertRaises(B.BootstrapError):B.file_table(value)
        value['files']={'a':{'bytes':1,'sha256':'a'*64},'a/b':{'bytes':1,'sha256':'b'*64}}
        with self.assertRaises(B.BootstrapError):B.file_table(value)

    def test_bounded_json_and_typed_identity(self):
        for raw in (b'[]',b'{"a":1,"a":2}',b'{"a":NaN}',b'x',b' '* (B.MAX_JSON+1)):
            with self.subTest(raw=raw[:20]),self.assertRaises(B.BootstrapError):B.decode(raw)
        for size in (-1,True,'1',513*1024*1024):
            with self.assertRaises(B.BootstrapError):
                B.file_table({'schema':1,'platform':'windows-amd64','files':{'a':{'bytes':size,'sha256':'a'*64}}})

    def test_overlap_rejected(self):
        base=PORT/'runs/fixture-overlap'
        for other in (base,base/'inside',base.parent):
            with self.assertRaises(B.BootstrapError):B.disjoint(base,other)
        B.disjoint(base,base.with_name('different'))

    def test_shader_status_requires_exact_completed_current_session(self):
        session='a'*32;identity='b'*64
        self.assertEqual(B.shader_status(shader_work(session,identity),session,identity)['built'],1)
        for change in ({'session':'0'*32},{'identity':'0'*64},{'state':'partial'},{'phase':'scan'},{'failed':1},{'skipped':1},{'errors':1},
                       {'files_done':0},{'programs_done':2},{'seed_done':1},{'variants_done':1},{'built':2**65},{'built':'-1'},{'unknown':1}):
            with self.subTest(change=change),self.assertRaises(B.BootstrapError):B.shader_status(shader_work(session,identity,**change),session,identity)
        for raw in (b'','x'.encode()*4097,shader_work(session,identity)+b'state complete\n',shader_work(session,identity)[:-1]):
            with self.assertRaises(B.BootstrapError):B.shader_status(raw,session,identity)


@unittest.skipUnless(os.name=='nt','native Windows authored builder contracts')
class NativeContracts(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.evidence=PORT/'runs'/('bootstrap-'+str(time.time_ns())+'-'+uuid.uuid4().hex[:8])
        cls.evidence.mkdir(parents=True)
        source=cls.evidence/'authored-companion.c'
        source.write_text('#include <windows.h>\nint main(int argc,char **argv){(void)argv;if(argc>1)Sleep(30000);return 0;}\n')
        cls.exe=cls.evidence/'authored-companion.exe'
        env=dict(os.environ,ZIG_GLOBAL_CACHE_DIR=str(PORT/'.deps/zig-global-cache'),ZIG_LOCAL_CACHE_DIR=str(cls.evidence/'zig-cache'))
        argv=[sys.executable,'-m','ziglang','cc','-O1','-UNDEBUG','-march=x86_64_v2','-ffp-contract=off',str(source),'-o',str(cls.exe)]
        result=subprocess.run(argv,env=env,capture_output=True,text=True,timeout=120)
        (cls.evidence/'compile.log').write_text(result.stdout+result.stderr)
        if result.returncode:raise RuntimeError('Authored fixture compile failed: '+result.stderr)
        cls.exe_bytes=cls.exe.read_bytes()
        provider=cls.evidence/'authored-provider.c'
        provider.write_text('\n'.join('__declspec(dllexport) int '+name+'(void){return 0;}' for name in
            ('ffxCreateContext','ffxDestroyContext','ffxDispatch','ffxQuery','ffxConfigure'))+'\n')
        binary=cls.evidence/'authored-provider.dll'
        result=subprocess.run([sys.executable,'-B','-m','ziglang','cc','-shared','-O1',str(provider),'-o',str(binary)],env=env,capture_output=True,text=True,timeout=120)
        (cls.evidence/'provider-compile.log').write_text(result.stdout+result.stderr)
        if result.returncode:raise RuntimeError('Authored provider compile failed: '+result.stderr)
        cls.provider_bytes=binary.read_bytes()
        cls.results=[]

    @classmethod
    def tearDownClass(cls):
        (cls.evidence/'receipt.json').write_text(json.dumps({'scope':'authored native builder components; no game build/input/profile',
            'sources':{str(p.relative_to(ROOT)):sha(p.read_bytes()) for p in [PORT/'tools/bootstrap_windows.py',Path(__file__),ROOT/'tools/windows_file_guard.py',ROOT/'tools/update_windows_release.py',ROOT/'tools/update_windows_retention.py',PORT/'scripts/build_windows.py']},
            'cases':cls.results},indent=2)+'\n')
        print('Native builder evidence: '+str(cls.evidence),flush=True)

    def setUp(self):
        self.folder=self.evidence/(self._testMethodName+'-'+uuid.uuid4().hex[:8]);self.folder.mkdir()
        self.cache=self.folder/('c'*64);self.cache.mkdir()
        self.input=self.folder/'authored extracted 測試';self.input.mkdir()
        exefs=self.input/'exefs';romfs=self.input/'romfs';exefs.mkdir();romfs.mkdir()
        self.expected={};modules=[]
        for n,name in enumerate(('main','sdk','rtld','subsdk0'),1):
            raw=bytearray(0x80);raw[:4]=b'NSO0';raw[0x40:0x48]=bytes([n])*8
            (exefs/name).write_bytes(raw);modules.append({'name':name,'build_id':(bytes([n])*8).hex()})
            if name in ('main','sdk'):self.expected[name]=sha(raw)
        (exefs/'main.npdm').write_bytes(b'authored metadata')
        (romfs/'authored.txt').write_bytes(b'authored only')
        size=len(b'authored only');manifest={'dirs':{'.':[1,size]},'files':1,'bytes':size,'sha256':sha(f'authored.txt\t{size}'.encode())}
        self.profile={'exefs_required':['main','sdk','rtld','subsdk0','main.npdm'],'modules':modules,'romfs_manifest':manifest}
        files={
            'setup/bootstrap_windows.py':(PORT/'tools/bootstrap_windows.py').read_bytes(),
            'setup/windows_file_guard.py':(ROOT/'tools/windows_file_guard.py').read_bytes(),
            'setup/update_windows_release.py':(ROOT/'tools/update_windows_release.py').read_bytes(),
            'setup/update_windows_retention.py':(ROOT/'tools/update_windows_retention.py').read_bytes(),
            'setup/update_release.py':(ROOT/'tools/update_release.py').read_bytes(),
            'setup/publication_guard.py':(ROOT/'tools/publication_guard.py').read_bytes(),
            'source/Monado_Windows/scripts/build_windows.py':(PORT/'scripts/build_windows.py').read_bytes(),
            'source/Monado_Windows/tools/windows_provider_selection.py':(PORT/'tools/windows_provider_selection.py').read_bytes(),
            'source/Monado_Windows/tools/pe_symbols_windows.py':(PORT/'tools/pe_symbols_windows.py').read_bytes(),
            'source/tools/windows_file_guard.py':(ROOT/'tools/windows_file_guard.py').read_bytes(),
            'source/publication-manifest.json':B.encoded({'schema':1,'scope':'authored source publication boundary; no game code'}),
            'source/Monado_Windows/games/xcde/profile.json':B.encoded(self.profile),
            'python/python.exe':self.exe_bytes,
            'native/ucrt64/authored-marker.txt':b'authored dependency boundary; no real game build',
        }
        source_files={name[7:]:{'bytes':len(data),'sha256':sha(data)} for name,data in files.items() if name.startswith('source/')}
        files['source-manifest.json']=B.encoded({'schema':1,'platform':'windows-amd64','files':source_files})
        for name,data in files.items():
            path=self.cache/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes(data)
        raw=B.encoded({'schema':1,'platform':'windows-amd64','files':{name:{'bytes':len(data),'sha256':sha(data)} for name,data in files.items()}})
        (self.cache/'payload-manifest.json').write_bytes(raw);self.manifest_hash=sha(raw)
        self.backend=None
        self.patch=patch.dict(B.EXPECTED_NSO,self.expected,clear=True);self.patch.start()
        self.addCleanup(self.patch.stop)
        self.addCleanup(self.close)

    def tearDown(self):
        result=getattr(getattr(self,'_outcome',None),'result',None)
        failures=(result.failures+result.errors) if result is not None else [(self,'missing result')]
        self.results.append({'case':self._testMethodName,'directory':self.folder.name,
                             'completed':not any(test is self for test,_ in failures)})

    def close(self):
        if self.backend:
            self.backend.close();self.backend=None

    def backend_open(self,action='resume',destination=None):
        self.close();state=self.cache/'state';token=uuid.uuid4().hex
        args=argparse.Namespace(cache=self.cache,payload_sha256=self.cache.name,manifest_sha256=self.manifest_hash,
            progress=state/('progress-'+token+'.jsonl'),cancel_file=state/('cancel-'+token),action=action,
            input=self.input,destination=destination,jobs=2,precompile=False)
        self.backend=B.Backend(args);self.backend.verify();return self.backend

    def make_install(self,path):
        import _winapi
        path.mkdir(parents=True);(path/'build').mkdir();(path/'targets').mkdir()
        for name in ('xcde.exe','xcde_launcher.exe'):(path/'build'/name).write_bytes(self.exe_bytes)
        for name in ('exefs','romfs'):_winapi.CreateJunction(str(self.input/name),str(path/'targets'/name))
        exefs={name:{'bytes':(self.input/'exefs'/name).stat().st_size,'sha256':self.expected[name]} for name in self.expected}
        record={'schema':1,'status':'success','outputs':{f'build/{n}':sha(self.exe_bytes) for n in ('xcde.exe','xcde_launcher.exe')},
                'input':{'path':str(self.input),'exefs':exefs},'installation':{'input_links':{n:str(self.input/n) for n in ('exefs','romfs')}}}
        record['retention_files']={name:{'bytes':len(self.exe_bytes),'sha256':digest} for name,digest in record['outputs'].items()}
        (path/'build-receipt.json').write_bytes(B.encoded(record));return record

    def fake_build(self,argv,*,cwd,phase,output=None):
        self.assertNotIn('--launch-current',list(map(str,argv)))
        if output:
            self.assertIn('--split-game',argv);record=self.make_install(output)
            choices={kind:argv[argv.index('--'+kind)+1] for kind in ('fidelityfx','streamline') if '--'+kind in argv}
            if choices:
                staged=self.backend.provider_module().stage(output,**choices)
                record['providers']=staged
                record['outputs'].update({name:item['sha256'] for name,item in staged['files'].items()})
                record['retention_files'].update(staged['files'])
                (output/'build-receipt.json').write_bytes(B.encoded(record))
        self.backend.progress.emit(phase,'Authored compiler boundary completed',done=1,total=1)

    def build_managed(self):
        destination=self.folder/'chosen install 測試'
        backend=self.backend_open('build',destination);backend.run_child=self.fake_build;backend.build()
        return backend.registry['installations'][backend.registry['last']]

    def local_provider(self):
        root=self.folder/'local MIT provider fixture 測試';root.mkdir()
        (root/'amd_fidelityfx_vk.dll').write_bytes(self.provider_bytes)
        (root/'LICENSE.txt').write_text('Authored synthetic DLL fixture, no vendor execution.\n')
        return root

    def test_selected_local_provider_is_receipted_persisted_and_reopened(self):
        root=self.local_provider();backend=self.backend_open('build',self.folder/'provider installation')
        backend.args.fidelityfx=root;backend.run_child=self.fake_build;backend.build();record=backend.selected()
        runtime=Path(record['root']);receipt=json.loads((runtime/'build-receipt.json').read_text())
        self.assertEqual(record['providers'],receipt['providers']['selection'])
        self.assertEqual((runtime/'build/amd_fidelityfx_vk.dll').read_bytes(),self.provider_bytes)
        self.close();backend=self.backend_open();backend.resume()
        last=json.loads(Path(backend.args.progress).read_text().splitlines()[-1])
        self.assertEqual(last['provider_fidelityfx'],str(root));self.assertEqual(last['launcher_args'],['--profile-root',record['profile']])

    def test_provider_apply_builds_fresh_runtime_and_preserves_profile(self):
        initial=self.build_managed();profile=Path(initial['profile']);before=(profile/'build-receipt.json').read_bytes()
        (profile/'human-save').write_bytes(b'preserve existing profile')
        self.backend.args.fidelityfx=self.local_provider();commands=[]
        def compile(argv,**kwargs):commands.append(list(argv));self.fake_build(argv,**kwargs)
        self.backend.run_child=compile;self.backend.select_providers();selected=self.backend.selected()
        self.assertNotEqual(selected['root'],initial['root']);self.assertEqual(selected['profile'],initial['profile'])
        self.assertTrue(selected['retention_authorities']);self.assertEqual((profile/'build-receipt.json').read_bytes(),before)
        self.assertFalse((profile/'build/amd_fidelityfx_vk.dll').exists());self.assertEqual((profile/'human-save').read_bytes(),b'preserve existing profile')
        self.assertIn('--fidelityfx',commands[0]);self.assertEqual(Path(commands[0][commands[0].index('--fidelityfx')+1]),self.backend.args.fidelityfx)

    def test_provider_missing_changed_or_failed_activation_preserves_selection(self):
        initial=self.build_managed();before=self.backend.state_guard.read_regular('checked-installations.json')
        root=self.local_provider();self.backend.args.fidelityfx=root
        original_put=B.put_json
        def refuse(guard,name,value):
            if name=='checked-installations.json':raise OSError('authored provider activation lock')
            return original_put(guard,name,value)
        with patch.object(B,'put_json',side_effect=refuse):
            with self.assertRaisesRegex(OSError,'activation lock'):self.backend.select_providers()
        self.assertEqual(self.backend.state_guard.read_regular('checked-installations.json'),before)
        self.assertEqual(self.backend.selected()['root'],initial['root'])
        self.close();backend=self.backend_open();backend.args.fidelityfx=root;backend.run_child=self.fake_build;backend.select_providers()
        selected=backend.selected();before=backend.state_guard.read_regular('checked-installations.json')
        self.close();(root/'LICENSE.txt').write_text('changed original license')
        backend=self.backend_open('build',Path(selected['destination']));backend.run_child=self.fake_build
        # Same source-kit rebuild retains its original provider source identity.
        with self.assertRaisesRegex(B.BootstrapError,'provider source changed'):backend.build()
        self.assertEqual(backend.state_guard.read_regular('checked-installations.json'),before)

    def test_provider_empty_inventory_and_mismatched_sidecar_refused(self):
        root=self.local_provider();backend=self.backend_open('build',self.folder/'provider inventory installation')
        backend.args.fidelityfx=root;backend.run_child=self.fake_build;backend.build();record=backend.selected();output=Path(record['root'])
        receipt=json.loads((output/'build-receipt.json').read_text());selection=record['providers']
        broken=json.loads(B.encoded(receipt));broken['providers']['files']={}
        with self.assertRaises(B.BootstrapError):backend.checked_providers(broken,selection,output)
        sidecar=output/'windows-providers.json';raw=sidecar.read_bytes();value=json.loads(raw);value['selection']={}
        changed=B.encoded(value);sidecar.write_bytes(changed)
        receipt['providers']['files']['windows-providers.json']={'bytes':len(changed),'sha256':sha(changed)}
        receipt['outputs']['windows-providers.json']=sha(changed)
        with self.assertRaisesRegex(B.BootstrapError,'sidecar differs'):backend.checked_providers(receipt,selection,output)

    def test_manager_footer_payload_binding_and_missing_optional_manager(self):
        import struct
        backend=self.backend_open();payload=b'authored embedded payload boundary';digest=sha(payload)
        manager=self.folder/'authored portable manager 測試.exe'
        raw=self.exe_bytes+payload+struct.pack('<8sQQ32s',b'M2BNDL01',len(self.exe_bytes),len(payload),bytes.fromhex(digest))
        manager.write_bytes(raw);backend.args.payload_sha256=digest
        backend.args.manager_executable=manager;backend.args.manager_sha256=sha(raw)
        checked,args=backend.manager_choice();self.assertEqual(checked['sha256'],sha(raw));self.assertIn('--preparation-manager',args)
        with self.assertRaises(OSError):manager.write_bytes(b'changed while held')
        self.close();manager.unlink();backend=self.backend_open()
        self.assertEqual(backend.manager_choice({'manager':checked}),(None,[]))
        broken=bytearray(raw);broken[len(self.exe_bytes)]^=1;manager.write_bytes(broken)
        backend.args.payload_sha256=digest;backend.args.manager_executable=manager;backend.args.manager_sha256=sha(broken)
        with self.assertRaisesRegex(B.BootstrapError,'payload hash differs'):backend.manager_choice()

    def test_verify_cold_resume_and_bounded_progress(self):
        backend=self.backend_open();backend.resume();backend.progress.emit('test','x'*100000,done=1,total=2)
        lines=Path(backend.args.progress).read_bytes().splitlines()
        self.assertEqual(json.loads(lines[-2])['status'],'needs-setup')
        self.assertTrue(all(len(line)<65536 for line in lines))
        self.assertTrue(all(type(json.loads(line)['done']) is int for line in lines))

    def test_isolated_bundled_packages_added_only_after_full_verification(self):
        state=self.cache/'state';token=uuid.uuid4().hex
        args=argparse.Namespace(cache=self.cache,payload_sha256=self.cache.name,manifest_sha256=self.manifest_hash,
            progress=state/('progress-'+token+'.jsonl'),cancel_file=state/('cancel-'+token),action='resume',
            input=self.input,destination=None,jobs=2,precompile=False)
        package_path=str(self.cache/'python/Lib/site-packages')
        self.assertNotIn(package_path,sys.path)
        self.backend=B.Backend(args)
        self.assertNotIn(package_path,sys.path)
        self.backend.verify()
        self.assertIn(package_path,sys.path)

    def test_fsr4_selection_reopen_and_changed_model_fallback_preserve_state(self):
        # Only the vendor/model verification boundary is authored here. The
        # actual guarded activation, durable profile selection and resume run.
        record=self.build_managed();profile=Path(record['profile'])
        worker=B.import_checked(PORT/'tools/fsr4_prepare_worker_windows.py','authored_fsr4_backend_worker')
        assets=self.folder/'authored model validator boundary';assets.mkdir()
        (assets/'receipt.json').write_bytes(b'authored only; no model assets')
        component=self.folder/'authored nonexecuted component.dll';component.write_bytes(b'no executable content')
        def verify(output,component=None):
            return dict(asset_root=str(output),receipt_path=str(Path(output)/'receipt.json'),receipt_sha256=sha((Path(output)/'receipt.json').read_bytes()),
                model_identity=worker.IDENTITY,component=str(component),dll_sha256=sha(Path(component).read_bytes()),gpu_qualified=False)
        pipeline=types.SimpleNamespace(verify_prepared=verify)
        self.backend.fsr4_modules=lambda:(worker,pipeline)
        self.backend.args.assets=assets;self.backend.args.component=component
        registry=self.backend.state_guard.read_regular('checked-installations.json')
        (profile/'human-save').write_bytes(b'preserve human state')
        self.backend.prepare_fsr4(True);selection=(profile/worker.SELECTION).read_bytes()
        self.assertEqual(self.backend.state_guard.read_regular('checked-installations.json'),registry)
        self.backend.resume();last=json.loads(Path(self.backend.args.progress).read_text().splitlines()[-1])
        self.assertEqual(last['launcher_args'],['--profile-root',str(profile),'--fsr4-assets',str(assets),'--fsr4-component',str(component)])
        self.assertEqual(last['fsr4_component'],str(component))
        component.write_bytes(b'changed authored component')
        self.backend.resume();last=json.loads(Path(self.backend.args.progress).read_text().splitlines()[-1])
        self.assertEqual(last['launcher_args'],['--profile-root',str(profile)])
        self.assertEqual((profile/worker.SELECTION).read_bytes(),selection)
        self.assertEqual((profile/'human-save').read_bytes(),b'preserve human state')

    def test_build_fsr4_tools_records_sources_without_changing_model_or_runtime(self):
        record=self.build_managed();profile=Path(record['profile'])
        worker=B.import_checked(PORT/'tools/fsr4_prepare_worker_windows.py','authored_tools_backend_worker')
        for key in ('archives','support'):
            path=self.folder/key;path.mkdir();setattr(self.backend.args,key,path)
        for key in ('cmake','ninja','compiler'):
            path=self.folder/(key+'.exe');path.write_bytes(b'authored nonexecuted build boundary');setattr(self.backend.args,key,path)
        def build(output,*args,**kwargs):
            Path(output).mkdir();(Path(output)/'receipt.json').write_bytes(b'authored tool verifier boundary, no binaries')
        def verify(output):
            return dict(schema=1,status='passed',kind='windows-fsr4-tools',output=str(output),receipt_sha256=sha((Path(output)/'receipt.json').read_bytes()))
        def component_build(output,**kwargs):
            Path(output).mkdir();(Path(output)/'monado-fsr4.dll').write_bytes(b'authored nonexecuted component boundary')
        def component_verify(path):return dict(component=str(path),dll_sha256=sha(Path(path).read_bytes()))
        self.backend.fsr4_modules=lambda:(worker,types.SimpleNamespace(tools=types.SimpleNamespace(build=build,verify_tools=verify),
            component=types.SimpleNamespace(build=component_build),verify_component=component_verify))
        registry=self.backend.state_guard.read_regular('checked-installations.json')
        self.backend.build_fsr4_tools();last=json.loads(Path(self.backend.args.progress).read_text().splitlines()[-1])
        self.assertEqual(last['status'],'success');self.assertEqual(last['fsr4_archives'],str(self.backend.args.archives))
        self.assertTrue(Path(last['fsr4_component']).is_file())
        self.assertTrue(Path(last['fsr4_tools']).is_dir());self.assertFalse((profile/worker.SELECTION).exists())
        self.assertEqual(self.backend.state_guard.read_regular('checked-installations.json'),registry)
        self.backend.resume();last=json.loads(Path(self.backend.args.progress).read_text().splitlines()[-1])
        self.assertEqual(last['fsr4_tools'],worker.document((profile/worker.TOOLS_SELECTION).read_bytes())['tools'])
        self.assertEqual(last['launcher_args'],['--profile-root',str(profile)])

    def test_shader_completion_and_cancel_use_current_runtime_session(self):
        record=self.build_managed();runtime=Path(record['root']);profile=Path(record['profile'])
        before=self.backend.state_guard.read_regular('checked-installations.json');commands=[]
        def child(argv,*,cwd,phase,cancel_request=None,**unused):
            commands.append(argv);self.assertEqual(phase,'shaders');self.assertEqual(Path(cwd),profile)
            session=argv[argv.index('--session')+1];identity=argv[argv.index('--identity')+1]
            self.assertEqual(identity,sha((runtime/'build/xcde.exe').read_bytes()))
            cancel_request();cancel=profile/'cache'/('precompile_cancel-'+session+'.txt')
            self.assertEqual(cancel.read_bytes(),('monado_precompile_cancel 1\nsession '+session+'\n').encode())
            (profile/'cache/precompile_work.txt').write_bytes(shader_work(session,identity))
        self.backend.run_child=child;self.backend.prepare_shaders(runtime,profile);self.backend.prepare_shaders(runtime,profile)
        self.assertNotEqual(commands[0][4],commands[1][4])
        self.assertEqual(self.backend.state_guard.read_regular('checked-installations.json'),before)
        def stale(argv,**unused):
            (profile/'cache/precompile_work.txt').write_bytes(shader_work('0'*32,argv[argv.index('--identity')+1]))
        self.backend.run_child=stale
        with self.assertRaises(B.BootstrapError):self.backend.prepare_shaders(runtime,profile)
        self.assertEqual(self.backend.state_guard.read_regular('checked-installations.json'),before)

    def test_payload_tampering_extra_code_and_wrong_anchor_fail_closed(self):
        backend=self.backend_open();self.close()
        candidate=self.cache/'source/Monado_Windows/games/xcde/profile.json';before=candidate.read_bytes();candidate.write_bytes(b'{}')
        with self.assertRaises(B.BootstrapError):self.backend_open()
        self.close();candidate.write_bytes(before)
        extra=self.cache/'python/sitecustomize.py';extra.write_text('raise RuntimeError("must never run")')
        with self.assertRaises(B.BootstrapError):self.backend_open()
        self.close();extra.unlink();self.manifest_hash='f'*64
        with self.assertRaises(B.BootstrapError):self.backend_open()

    def test_native_cache_markers_are_bounded_and_identity_checked(self):
        (self.cache/'.lock').write_bytes(b'');(self.cache/'.complete').write_bytes(self.cache.name.encode('ascii'))
        self.backend_open();self.close()
        (self.cache/'.complete').write_bytes(b'e'*64)
        with self.assertRaises(B.BootstrapError):self.backend_open()
        self.close();(self.cache/'.complete').write_bytes(self.cache.name.encode('ascii'));(self.cache/'.lock').write_bytes(b'x')
        with self.assertRaises(B.BootstrapError):self.backend_open()

    def test_full_input_inventory_and_version_refusal(self):
        backend=self.backend_open();identity=backend.input_identity(self.input)
        self.assertEqual(len(identity['exefs']),5)
        (self.input/'exefs/main.npdm').write_bytes(b'changed authored metadata')
        with self.assertRaises(B.BootstrapError):backend.input_identity(self.input,identity)
        (self.input/'exefs/main').write_bytes(b'bad')
        with self.assertRaises(B.BootstrapError):backend.input_identity(self.input)

    def test_unknown_destination_and_modified_source_preserved(self):
        backend=self.backend_open();destination=self.folder/'human';destination.mkdir();(destination/'unknown.txt').write_bytes(b'keep')
        with self.assertRaises(B.BootstrapError):backend.stage_source(destination)
        self.assertEqual((destination/'unknown.txt').read_bytes(),b'keep')
        destination=self.folder/'new';backend.args.destination=destination;backend.stage_source(destination)
        source=destination/'source/Monado_Windows/scripts/build_windows.py';source.write_text('# human edit\n')
        with self.assertRaises(B.BootstrapError):backend.stage_source(destination)
        self.assertEqual(source.read_text(),'# human edit\n')

    def test_build_retry_reopen_keeps_profile_and_receipts(self):
        first=self.build_managed();profile=Path(first['profile']);saved=profile/'unknown-profile.data';saved.write_bytes(b'used authored profile')
        original=(profile/'build-receipt.json').read_bytes();self.close()
        backend=self.backend_open('build',Path(first['destination']));backend.run_child=self.fake_build;backend.build()
        second=backend.registry['installations'][first['id']]
        self.assertNotEqual(second['root'],first['root']);self.assertEqual(second['profile'],first['profile'])
        self.assertEqual(saved.read_bytes(),b'used authored profile');self.assertEqual((profile/'build-receipt.json').read_bytes(),original)
        self.close();backend=self.backend_open();backend.resume()
        last=json.loads(Path(backend.args.progress).read_text().splitlines()[-1])
        self.assertEqual(last['launcher_args'],['--profile-root',str(profile)]);self.assertEqual(last['status'],'success')

    def test_adopt_original_then_locate_is_atomic_and_receipt_immutable(self):
        root=self.folder/'legacy';self.make_install(root);before=(root/'build-receipt.json').read_bytes()
        backend=self.backend_open('adopt',root);backend.adopt();record=backend.selected()
        moved=self.folder/'moved original';self.input.rename(moved);backend.args.input=moved;backend.args.action='locate-input'
        backend.locate();record=backend.selected()
        self.assertEqual((root/'targets/exefs').resolve(),moved/'exefs')
        self.assertEqual((root/'build-receipt.json').read_bytes(),before)
        self.assertEqual(record['input']['exefs'],B.validate_input(moved/'exefs',moved/'romfs',self.profile,self.expected)['exefs'])

    def test_legacy_missing_baseline_refused_without_reseeding(self):
        root=self.folder/'legacy';self.make_install(root);before=(root/'build-receipt.json').read_bytes()
        self.input.rename(self.folder/'missing original');backend=self.backend_open('adopt',root)
        with self.assertRaisesRegex(B.BootstrapError,'Original input is unavailable'):backend.adopt()
        self.assertEqual((root/'build-receipt.json').read_bytes(),before)
        self.assertIsNone(backend.registry['last'])

    def test_locate_rejects_wrong_inventory_and_unknown_target_directory(self):
        root=self.folder/'legacy';self.make_install(root);backend=self.backend_open('adopt',root);backend.adopt()
        before=backend.state_guard.read_regular('checked-installations.json')
        (self.input/'exefs/main.npdm').write_bytes(b'different')
        with self.assertRaises(B.BootstrapError):backend.locate()
        self.assertEqual(backend.state_guard.read_regular('checked-installations.json'),before)
        # A human directory at the known junction name must never be removed.
        (root/'targets/exefs').rmdir();(root/'targets/exefs').mkdir();(root/'targets/exefs/keep').write_bytes(b'keep')
        with self.assertRaises(B.BootstrapError):backend.locate()
        self.assertEqual((root/'targets/exefs/keep').read_bytes(),b'keep')

    def test_failed_durable_locate_publish_restores_both_original_junctions(self):
        root=self.folder/'legacy';self.make_install(root);before_receipt=(root/'build-receipt.json').read_bytes()
        backend=self.backend_open('adopt',root);backend.adopt()
        before=backend.state_guard.read_regular('checked-installations.json');memory=B.encoded(backend.registry)
        old_input=self.input; moved=self.folder/'relocated original';old_input.rename(moved);backend.args.input=moved
        original_put=B.put_json
        def refuse(guard,name,document):
            if name=='checked-installations.json':raise OSError('authored durable registry lock')
            return original_put(guard,name,document)
        with patch.object(B,'put_json',side_effect=refuse):
            with self.assertRaisesRegex(OSError,'registry lock'):backend.locate()
        self.assertEqual(backend.state_guard.read_regular('checked-installations.json'),before)
        self.assertEqual(B.encoded(backend.registry),memory)
        self.assertEqual((root/'build-receipt.json').read_bytes(),before_receipt)
        self.assertEqual({p.name for p in (root/'targets').iterdir()},{'exefs','romfs'})
        for name in ('exefs','romfs'):
            self.assertTrue((root/'targets'/name).is_junction())
            self.assertEqual((root/'targets'/name).resolve(),old_input/name)
        backend.locate()
        self.assertEqual((root/'targets/exefs').resolve(),moved/'exefs')

    def test_owned_job_cancel_stops_descendant_and_records_real_output(self):
        backend=self.backend_open();backend.args.input=None
        code='import subprocess,sys,time; p=subprocess.Popen([sys.executable,"-c","import time;time.sleep(30)"]);print("owned-descendant",p.pid,flush=True);time.sleep(30)'
        def cancel():
            time.sleep(.5);backend.state_guard.write_new(backend.cancel.name,b'cancel')
        thread=threading.Thread(target=cancel);thread.start()
        try:
            with self.assertRaises(B.Cancelled):backend.run_child([sys.executable,'-u','-c',code],cwd=self.folder,phase='authored-child')
        finally:thread.join()
        text=backend.log_path.read_text();self.assertIn('owned-descendant',text)
        pid=int(text.split('owned-descendant ')[1].splitlines()[0])
        api=ctypes.WinDLL('kernel32',use_last_error=True);api.OpenProcess.restype=ctypes.c_void_p
        api.OpenProcess.argtypes=[ctypes.c_ulong,ctypes.c_int,ctypes.c_ulong];api.WaitForSingleObject.argtypes=[ctypes.c_void_p,ctypes.c_ulong];api.CloseHandle.argtypes=[ctypes.c_void_p]
        process=api.OpenProcess(0x100000,False,pid)
        if process:
            try:self.assertEqual(api.WaitForSingleObject(process,1000),0)
            finally:api.CloseHandle(process)

    def test_real_compile_progress_and_child_failure_are_not_success(self):
        backend=self.backend_open();output=self.folder/'authored-output';(output/'generated').mkdir(parents=True);(output/'generated/a.c').write_text('/* authored */')
        code='print("Compiled runtime/first.c");print("Compiled build/authored/generated/a.c");print("Split authored host and local GameLib linked");print("Compiled tools/launcher/fixture.cpp")'
        backend.run_child([sys.executable,'-c',code],cwd=self.folder,phase='build',output=output)
        rows=[json.loads(line) for line in Path(backend.args.progress).read_text().splitlines()]
        generated=[r for r in rows if r['phase']=='game-code'];self.assertEqual([(r['done'],r['total']) for r in generated],[(1,1)])
        with self.assertRaises(B.BootstrapError):backend.run_child([sys.executable,'-c','raise SystemExit(17)'],cwd=self.folder,phase='failure')

    def test_owned_python_children_ignore_hostile_user_python_environment(self):
        backend=self.backend_open();hostile=self.folder/'hostile Python environment';hostile.mkdir()
        (hostile/'sitecustomize.py').write_text('raise RuntimeError("untrusted environment code must not execute")\n')
        code='''import os,site,subprocess,sys
expected={'PYTHONNOUSERSITE':'1','PYTHONDONTWRITEBYTECODE':'1','PYTHONUNBUFFERED':'1'}
actual={k:v for k,v in os.environ.items() if k.upper().startswith('PYTHON')}
assert actual==expected,actual
assert not site.ENABLE_USER_SITE
assert os.environ['MONADO_AUTHORED_PRESERVED_ENV']=='keep'
subprocess.run([sys.executable,'-c',"import os,site; assert 'PYTHONPATH' not in os.environ; assert 'PYTHONHOME' not in os.environ; assert 'PYTHONUSERBASE' not in os.environ; assert not site.ENABLE_USER_SITE; print('nested checked Python environment')"],check=True)
print('checked Python environment')
'''
        with patch.dict(os.environ,{'PYTHONHOME':str(hostile/'missing home'),'PYTHONPATH':str(hostile),
                    'PYTHONUSERBASE':str(hostile),'PYTHONNOUSERSITE':'0','MONADO_AUTHORED_PRESERVED_ENV':'keep'}):
            backend.run_child([sys.executable,'-c',code],cwd=self.folder,phase='authored-environment')
        log=backend.log_path.read_text();self.assertIn('nested checked Python environment',log)
        self.assertIn('checked Python environment',log)

    def test_busy_exact_authored_runtime_refuses_without_closing_it(self):
        root=self.folder/'busy';self.make_install(root);process=subprocess.Popen([str(root/'build/xcde.exe'),'sleep'])
        try:
            time.sleep(.2);backend=self.backend_open()
            with self.assertRaisesRegex(B.BootstrapError,'open game or settings'):backend.require_idle({'root':str(root),'profile':str(root)})
            self.assertIsNone(process.poll())
        finally:process.terminate();process.wait(timeout=10)

    def busy_after_build(self,action):
        record=self.build_managed();profile=Path(record['profile']);processes=[]
        before=self.backend.state_guard.read_regular('checked-installations.json');receipt=(profile/'build-receipt.json').read_bytes()
        (profile/'cache').mkdir();(profile/'cache/human-cache-sentinel').write_bytes(b'keep existing useful cache')
        manifest={'files':self.backend.source_files};publication=B.encoded(manifest);outer=self
        class Update:
            UpdateCancelled=B.Cancelled
            def discover(self,version):return {'version':'1.0.2','source_manifest_sha256':sha(publication)}
            def fetch_and_extract(self,offer,candidate,**kwargs):
                candidate.mkdir();shutil.copytree(outer.cache/'source',candidate/'source')
                (candidate/'source/publication-manifest.json').write_bytes(publication);return candidate/'source'
            def verified_source(self,source,offer):return manifest
        def compile_boundary(argv,*,cwd,phase,output=None,**unused):
            self.assertNotEqual(phase,'shaders','shader child started after the existing profile became busy')
            self.fake_build(argv,cwd=cwd,phase=phase,output=output)
            # Actual native process identity appears after the lengthy compiler
            # boundary. No game input or human process is used or terminated.
            binary=str(Path(record['root'])/'build/xcde.exe')
            native='\\\\?\\UNC\\'+binary[2:] if binary.startswith('\\\\') else '\\\\?\\'+binary
            processes.append(subprocess.Popen([binary,'sleep'],executable=native))
            time.sleep(.2);self.assertIsNone(processes[-1].poll())
        self.backend.run_child=compile_boundary;self.backend.args.precompile=True
        original=B.import_checked
        try:
            with patch.object(B,'import_checked',side_effect=lambda path,name:Update() if name=='monado_trusted_windows_updater' else original(path,name)):
                with self.assertRaisesRegex(B.BootstrapError,'open game or settings'):
                    if action=='build':self.backend.build()
                    else:self.backend.args.destination=None;self.backend.update(True)
            self.assertEqual(len(processes),1);self.assertIsNone(processes[0].poll())
            self.assertEqual(self.backend.state_guard.read_regular('checked-installations.json'),before)
            self.assertEqual((profile/'build-receipt.json').read_bytes(),receipt)
            self.assertEqual({p.name for p in (profile/'cache').iterdir()},{'human-cache-sentinel'})
            self.assertEqual((profile/'cache/human-cache-sentinel').read_bytes(),b'keep existing useful cache')
        finally:
            for process in processes:
                if process.poll() is None:process.terminate()
                process.wait(timeout=10)

    def test_rebuild_rechecks_busy_profile_before_shader_preparation(self):self.busy_after_build('build')
    def test_update_rechecks_busy_profile_before_shader_preparation(self):self.busy_after_build('update')

    def test_update_verifies_before_build_and_activates_registry_only(self):
        record=self.build_managed();profile=Path(record['profile']);(profile/'human-save').write_bytes(b'authored used save')
        before=(profile/'build-receipt.json').read_bytes();events=[]
        manifest={'files':self.backend.source_files};publication=B.encoded(manifest)
        class Update:
            UpdateCancelled=B.Cancelled
            def discover(self,version):events.append('discover');return {'version':'1.0.1','source_manifest_sha256':sha(publication)}
            def fetch_and_extract(self,offer,candidate,**kwargs):
                events.append('fetch');candidate.mkdir();shutil.copytree(self_outer.cache/'source',candidate/'source')
                (candidate/'source/publication-manifest.json').write_bytes(publication);return candidate/'source'
            def verified_source(self,source,offer):events.append('verify');return manifest
        self_outer=self;original=B.import_checked
        def load(path,name):return Update() if name=='monado_trusted_windows_updater' else original(path,name)
        self.backend.args.destination=None
        def compile_boundary(*args,**kwargs):
            events.append('build');self.fake_build(*args,**kwargs)
            derived=kwargs['cwd']/'.deps/zig-cache';derived.mkdir(parents=True);(derived/'authored-cache').write_bytes(b'authored generated cache')
        self.backend.run_child=compile_boundary
        with patch.object(B,'import_checked',side_effect=load):self.backend.update(True)
        selected=self.backend.registry['installations'][record['id']]
        self.assertEqual(events,['discover','fetch','verify','build']);self.assertNotEqual(selected['root'],record['root'])
        self.assertEqual(selected['profile'],record['profile']);self.assertEqual((profile/'human-save').read_bytes(),b'authored used save')
        self.assertEqual((profile/'build-receipt.json').read_bytes(),before)
        candidate=Path(selected['source_root']).parents[1]
        authority=Path(record['destination'])/'updates'/('.retention-'+candidate.name+'.json')
        self.assertEqual(selected['retention']['manifest_sha256'],sha(authority.read_bytes()))
        # A later action has no live leases on the previous candidate. Only
        # that exact prepared obsolete tree may disappear after activation.
        self.close();self.backend_open();self.backend.args.destination=None;self.backend.run_child=compile_boundary
        with patch.object(B,'import_checked',side_effect=load):self.backend.update(True)
        self.assertFalse(candidate.exists());self.assertFalse(authority.exists())
        self.assertEqual((profile/'human-save').read_bytes(),b'authored used save')

    def test_update_cleanup_failure_keeps_successful_new_selection(self):
        record=self.build_managed();manifest={'files':self.backend.source_files};publication=B.encoded(manifest)
        outer=self
        class Update:
            UpdateCancelled=B.Cancelled
            def discover(self,version):return {'version':'1.0.2','source_manifest_sha256':sha(publication)}
            def fetch_and_extract(self,offer,candidate,**kwargs):
                candidate.mkdir();shutil.copytree(outer.cache/'source',candidate/'source')
                (candidate/'source/publication-manifest.json').write_bytes(publication);return candidate/'source'
            def verified_source(self,source,offer):return manifest
        original=B.import_checked
        def load(path,name):
            if name=='monado_trusted_windows_updater':return Update()
            result=original(path,name)
            if name=='monado_trusted_windows_retention':
                result.cleanup=lambda *args,**kwargs:(_ for _ in ()).throw(OSError('authored locked obsolete inventory'))
            return result
        self.backend.args.destination=None;self.backend.run_child=self.fake_build
        with patch.object(B,'import_checked',side_effect=load):self.backend.update(True)
        selected=self.backend.registry['installations'][record['id']]
        self.assertNotEqual(selected['root'],record['root'])
        result=json.loads(Path(self.backend.args.progress).read_text().splitlines()[-1])
        self.assertEqual(result['status'],'success');self.assertIn('cleanup is deferred',result['message'])
        persisted=json.loads(self.backend.state_guard.read_regular('checked-installations.json'))
        self.assertEqual(persisted['installations'][record['id']]['root'],selected['root'])

    def test_failed_update_activation_preserves_original_durable_selection(self):
        record=self.build_managed();before=self.backend.state_guard.read_regular('checked-installations.json')
        memory=B.encoded(self.backend.registry);manifest={'files':self.backend.source_files};publication=B.encoded(manifest)
        outer=self
        class Update:
            UpdateCancelled=B.Cancelled
            def discover(self,version):return {'version':'1.0.2','source_manifest_sha256':sha(publication)}
            def fetch_and_extract(self,offer,candidate,**kwargs):
                candidate.mkdir();shutil.copytree(outer.cache/'source',candidate/'source')
                (candidate/'source/publication-manifest.json').write_bytes(publication);return candidate/'source'
            def verified_source(self,source,offer):return manifest
        original=B.import_checked;original_put=B.put_json
        def refuse(guard,name,document):
            if name=='checked-installations.json':raise OSError('authored activation registry lock')
            return original_put(guard,name,document)
        self.backend.args.destination=None;self.backend.run_child=self.fake_build
        with patch.object(B,'import_checked',side_effect=lambda path,name:Update() if name=='monado_trusted_windows_updater' else original(path,name)),patch.object(B,'put_json',side_effect=refuse):
            with self.assertRaisesRegex(OSError,'registry lock'):self.backend.update(True)
        self.assertEqual(self.backend.state_guard.read_regular('checked-installations.json'),before)
        self.assertEqual(B.encoded(self.backend.registry),memory)
        self.assertTrue(Path(record['root']).is_dir())
        self.assertTrue((Path(record['destination'])/'updates').is_dir())

    def reuse_update_case(self,mode):
        # The separate native GameLib suite authenticates real split PE files.
        # This fixture controls only that compiler/classifier boundary to prove
        # CLI forwarding, retry classification and separate-output activation.
        record=self.build_managed();before=self.backend.state_guard.read_regular('checked-installations.json')
        manifest={'files':self.backend.source_files};publication=B.encoded(manifest);outer=self;attempts=[]
        class Update:
            UpdateCancelled=B.Cancelled
            def discover(self,version):return {'version':'1.0.3','source_manifest_sha256':sha(publication)}
            def fetch_and_extract(self,offer,candidate,**kwargs):
                candidate.mkdir();shutil.copytree(outer.cache/'source',candidate/'source')
                (candidate/'source/publication-manifest.json').write_bytes(publication);return candidate/'source'
            def verified_source(self,source,offer):return manifest
        original_import=B.import_checked;original_check=self.backend.checked_build
        def checked(path,*args,**kwargs):
            receipt,digest=original_check(path,*args,**kwargs)
            if Path(path)==Path(record['root']):receipt=dict(receipt,split_game={'authored_compiler_boundary':True})
            return receipt,digest
        def compile_boundary(argv,*,cwd,phase,output=None):
            attempts.append((list(map(str,argv)),output))
            if len(attempts)==1 and mode!='reuse':
                output.mkdir(parents=True)
                failure={'schema':1,'implementation':'windows-parity-development','status':'failed',
                         'input':{'exefs':{n:record['input']['exefs'][n] for n in self.expected}}}
                if mode=='incompatible':failure['failure_kind']='incompatible-game-library'
                (output/'build-receipt.json').write_bytes(B.encoded(failure))
                raise B.ChildFailure('authored '+mode,1 if mode=='corrupt' else 3)
            self.fake_build(argv,cwd=cwd,phase=phase,output=output)
        self.backend.args.destination=None;self.backend.run_child=compile_boundary
        with patch.object(B,'import_checked',side_effect=lambda path,name:Update() if name=='monado_trusted_windows_updater' else original_import(path,name)),patch.object(self.backend,'checked_build',side_effect=checked):
            if mode in ('corrupt','unclassified'):
                with self.assertRaises(B.ChildFailure):self.backend.update(True)
            else:self.backend.update(True)
        self.assertIn('--reuse-game-library',attempts[0][0])
        self.assertEqual(attempts[0][0][attempts[0][0].index('--reuse-game-library')+1],record['root'])
        if mode=='incompatible':
            self.assertEqual(len(attempts),2);self.assertNotIn('--reuse-game-library',attempts[1][0])
            self.assertNotEqual(attempts[0][1],attempts[1][1]);self.assertFalse(attempts[0][1].exists())
            saved=list((Path(record['destination'])/'failed-builds').iterdir());self.assertEqual(len(saved),1)
            self.assertEqual(json.loads((saved[0]/'build-receipt.json').read_bytes())['failure_kind'],'incompatible-game-library')
        else:self.assertEqual(len(attempts),1)
        if mode in ('corrupt','unclassified'):
            self.assertEqual(self.backend.state_guard.read_regular('checked-installations.json'),before)
            self.assertEqual(self.backend.registry['installations'][record['id']]['root'],record['root'])
        else:
            self.assertNotEqual(self.backend.registry['installations'][record['id']]['root'],record['root'])
            self.assertEqual(json.loads(Path(self.backend.args.progress).read_text().splitlines()[-1])['status'],'success')

    def test_update_forwards_compatible_game_library_reuse(self):self.reuse_update_case('reuse')
    def test_update_typed_incompatibility_regenerates_in_separate_output(self):self.reuse_update_case('incompatible')
    def test_update_corrupt_reuse_never_falls_back(self):self.reuse_update_case('corrupt')
    def test_update_unclassified_exit3_never_falls_back(self):self.reuse_update_case('unclassified')

    def test_update_verifier_failure_never_executes_candidate(self):
        record=self.build_managed();before=self.backend.state_guard.read_regular('checked-installations.json')
        class Update:
            UpdateCancelled=B.Cancelled
            def discover(self,version):return {'version':'1.0.1','source_manifest_sha256':'e'*64}
            def fetch_and_extract(self,*args,**kwargs):raise B.BootstrapError('authored verifier refusal')
        original=B.import_checked
        self.backend.args.destination=None;self.backend.run_child=lambda *args,**kwargs:self.fail('refused candidate was executed')
        with patch.object(B,'import_checked',side_effect=lambda path,name:Update() if name=='monado_trusted_windows_updater' else original(path,name)):
            with self.assertRaisesRegex(B.BootstrapError,'verifier refusal'):self.backend.update(True)
        self.assertEqual(self.backend.state_guard.read_regular('checked-installations.json'),before)


if __name__=='__main__':unittest.main(verbosity=2)
