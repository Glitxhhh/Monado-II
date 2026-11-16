"""Native worker metadata/jobs contracts with no vendor/model/game assets.

Only the external all-model/GPU validator boundary is replaced by the explicit
authored verifier below. These fixtures do not qualify FSR4 model/GPU behavior.
Context tests replace interpreter/entry identity only; native leases, closure
hashes, import membership, receipts and actual metadata replacement stay real.
"""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import sys
import time
import types
import unittest
from unittest.mock import patch
import uuid

PORT=Path(__file__).resolve().parents[2];ROOT=PORT.parent
def load(path,name):
    spec=importlib.util.spec_from_file_location(name,path);module=importlib.util.module_from_spec(spec)
    sys.modules[name]=module;spec.loader.exec_module(module);return module
W=load(PORT/'tools/fsr4_prepare_worker_windows.py','fsr4_worker_contract')
G=load(ROOT/'tools/windows_file_guard.py','fsr4_worker_guard_contract')
def sha(data):return hashlib.sha256(data).hexdigest()

class Pure(unittest.TestCase):
    def test_progress_is_real_bounded_and_typed(self):
        prefix='MONADO_FSR4_PROGRESS '
        self.assertIsNone(W.progress_record('ordinary log'))
        value={'stage':'validate','current':3,'total':48,'message':'actual completed shader'}
        self.assertEqual(W.progress_record(prefix+json.dumps(value)),{'phase':'fsr4-validate','done':3,'total':48,'message':value['message']})
        for key,item in (('current',True),('current',-1),('total',1.5),('stage','path/escape'),('message','x'*2049)):
            bad=dict(value,**{key:item})
            with self.subTest(key=key,item=str(item)[:20]),self.assertRaises(W.PreparationError):W.progress_record(prefix+json.dumps(bad))
        for raw in (b'[]',b'{"x":1,"x":2}',b'{"x":NaN}',b'x'*(W.BOUND+1)):
            with self.assertRaises(W.PreparationError):W.document(raw)

    def test_fixed_inprocess_pipeline_streams_only_real_progress(self):
        rows=[];seen=[]
        def prepare(*args):
            seen.append(args)
            print('MONADO_FSR4_PROGRESS '+json.dumps({'stage':'capture','current':1,'total':4,'message':'actual stage'}))
            return {'authored':'pipeline boundary only'}
        argv=['python','-I','-S','-B','prepare.py','--prepare','--sdk','sdk','--tools','tools','--component','component','--output','output','--cancel-file','cancel']
        value=W.run_verified(types.SimpleNamespace(prepare=prepare),argv,emit=lambda **row:rows.append(row))
        self.assertEqual(value,{'authored':'pipeline boundary only'})
        self.assertEqual(seen,[(Path('output'),Path('sdk'),Path('tools'),Path('component'),Path('cancel'))])
        self.assertEqual(rows,[{'phase':'fsr4-capture','done':1,'total':4,'message':'actual stage'}])
        with self.assertRaises(W.PreparationError):W.run_verified(None,argv+['--unknown'],emit=lambda **row:None)

