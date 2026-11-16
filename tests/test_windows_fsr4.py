"""Authored optional native FSR4 component/receipt fixtures; no vendor models/game."""
from pathlib import Path
import copy
import ctypes as C
import importlib.util
import json
import os
import hashlib
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import uuid

ROOT=Path(__file__).resolve().parents[1]
PORT=ROOT/'Monado_Windows'
spec=importlib.util.spec_from_file_location('fsr4_windows_setup_fixture',PORT/'tools/setup_windows_fsr4.py')
setup=importlib.util.module_from_spec(spec);spec.loader.exec_module(setup)

class Fsr4ReceiptPolicy(unittest.TestCase):
    def test_complete_model_identity_and_negative_cases(self):
        lock=json.loads((PORT/'fsr4-windows.lock.json').read_text(encoding='utf8'))
        record={'schema':1,'status':'passed','model':'int8_lite','model_identity':lock['model_identity'],'sdk':lock['sdk'],
          'resources':{'lite/'+tier+'/'+name:{'bytes':27392 if name.endswith('.bin') else 20,'sha256':'0'*64}
          for tier in lock['sets'] for name in [*[p+'.spv' for p in lock['passes']],'initializer.bin']}}
        self.assertEqual(len(setup.verify_model_record(record,lock)),52)
        for field,value in [('schema',2),('status','cancelled'),('model','fp8'),('model_identity','0'*64),('sdk',{})]:
            changed=copy.deepcopy(record);changed[field]=value
            with self.subTest(field=field),self.assertRaises(ValueError):setup.verify_model_record(changed,lock)
        for name,value in [('lite/t1080_m0/initializer.bin',{'bytes':27391,'sha256':'0'*64}),
                           ('lite/t2160_m1/rcas.spv',{'bytes':19,'sha256':'0'*64}),
                           ('lite/t2160_m1/rcas.spv',{'bytes':24,'sha256':'x'*64})]:
            changed=copy.deepcopy(record);changed['resources'][name]=value
            with self.subTest(name=name),self.assertRaises(ValueError):setup.verify_model_record(changed,lock)
        changed=copy.deepcopy(record);changed['resources'].pop('lite/t1080_m0/pass1.spv')
        with self.assertRaises(ValueError):setup.verify_model_record(changed,lock)
        changed=copy.deepcopy(record);changed['resources']['extra.spv']={'bytes':20,'sha256':'0'*64}
        with self.assertRaises(ValueError):setup.verify_model_record(changed,lock)
    def test_duplicate_receipt_keys_rejected(self):
        with self.assertRaises(ValueError):setup.document(b'{"schema":1,"schema":2}')

