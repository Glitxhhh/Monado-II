"""Pinned download/review fixtures; no NVIDIA code, network or agreement use."""
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import sys
import time
import unittest
import uuid
import zipfile

PORT=Path(__file__).resolve().parents[2]
def load(path,name):
    spec=importlib.util.spec_from_file_location(name,path);module=importlib.util.module_from_spec(spec);sys.modules[name]=module;spec.loader.exec_module(module);return module
D=load(PORT/'tools/windows_dlss_download.py','authored_dlss_download')

def archive(extra=(),reflex=None):
    target=io.BytesIO()
    with zipfile.ZipFile(target,'w',compression=zipfile.ZIP_DEFLATED) as z:
        for name in D.MEMBERS:z.writestr(name,reflex if reflex is not None and name.endswith('reflex.license.txt') else b'Authored license; no vendor terms.\n' if name.endswith('.txt') else b'MZ authored DLL fixture never executed')
        for name,data in extra:z.writestr(name,data)
    return target.getvalue()

def pin(data):return {'version':'authored','url':'https://example.invalid/no-network','bytes':len(data),'sha256':hashlib.sha256(data).hexdigest()}

class Inventory(unittest.TestCase):
    def test_only_production_members_selected(self):
        data=archive([('bin/x64/development/nvngx_dlss.dll',b'not selected'),('samples/never.exe',b'never selected')])
        with zipfile.ZipFile(io.BytesIO(data)) as z:self.assertEqual(set(D.checked_zip(z)),set(D.MEMBERS))
    def test_unsafe_or_alias_table_rejected_before_extraction(self):
        for name in ('../outside','/absolute','x\\outside','CON.txt','bin/x64/SL.COMMON.DLL'):
            data=archive([(name,b'x')])
            if '\\' in name:data=data.replace(b'x/outside',b'x\\outside') # ZipInfo normalizes the host separator during fixture creation.
            with self.subTest(name=name),zipfile.ZipFile(io.BytesIO(data)) as z,self.assertRaises(ValueError):D.checked_zip(z)
    def test_missing_production_member_not_replaced_by_development(self):
        data=io.BytesIO()
        with zipfile.ZipFile(data,'w') as z:
            for name in D.MEMBERS:
                if name.endswith('nvngx_dlss.dll'):name='bin/x64/development/nvngx_dlss.dll'
                z.writestr(name,b'authored')
        with zipfile.ZipFile(io.BytesIO(data.getvalue())) as z,self.assertRaises(ValueError):D.checked_zip(z)

@unittest.skipUnless(os.name=='nt','actual native current-owner file guards')
class Native(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.guard=load(PORT.parent/'tools/windows_file_guard.py','authored_dlss_download_guard')
        cls.folder=PORT/'runs'/('dlss-download-'+str(time.time_ns())+'-'+uuid.uuid4().hex[:8]);cls.folder.mkdir()
    def setUp(self):
        self.state=self.folder/self._testMethodName;self.state.mkdir()
    def test_review_output_never_activates_registry_profile_or_runtime(self):
        (self.state/'human.txt').write_bytes(b'preserved');data=archive();events=[]
        with self.guard.locked_directory(self.state,owner=True) as state:
            source=Path(D.prepare(state,archive_source=io.BytesIO(data),_pin=pin(data),progress=lambda *v:events.append(v)))
        self.assertEqual(set(p.name for p in source.iterdir()),set(D.MEMBERS.values())|{'download-receipt.json'})
        result=json.loads((source/'download-receipt.json').read_text());self.assertEqual(result['status'],'downloaded-unselected')
        self.assertFalse((self.state/'checked-installations.json').exists());self.assertFalse((source/'build').exists());self.assertEqual((self.state/'human.txt').read_bytes(),b'preserved');self.assertTrue(events)
    def test_cancellation_preserves_previous_selection_and_retry_is_fresh(self):
        data=archive();(self.state/'checked-installations.json').write_bytes(b'authored previous selection');calls=0
        def checkpoint():
            nonlocal calls
            calls+=1
            if calls==5:raise ValueError('authored cancel')
        with self.guard.locked_directory(self.state,owner=True) as state:
            with self.assertRaisesRegex(ValueError,'authored cancel'):D.prepare(state,archive_source=io.BytesIO(data),_pin=pin(data),checkpoint=checkpoint)
            first=set(self.state.iterdir());source=Path(D.prepare(state,archive_source=io.BytesIO(data),_pin=pin(data)))
        self.assertNotIn(source.parent,first);self.assertEqual((self.state/'checked-installations.json').read_bytes(),b'authored previous selection')
    def test_wrong_sha_or_size_does_not_produce_reviewable_sdk(self):
        data=archive()
        with self.guard.locked_directory(self.state,owner=True) as state:
            for bad in (dict(pin(data),sha256='0'*64),dict(pin(data),bytes=len(data)-1)):
                with self.assertRaises(ValueError):D.prepare(state,archive_source=io.BytesIO(data),_pin=bad)
        self.assertFalse(any(self.state.rglob('download-receipt.json')))
    def test_cp1252_dependent_license_preserves_original_bytes_and_identity(self):
        original=b'Authored dependent \x93license\x94; no NVIDIA agreement.\r\n';data=archive(reflex=original)
        with self.guard.locked_directory(self.state,owner=True) as state:
            source=Path(D.prepare(state,archive_source=io.BytesIO(data),_pin=pin(data)))
        self.assertEqual((source/'reflex.license.txt').read_bytes(),original)
        receipt=json.loads((source/'download-receipt.json').read_text());self.assertEqual(receipt['files']['reflex.license.txt']['sha256'],hashlib.sha256(original).hexdigest())

if __name__=='__main__':unittest.main()
