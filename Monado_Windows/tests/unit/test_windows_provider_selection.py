"""Native local provider packaging fixtures; authored DLLs are never loaded."""
from __future__ import annotations
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time
import unittest
import uuid

PORT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('authored_provider_selection', PORT/'tools/windows_provider_selection.py')
P = importlib.util.module_from_spec(spec);spec.loader.exec_module(P)


@unittest.skipUnless(os.name == 'nt', 'native Windows provider packaging contracts')
class Native(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.evidence=PORT/'runs'/('windows-providers-'+str(time.time_ns())+'-'+uuid.uuid4().hex[:8]);cls.evidence.mkdir()
        exports=set().union(*P.EXPORTS.values())|{'slGetPluginFunction','authored_fixture'}
        code='\n'.join('__declspec(dllexport) int '+name+'(void){return 0;}' for name in sorted(exports))+'\n'
        source=cls.evidence/'provider.c';source.write_text(code)
        cls.dll=cls.evidence/'authored-provider.dll'
        env=dict(os.environ,ZIG_GLOBAL_CACHE_DIR=str(cls.evidence/'zig-global'),ZIG_LOCAL_CACHE_DIR=str(cls.evidence/'zig-local'))
        command=[sys.executable,'-B','-m','ziglang','cc','-shared','-O1','-march=x86_64_v2',str(source),'-o',str(cls.dll)]
        result=subprocess.run(command,env=env,capture_output=True,text=True,timeout=120)
        (cls.evidence/'compile.log').write_text(result.stdout+result.stderr)
        if result.returncode:raise RuntimeError(result.stderr)
        cls.data=cls.dll.read_bytes();cls.cases=[]

    @classmethod
    def tearDownClass(cls):
        paths=[PORT/'tools/windows_provider_selection.py',PORT/'tools/pe_symbols_windows.py',PORT.parent/'tools/windows_file_guard.py',Path(__file__)]
        (cls.evidence/'receipt.json').write_text(json.dumps({'scope':'authored PE/guard/staging contracts, no vendor execution or GPU qualification',
            'sources':{str(p.relative_to(PORT.parent)):hashlib.sha256(p.read_bytes()).hexdigest() for p in paths},'cases':cls.cases},indent=2)+'\n')
        print('Native provider evidence: '+str(cls.evidence),flush=True)

    def setUp(self):
        self.folder=self.evidence/(self._testMethodName+'-'+uuid.uuid4().hex[:8]);self.folder.mkdir()

    def tearDown(self):
        result=getattr(getattr(self,'_outcome',None),'result',None)
        failures=result.failures+result.errors if result else [(self,'missing result')]
        self.cases.append({'case':self._testMethodName,'passed':not any(test is self for test,_ in failures)})

    def sdk(self,kind='fidelityfx',nested=False):
        root=self.folder/(kind+' SDK 測試');root.mkdir()
        binaries=root/'bin/x64' if nested else root;binaries.mkdir(parents=True,exist_ok=True)
        for name in P.REQUIRED[kind]:(binaries/name).write_bytes(self.data)
        (root/'LICENSE.txt').write_text('Authored synthetic fixture license. No vendor binaries.\n')
        if kind=='streamline':(binaries/'nvngx_dlss.license.txt').write_text('Authored synthetic fixture; no NVIDIA agreement.\n')
        return root

    def output(self):
        out=self.folder/'fresh output 測試';out.mkdir();(out/'build').mkdir();return out

    def test_complete_sdk_stage_is_receipted_and_unknown_source_is_not_copied(self):
        a=self.sdk();b=self.sdk('streamline',True);(b/'unknown.dll').write_bytes(b'never copied')
        out=self.output();events=[]
        result=P.stage(out,fidelityfx=a,streamline=b,progress=lambda *row:events.append(row))
        self.assertEqual(set(result['selection']),{'fidelityfx','streamline'})
        self.assertEqual({p.name for p in (out/'build').iterdir()},set(P.REQUIRED['fidelityfx'])|set(P.REQUIRED['streamline']))
        for name,item in result['files'].items():self.assertEqual(P.identity((out/name).read_bytes()),item)
        self.assertTrue(events);self.assertEqual(json.loads((out/'windows-providers.json').read_text())['qualification'],'local-integrity-only')
        self.assertEqual((b/'unknown.dll').read_bytes(),b'never copied')

    def test_missing_dll_license_or_exports_fails_before_staging(self):
        for failure in ('dll','license','exports'):
            with self.subTest(failure=failure):
                case=self.folder/failure;case.mkdir();old=self.folder;self.folder=case
                root=self.sdk();out=self.output()
                if failure=='dll':(root/'amd_fidelityfx_vk.dll').unlink()
                elif failure=='license':(root/'LICENSE.txt').unlink()
                else:
                    data=bytearray(self.data);at=data.index(b'ffxConfigure\0');data[at:at+12]=b'badConfigure';(root/'amd_fidelityfx_vk.dll').write_bytes(data)
                with self.assertRaises((OSError,ValueError)):P.stage(out,fidelityfx=root)
                self.assertEqual(list((out/'build').iterdir()),[]);self.assertFalse((out/'windows-providers.json').exists())
                self.folder=old

    def test_existing_destination_is_preserved_even_when_bytes_match(self):
        root=self.sdk();out=self.output();existing=out/'build/amd_fidelityfx_vk.dll';existing.write_bytes(self.data)
        with self.assertRaisesRegex(ValueError,'already exists'):P.stage(out,fidelityfx=root)
        self.assertEqual(existing.read_bytes(),self.data);self.assertFalse((out/'windows-providers.json').exists())

    def test_unimported_low_latency_sibling_is_not_staged_or_licensed(self):
        root=self.sdk('streamline');(root/'NvLowLatencyVk.dll').write_bytes(self.data)
        result=P.stage(self.output(),streamline=root)
        self.assertNotIn('nvlowlatencyvk.dll',result['selection']['streamline']['stage'])
        self.assertFalse(any('reflex' in n.lower() for n in result['files']))

    def test_imported_low_latency_helper_requires_its_exact_license(self):
        root=self.sdk('streamline');source=self.folder/'low-latency.c';source.write_text('__declspec(dllexport) int authored_latency(void){return 7;}\n')
        dll=self.folder/'NvLowLatencyVk.dll';library=self.folder/'NvLowLatencyVk.lib'
        env=dict(os.environ,ZIG_GLOBAL_CACHE_DIR=str(self.evidence/'zig-global'),ZIG_LOCAL_CACHE_DIR=str(self.evidence/'zig-local'))
        def compile(arguments):
            result=subprocess.run([sys.executable,'-B','-m','ziglang','cc','-shared','-O1',*map(str,arguments)],env=env,capture_output=True,text=True,timeout=120)
            self.assertEqual(result.returncode,0,result.stderr)
        compile([source,'-o',dll,'-Wl,--out-implib,'+str(library)])
        source=self.folder/'common.c';source.write_text('__declspec(dllimport) int authored_latency(void);\n__declspec(dllexport) int authored_common(void){return authored_latency();}\n')
        compile([source,library,'-o',root/'sl.common.dll']);(root/'NvLowLatencyVk.dll').write_bytes(dll.read_bytes())
        with self.assertRaises((OSError,ValueError)):P.stage(self.output(),streamline=root)
        (root/'reflex.license.txt').write_bytes(b'Authored dependent license; no NVIDIA agreement.\n')
        licensed=self.folder/'fresh licensed output';licensed.mkdir();(licensed/'build').mkdir()
        result=P.stage(licensed,streamline=root)
        self.assertIn('nvlowlatencyvk.dll',result['selection']['streamline']['stage'])
        self.assertIn('providers/streamline/reflex.license.txt',result['files'])

    def test_original_changed_or_ambiguous_requires_explicit_reselection(self):
        root=self.sdk();original=P.inspect_selection({'fidelityfx':root})
        with (root/'LICENSE.txt').open('ab') as f:f.write(b'changed')
        with self.assertRaisesRegex(ValueError,'changed'):P.inspect_selection({'fidelityfx':root},expected=original)
        (root/'bin').mkdir();(root/'bin/amd_fidelityfx_vk.dll').write_bytes(self.data)
        with self.assertRaisesRegex(ValueError,'Ambiguous'):P.inspect_selection({'fidelityfx':root})

    def test_hardlink_and_junction_refused(self):
        root=self.sdk();leaf=root/'amd_fidelityfx_vk.dll';os.link(leaf,self.folder/'other-link.dll')
        with self.assertRaises(OSError):P.inspect_selection({'fidelityfx':root})
        leaf.unlink();leaf.write_bytes(self.data)
        import _winapi
        link=self.folder/'junction';_winapi.CreateJunction(str(root),str(link))
        with self.assertRaises(OSError):P.inspect_selection({'fidelityfx':link})

    def test_source_file_and_parent_are_held_through_staging(self):
        root=self.sdk()
        with P.checked({'fidelityfx':root}):
            with self.assertRaises(OSError):(root/'amd_fidelityfx_vk.dll').write_bytes(b'changed')
            with self.assertRaises(OSError):root.rename(root.with_name('moved'))
        self.assertEqual((root/'amd_fidelityfx_vk.dll').read_bytes(),self.data)

    def test_cancelled_fresh_stage_has_no_completion_record(self):
        root=self.sdk('streamline');out=self.output();calls=[]
        class Cancelled(ValueError):pass
        def report(*row):calls.append(row)
        def cancel():
            if calls:raise Cancelled('authored cancellation')
        with self.assertRaises(Cancelled):P.stage(out,streamline=root,cancel=cancel,progress=report)
        self.assertFalse((out/'windows-providers.json').exists());self.assertTrue((root/'sl.interposer.dll').exists())
        fresh=self.folder/'retry';fresh.mkdir();(fresh/'build').mkdir();P.stage(fresh,streamline=root)
        self.assertTrue((fresh/'windows-providers.json').is_file())

    def test_unreviewed_transitive_dependency_and_exe_kind_rejected(self):
        guard,Image=P.dependencies();root=self.sdk();path=root/'amd_fidelityfx_vk.dll'
        image=Image(path);original=image.imports
        image.imports=lambda:dict(original(),**{'unknown-local.dll':['fixture']})
        self.assertIn('unknown-local.dll',P.pe_contract(image,path.name))
        from unittest.mock import patch
        with patch.object(P,'dependencies',return_value=(guard,lambda path:image)):
            with self.assertRaisesRegex(ValueError,'Unreviewed'):P.inspect_selection({'fidelityfx':root})
        data=bytearray(self.data);pe=int.from_bytes(data[0x3c:0x40],'little');data[pe+22:pe+24]=(int.from_bytes(data[pe+22:pe+24],'little')&~0x2000).to_bytes(2,'little');path.write_bytes(data)
        with self.assertRaisesRegex(ValueError,'not an AMD64 DLL'):P.inspect_selection({'fidelityfx':root})

    def system_import_fixture(self):
        guard,Image=P.dependencies();root=self.sdk()
        def image(path):
            result=Image(path)
            if Path(path).name=='amd_fidelityfx_vk.dll':
                original=result.imports;result.imports=lambda:dict(original(),**{'msvcp140.dll':['authored_import_boundary']})
            return result
        return root,guard,image

    def test_known_installed_system_crt_is_checked_without_copying(self):
        from unittest.mock import patch
        root,guard,image=self.system_import_fixture();out=self.output()
        with patch.object(P,'dependencies',return_value=(guard,image)):
            selected=P.stage(out,fidelityfx=root)['selection']['fidelityfx']
        self.assertEqual(selected['system']['msvcp140.dll']['origin'],'installed-system-runtime')
        self.assertEqual(Path(selected['system']['msvcp140.dll']['path']).parent,P.system_directory())
        self.assertNotIn('msvcp140.dll',selected['stage']);self.assertFalse((out/'build/msvcp140.dll').exists())

    def test_missing_system_crt_gives_manual_runtime_guidance(self):
        from unittest.mock import patch
        root,guard,image=self.system_import_fixture();system=self.folder/'empty system boundary';system.mkdir()
        with patch.object(P,'dependencies',return_value=(guard,image)),patch.object(P,'system_directory',return_value=system):
            with self.assertRaisesRegex(ValueError,'official runtime manually'):P.inspect_selection({'fidelityfx':root})

    def test_current_user_file_is_not_an_installed_system_runtime(self):
        from unittest.mock import patch
        root,guard,image=self.system_import_fixture();system=self.folder/'authored non-OS folder';system.mkdir()
        (system/'msvcp140.dll').write_bytes(self.data)
        with patch.object(P,'dependencies',return_value=(guard,image)),patch.object(P,'system_directory',return_value=system):
            with self.assertRaisesRegex(ValueError,'not owned by Windows'):P.inspect_selection({'fidelityfx':root})


if __name__=='__main__':unittest.main()