@unittest.skipUnless(os.name=='nt','Native AMD64 Win32 component')
class NativeFsr4Component(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        from native_renderer_support import require_native_graphics
        # setup.build below exercises the production default dependency paths;
        # a partial/alternate configured setup must fail rather than skip.
        require_native_graphics(production_defaults=True)
        cls.output=PORT/'.deps/fsr4/build'/('authored-fixture-'+uuid.uuid4().hex)
        cls.receipt=setup.build(cls.output)
        sys.path.insert(0,str(ROOT/'tests'))
        from native_renderer_support import compiler_command,compiler_environment,graphics_prefix
        cls.compiler=compiler_command();cls.environment=compiler_environment();cls.prefix=graphics_prefix()

    def compile(self,source,binary,extra=()):
        args=self.compiler+['-std=gnu11','-O1','-UNDEBUG','-march=x86_64_v2','-ffp-contract=off','-fno-fast-math',
             '-I'+str(PORT/'runtime'),'-I'+str(self.prefix/'include'),str(source),*extra,'-o',str(binary)]
        result=subprocess.run(args,env=self.environment,capture_output=True,text=True,timeout=120)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)

    def test_actual_source_only_dll_abi_and_invalid_arguments(self):
        output=self.output;receipt=self.receipt
        self.assertEqual(receipt['status'],'passed');self.assertFalse(receipt['models_prepared'])
        module=C.WinDLL(str(output/setup.NAME),winmode=0x900)
        get=module.monado_fsr4_get_api;get.argtypes=[C.c_uint32];get.restype=C.c_void_p
        self.assertFalse(get(999));address=get(2);self.assertTrue(address)
        class Api(C.Structure):
            _fields_=[('abi',C.c_uint32),('create',C.c_void_p),('destroy',C.c_void_p),('build',C.c_void_p),('record',C.c_void_p),('error',C.c_void_p)]
        api=Api.from_address(address);self.assertEqual(api.abi,2)
        create=C.CFUNCTYPE(C.c_void_p,C.c_void_p,C.c_void_p,C.c_char_p)(api.create)
        build=C.CFUNCTYPE(C.c_int,C.c_void_p,C.c_uint32,C.c_uint32,C.c_uint32)(api.build)
        record=C.CFUNCTYPE(C.c_int,C.c_void_p,C.c_void_p)(api.record)
        error=C.CFUNCTYPE(C.c_char_p,C.c_void_p)(api.error)
        destroy=C.CFUNCTYPE(None,C.c_void_p)(api.destroy)
        self.assertFalse(create(None,None,b'authored-missing-assets'))
        self.assertFalse(create(None,None,None));self.assertFalse(build(None,320,180,0))
        self.assertFalse(build(1,0,180,0));self.assertFalse(record(None,None))
        self.assertEqual(error(None),b'null FSR4 context');destroy(None)
        # No valid physical/device/model handle was supplied; this proves the
        # actual native exported ABI/failure boundary, never a GPU dispatch pass.
        import _ctypes
        _ctypes.FreeLibrary(module._handle)

    def test_actual_optional_device_contract_and_size_boundaries(self):
        source=(PORT/'runtime/nvn/vulkan/device.c').read_text(encoding='utf8')
        first=source.index('static void fsr4_device_prepare(');last=source.index('\nstatic void create_device',first)
        with tempfile.TemporaryDirectory(prefix='monado-fsr4-caps-') as temporary:
            directory=Path(temporary);(directory/'fsr4_device_source.h').write_text(source[first:last],encoding='utf8')
            binary=directory/'caps.exe'
            self.compile(PORT/'tests/native/test_fsr4_windows_capabilities.c',binary,['-I'+str(directory)])
            result=subprocess.run([binary],capture_output=True,text=True,timeout=20)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr);self.assertIn('capability15cases',result.stdout)

    def test_actual_dispatch_contract_with_component_boundary_mocks(self):
        with tempfile.TemporaryDirectory(prefix='monado-fsr4-dispatch-') as temporary:
            binary=Path(temporary)/'dispatch.exe'
            self.compile(PORT/'tests/native/test_fsr4_windows_dispatch.c',binary,['-lbcrypt','-ladvapi32'])
            result=subprocess.run([binary],capture_output=True,text=True,timeout=20)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            self.assertIn('ring retirement',result.stdout)

    def test_actual_native_model_configuration_and_dll_failure_boundaries(self):
        lock=json.loads((PORT/'fsr4-windows.lock.json').read_text(encoding='utf8'))
        with tempfile.TemporaryDirectory(prefix='monado-fsr4-authored-') as temporary:
            directory=Path(temporary);assets=directory/'authored models-雪-$()-`';assets.mkdir()
            (assets/'models.id').write_bytes((lock['model_identity']+'\n').encode('ascii'))
            # Authored five-word headers/zero weights exercise file contracts
            # only. They are never recorded on a GPU or promoted as SDK models.
            for tier in lock['sets']:
                for name in [*[p+'.spv' for p in lock['passes']],'initializer.bin']:
                    target=assets/'lite'/tier/name;target.parent.mkdir(parents=True,exist_ok=True)
                    data=b'\0'*27392 if name.endswith('.bin') else struct.pack('<5I',0x07230203,0x10300,0,1,0)
                    target.write_bytes(data);Path(str(target)+'.sha256').write_bytes((hashlib.sha256(data).hexdigest()+'\n').encode('ascii'))
            component=directory/'native component-雪.dll';shutil.copyfile(self.output/setup.NAME,component)
            side=Path(str(component)+'.sha256');side.write_bytes((hashlib.sha256(component.read_bytes()).hexdigest()+'\n').encode('ascii'))
            binary=directory/'configuration.exe'
            self.compile(PORT/'tests/native/test_fsr4_windows_config.c',binary,['-municode','-lbcrypt','-ladvapi32'])
            def run(mode):
                result=subprocess.run([binary,assets,component,mode],capture_output=True,text=True,timeout=30)
                self.assertEqual(result.returncode,0,result.stdout+result.stderr)
                return result.stdout
            self.assertIn('owned52-file',run('good'))
            target=assets/'lite/t1080_m0/prepass.spv';original=target.read_bytes();target.write_bytes(b'changed')
            self.assertIn('rejection',run('bad'));target.write_bytes(original)
            alias=directory/'authored-hardlink.spv';os.link(target,alias)
            self.assertIn('rejection',run('bad'));alias.unlink()
            saved=side.read_bytes();side.write_bytes(b'0'*64+b'\n')
            self.assertIn('rejection',run('bad'));side.write_bytes(saved)
            (assets/'models.id').write_bytes(b'0'*64+b'\n');self.assertIn('rejection',run('bad'))
            (assets/'models.id').write_bytes((lock['model_identity']+'\r\n').encode('ascii'));self.assertIn('rejection',run('bad'))
            (assets/'models.id').write_bytes((lock['model_identity']+'\n').encode('ascii'))
            source=directory/'bad-api.c';source.write_text('__declspec(dllexport) const void* monado_fsr4_get_api(unsigned abi){(void)abi;return 0;}\n',encoding='ascii')
            self.compile(source,component,['-shared'])
            side.write_bytes((hashlib.sha256(component.read_bytes()).hexdigest()+'\n').encode('ascii'))
            self.assertIn('wrong DLL ABI rejection',run('bad-abi'))

if __name__=='__main__':unittest.main()