@unittest.skipUnless(os.name=='nt','native Windows worker metadata contracts')
class Native(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.evidence=PORT/'runs'/('fsr4-worker-'+str(time.time_ns())+'-'+uuid.uuid4().hex[:8]);cls.evidence.mkdir()
        cls.results=[]
    @classmethod
    def tearDownClass(cls):
        (cls.evidence/'receipt.json').write_bytes(W.encoded({'scope':__doc__,'sources':{
            str(p.relative_to(ROOT)):sha(p.read_bytes()) for p in (Path(__file__),PORT/'tools/fsr4_prepare_worker_windows.py',ROOT/'tools/windows_file_guard.py')},'cases':cls.results}))
        print('Native FSR4 worker evidence: '+str(cls.evidence),flush=True)
    def setUp(self):
        self.root=self.evidence/(self._testMethodName+'-'+uuid.uuid4().hex[:8]);self.root.mkdir()
        self.profile=self.root/'profile 測試';self.profile.mkdir()
        self.assets=self.root/'authored validator boundary';self.assets.mkdir()
        (self.assets/'receipt.json').write_bytes(b'authored verifier boundary, no models')
        self.component=self.root/'authored-component-boundary.dll';self.component.write_bytes(b'not executable; no component execution')
        self.calls=[]
        def verify(assets,component=None):
            self.calls.append((Path(assets),Path(component)))
            return {'asset_root':str(assets),'receipt_path':str(Path(assets)/'receipt.json'),
                'receipt_sha256':sha((Path(assets)/'receipt.json').read_bytes()),'model_identity':W.IDENTITY,
                'component':str(component),'dll_sha256':sha(Path(component).read_bytes()),'gpu_qualified':False}
        self.pipeline=types.SimpleNamespace(verify_prepared=verify)
    def tearDown(self):
        result=getattr(getattr(self,'_outcome',None),'result',None);failures=(result.failures+result.errors) if result is not None else [(self,'missing result')]
        self.results.append({'case':self._testMethodName,'directory':self.root.name,'completed':not any(test is self for test,_ in failures)})
    def select(self,**extra):return W.activate(self.profile,assets=self.assets,component=self.component,pipeline=self.pipeline,guard=G,**extra)
    def raw(self):return (self.profile/W.SELECTION).read_bytes()
    def test_atomic_selection_and_reopen_preserve_human_state(self):
        (self.profile/'xcde.ini').write_bytes(b'unknown preference=yes');(self.profile/'human-save').write_bytes(b'human sentinel')
        first=self.select(sdk='explicit local SDK',tools='explicit local tools')
        self.assertEqual(W.verify_selection(self.profile,pipeline=self.pipeline,guard=G),first)
        second=self.select();self.assertNotEqual(first['selection_id'],second['selection_id'])
        self.assertEqual((self.profile/'xcde.ini').read_bytes(),b'unknown preference=yes')
        self.assertEqual((self.profile/'human-save').read_bytes(),b'human sentinel')
        self.assertFalse(second['gpu_qualified'])
    def test_cancel_and_validator_failure_preserve_current_model(self):
        self.select();before=self.raw()
        def cancel():raise RuntimeError('authored cancellation checkpoint')
        with self.assertRaisesRegex(RuntimeError,'authored cancellation'):self.select(checkpoint=cancel)
        self.assertEqual(self.raw(),before)
        def refuse(*args,**kwargs):raise ValueError('authored model verifier refusal')
        self.pipeline.verify_prepared=refuse
        with self.assertRaisesRegex(ValueError,'verifier refusal'):self.select()
        self.assertEqual(self.raw(),before)
    def test_unknown_modified_and_racing_selection_preserved(self):
        unknown=b'{"human":"keep"}'
        (self.profile/W.SELECTION).write_bytes(unknown)
        with self.assertRaises(W.PreparationError):self.select()
        self.assertEqual(self.raw(),unknown)
        (self.profile/W.SELECTION).unlink();self.select();before=self.raw()
        bad=W.document(before);bad['schema']=999;(self.profile/W.SELECTION).write_bytes(W.encoded(bad))
        with self.assertRaises(W.PreparationError):self.select()
        self.assertEqual(W.document(self.raw())['schema'],999)
        (self.profile/W.SELECTION).write_bytes(before)
        with self.assertRaisesRegex(W.PreparationError,'changed during'):self.select(expected=b'other selection')
        self.assertEqual(self.raw(),before)
        def change():
            with G.locked_directory(self.profile,owner=True) as owner:
                owner.atomic_replace(W.SELECTION,unknown,expected_sha256=sha(before),owner=True)
        with self.assertRaisesRegex(W.PreparationError,'changed while validating'):self.select(checkpoint=change)
        self.assertEqual(self.raw(),unknown)
    def test_locked_hardlinked_selection_fails_without_replacement(self):
        self.select();before=self.raw()
        with G.open_regular(self.profile/W.SELECTION,max_bytes=W.BOUND,owner=True):
            with self.assertRaises(OSError):self.select()
        self.assertEqual(self.raw(),before)
        os.link(self.profile/W.SELECTION,self.root/'authored hardlink')
        with self.assertRaises(OSError):self.select()
        self.assertEqual(self.raw(),before)
    def test_selection_detects_changed_component_and_accepted_gpu_result(self):
        self.select();self.component.write_bytes(b'changed authored sentinel')
        with self.assertRaisesRegex(W.PreparationError,'differs'):W.verify_selection(self.profile,pipeline=self.pipeline,guard=G)
        original=self.pipeline.verify_prepared
        self.pipeline.verify_prepared=lambda *a,**k:dict(original(*a,**k),gpu_qualified=True)
        self.assertTrue(self.select()['gpu_qualified']) # Mocked validator boundary, never a real GPU assertion.
    def test_prepare_cancel_retry_is_fresh_and_activation_is_last(self):
        self.select();before=self.raw();sources={}
        for name in ('sdk','python','script'):
            sources[name]=self.root/name;sources[name].write_bytes(b'authored nonexecuted source boundary')
        tools=self.root/'tools';tools.mkdir();attempts=[];rows=[]
        def run(argv,**kwargs):
            output=Path(argv[argv.index('--output')+1]);attempts.append(output);output.mkdir()
            (output/'receipt.json').write_bytes(b'authored verifier boundary; no model data')
            if len(attempts)==1:raise RuntimeError('authored process cancellation')
        def attempt():
            return W.prepare(installation=self.root,profile=self.profile,sdk=sources['sdk'],tools=tools,component=self.component,
                python=sources['python'],script=sources['script'],cancel_file=self.root/'cancel',pipeline=self.pipeline,guard=G,
                run=run,progress=lambda *row:rows.append(row))
        with self.assertRaisesRegex(RuntimeError,'cancellation'):attempt()
        self.assertEqual(self.raw(),before)
        value=attempt();self.assertNotEqual(attempts[0],attempts[1]);self.assertTrue(attempts[0].exists())
        self.assertEqual(value['asset_root'],str(attempts[1]));self.assertEqual(len(attempts),2)
        with self.assertRaises(W.PreparationError):W.prepare(installation=self.root,profile=self.profile,sdk=sources['sdk'],tools=tools,
            component=self.component,python=sources['python'],script=sources['script'],cancel_file=self.root/'cancel',pipeline=self.pipeline,
            guard=G,run=run,progress=lambda *row:None,output=attempts[-1])
        self.assertEqual(len(attempts),2)

    def tools_boundary(self):
        # Real guards/publication, explicit compiler/validator boundary mock.
        sources={}
        for name in ('archives','support','vulkan_prefix'):
            sources[name]=self.root/name;sources[name].mkdir()
        for name in ('cmake','ninja','compiler'):
            sources[name]=self.root/(name+'.exe');sources[name].write_bytes(b'authored boundary, never executed')
        attempts=[]
        def build(output,*args,**kwargs):
            attempts.append(Path(output));Path(output).mkdir()
            kwargs['cancel']()
            (Path(output)/'receipt.json').write_bytes(b'authored tools validator boundary; no native binaries')
            print('MONADO_FSR4_PROGRESS '+json.dumps({'stage':'compile-tools','current':1,'total':1,'message':'authored execution boundary'}))
        def verify(output):
            return {'schema':1,'status':'passed','kind':'windows-fsr4-tools','output':str(output),'receipt_sha256':sha((Path(output)/'receipt.json').read_bytes())}
        module=types.SimpleNamespace(build=build,verify_tools=verify)
        module.sources=sources
        def build_component(output,**kwargs):
            Path(output).mkdir();(Path(output)/'monado-fsr4.dll').write_bytes(b'authored nonexecuted component validator boundary')
        def verify_component(path):return {'component':str(path),'dll_sha256':sha(Path(path).read_bytes())}
        module.component=types.SimpleNamespace(build=build_component);module.verify_component=verify_component
        def attempt(**extra):
            return W.build_tools(installation=self.root,profile=self.profile,**sources,jobs=2,tools_module=module,
                component_module=module.component,component_verifier=module.verify_component,guard=G,
                progress=lambda *a,**k:None,**extra)
        return attempt,module,attempts
    def test_tools_build_selection_retry_and_changed_receipt_preserve_model(self):
        self.select();model=self.raw();attempt,module,outputs=self.tools_boundary()
        first=attempt();before=(self.profile/W.TOOLS_SELECTION).read_bytes()
        self.assertEqual(W.verify_tools_selection(self.profile,tools_module=module,component_verifier=module.verify_component,guard=G),first)
        original=module.build
        def cancelled(output,*a,**k):original(output,*a,**k);raise RuntimeError('authored tools process cancellation')
        module.build=cancelled
        with self.assertRaisesRegex(RuntimeError,'tools process cancellation'):attempt()
        self.assertEqual((self.profile/W.TOOLS_SELECTION).read_bytes(),before)
        self.assertEqual(self.raw(),model)
        module.build=original;second=attempt();self.assertNotEqual(first['tools'],second['tools'])
        self.assertEqual(len(outputs),3);self.assertEqual(len(set(outputs)),3);self.assertTrue(outputs[1].exists())
        (Path(second['tools'])/'receipt.json').write_bytes(b'changed authored tool receipt')
        with self.assertRaisesRegex(W.PreparationError,'differ'):W.verify_tools_selection(self.profile,tools_module=module,component_verifier=module.verify_component,guard=G)
        self.assertEqual(self.raw(),model)
    def test_tools_unknown_or_locked_selection_never_replaced(self):
        attempt,module,outputs=self.tools_boundary()
        (self.profile/W.TOOLS_SELECTION).write_bytes(b'{"human":"preserve"}')
        with self.assertRaisesRegex(W.PreparationError,'Unknown'):attempt()
        self.assertEqual(outputs,[])
        (self.profile/W.TOOLS_SELECTION).unlink();attempt();before=(self.profile/W.TOOLS_SELECTION).read_bytes()
        with G.open_regular(self.profile/W.TOOLS_SELECTION,max_bytes=W.BOUND,owner=True):
            with self.assertRaises(OSError):attempt()
        self.assertEqual((self.profile/W.TOOLS_SELECTION).read_bytes(),before)
    def test_component_failure_preserves_previous_tools_and_model(self):
        self.select();model=self.raw();attempt,module,outputs=self.tools_boundary();first=attempt()
        before=(self.profile/W.TOOLS_SELECTION).read_bytes()
        def refuse(*args,**kwargs):raise RuntimeError('authored component build failure')
        module.component.build=refuse
        with self.assertRaisesRegex(RuntimeError,'component build failure'):attempt()
        self.assertEqual((self.profile/W.TOOLS_SELECTION).read_bytes(),before)
        self.assertEqual(self.raw(),model);self.assertEqual(len(outputs),2)
        self.assertTrue(Path(first['component']).is_file())
    def test_real_native_component_build_after_authored_tools_boundary(self):
        # The translator/vendor-D3D boundary is authored. Component compilation,
        # PE/export/import/receipt/hash checks and final publication are real.
        self.select();model=self.raw();attempt,module,_=self.tools_boundary()
        load(ROOT/'tools/windows_file_guard.py','windows_file_guard')
        component=load(PORT/'tools/setup_windows_fsr4.py','setup_windows_fsr4')
        pipeline=load(PORT/'tools/prepare_windows_fsr4.py','authored_real_component_validator')
        module.component=component;module.verify_component=pipeline.verify_component
        module.sources['compiler']=Path(os.environ.get('MONADO_NATIVE_TEST_COMPILER',str(PORT/'.venv/Lib/site-packages/ziglang/zig.exe')))
        module.sources['vulkan_prefix']=Path(os.environ.get('MONADO_NATIVE_TEST_PREFIX',json.loads((PORT/'.deps/windows-deps.json').read_bytes())['prefix']))
        selected=attempt();receipt=json.loads(Path(selected['component']).with_name('receipt.json').read_bytes())
        self.assertEqual(receipt['status'],'passed');self.assertEqual(receipt['abi'],2)
        self.assertFalse(receipt['models_prepared']);self.assertEqual(receipt['dll_sha256'],selected['component_sha256'])
        self.assertTrue(Path(selected['component']).with_name('FSR4-component-source.zip').is_file())
        self.assertEqual(self.raw(),model)
    def context(self):
        source=self.root/'source';tool=source/'Monado_Windows/tools';tool.mkdir(parents=True);(source/'tools').mkdir()
        python=self.root/'pinned authored interpreter boundary';(python/'Lib/site-packages').mkdir(parents=True)
        members={
            source/'tools/windows_file_guard.py':(ROOT/'tools/windows_file_guard.py').read_bytes(),
            tool/'fsr4_prepare_worker_windows.py':(PORT/'tools/fsr4_prepare_worker_windows.py').read_bytes(),
            tool/'bootstrap_windows.py':(PORT/'tools/bootstrap_windows.py').read_bytes(),
            tool/'prepare_windows_fsr4.py':b'CHECKED_AUTHORED_CONTEXT_FIXTURE = True\n',
            python/'python.exe':b'authored interpreter identity boundary, never executed'}
        for path,data in members.items():path.write_bytes(data)
        receipt=self.root/'build-receipt.json';path=self.root/'fsr4-preparation-context.json'
        value={'schema':1,'platform':'windows-amd64','status':'available','reason':'',
            'installation':str(self.root),'receipt':str(receipt),'source_root':str(source),'python':str(python/'python.exe'),
            'python_site':str(python/'Lib/site-packages'),'worker':str(tool/'fsr4_prepare_worker_windows.py'),
            'pipeline':str(tool/'prepare_windows_fsr4.py'),'guard':str(source/'tools/windows_file_guard.py'),
            'files':{str(p):{'bytes':len(data),'sha256':sha(data)} for p,data in members.items()}}
        raw=W.encoded(value);path.write_bytes(raw);receipt.write_bytes(W.encoded({'schema':1,'status':'success','preparation_context':str(path),'preparation_context_sha256':sha(raw)}))
        return value,path,sha(raw)
    def test_context_authenticates_before_import_and_holds_members(self):
        context,path,digest=self.context()
        with patch.object(W.sys,'executable',context['python']),patch.object(W,'__file__',context['worker']):
            # Win32 module paths can preserve a different case spelling from
            # the build receipt. Ownership, held handles and hashes still bind
            # the same ordinary file; no resolve/reparse relaxation is used.
            with W.verified_context(Path(str(path).swapcase()),digest) as (_,_,pipeline,_):
                self.assertTrue(pipeline.CHECKED_AUTHORED_CONTEXT_FIXTURE)
                with self.assertRaises(OSError):Path(context['pipeline']).write_bytes(b'changed while leased')
            Path(context['pipeline']).write_bytes(b'changed after lease')
            with self.assertRaisesRegex(W.PreparationError,'member changed'):
                with W.verified_context(path,digest):pass
    def test_context_rejects_hash_receipt_and_import_injection(self):
        context,path,digest=self.context()
        with self.assertRaisesRegex(W.PreparationError,'differs from the checked launcher'):
            with W.verified_context(path,'0'*64):pass
        with patch.object(W.sys,'executable',context['python']),patch.object(W,'__file__',context['worker']):
            injection=Path(context['pipeline']).with_name('unreviewed.py');injection.write_bytes(b'raise RuntimeError("must not import")')
            with self.assertRaisesRegex(W.PreparationError,'Unlisted authored'):
                with W.verified_context(path,digest):pass
            injection.unlink()
            package=injection.parent/'unreviewed_package';package.mkdir();(package/'__init__.py').write_bytes(b'raise RuntimeError("must not import")')
            with self.assertRaisesRegex(W.PreparationError,'Unlisted authored'):
                with W.verified_context(path,digest):pass
            (package/'__init__.py').unlink();package.rmdir()
            Path(context['receipt']).write_bytes(W.encoded({'schema':1,'status':'success','preparation_context':str(path),'preparation_context_sha256':'0'*64}))
            with self.assertRaisesRegex(W.PreparationError,'receipt does not bind'):
                with W.verified_context(path,digest):pass

if __name__=='__main__':unittest.main(verbosity=2)
