"""Native authored context-emission fixtures; no tools/models are executed."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import struct
import tempfile
import unittest
from unittest import mock

PORT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('authored_preparation_context', PORT/'tools/preparation_context_windows.py')
context = importlib.util.module_from_spec(spec)
spec.loader.exec_module(context)


@unittest.skipUnless(os.name == 'nt', 'native Windows ownership/ancestor fixtures')
class NativePreparationContext(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='authored-context-', dir=PORT/'build')
        self.root = Path(self.temp.name)
        self.port = self.root/'source/Monado_Windows'
        (self.port/'tools/package').mkdir(parents=True)
        root_tools = self.port.parent/'tools'
        root_tools.mkdir()
        shutil.copyfile(PORT.parent/'tools/windows_file_guard.py', root_tools/'windows_file_guard.py')
        for name in ('fsr4_prepare_worker_windows.py', 'prepare_windows_fsr4.py', 'package/__init__.py'):
            (self.port/'tools'/name).write_text('# authored context fixture only\n', encoding='utf-8')
        reviewed = {p.relative_to(self.port.parent).as_posix(): {'bytes': p.stat().st_size, 'sha256': hashlib.sha256(p.read_bytes()).hexdigest()}
                    for p in self.port.parent.rglob('*.py')}
        (self.port.parent/'publication-manifest.json').write_text(json.dumps({'files': reviewed}),encoding='utf-8')
        self.home = self.root/'cache/python'
        self.home.mkdir(parents=True)
        files = {}
        for name in ('python.exe', 'python314.zip', 'python314.dll'):
            data = b'authored metadata fixture: not an executable'
            (self.home/name).write_bytes(data)
            files['python/'+name] = {'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
        self.manifest = self.home.parent/'payload-manifest.json'
        raw = json.dumps({'schema': 1, 'platform': 'windows-amd64', 'files': files}).encode()
        self.manifest.write_bytes(raw)
        self.identity = hashlib.sha256(raw).hexdigest()
        self.output = self.root/'build'
        self.output.mkdir()

    def tearDown(self):
        self.temp.cleanup()

    def create(self, identity=None):
        with mock.patch.object(context.sys, 'executable', str(self.home/'python.exe')):
            return context.create(self.port, self.output, installation=self.output,
                                  manifest=self.manifest, manifest_sha256=identity or self.identity)

    def test_available_context_binds_exact_pinned_members_and_recursive_authored_imports(self):
        result = self.create()
        self.assertEqual(result['status'], 'available', result['reason'])
        for name in ('python.exe', 'python314.zip', 'python314.dll'):
            self.assertIn(str(self.home/name), result['files'])
        self.assertIn(str(self.port/'tools/package/__init__.py'), result['files'])
        self.assertEqual(result['receipt'], str(self.output/'build-receipt.json'))

    def test_changed_manifest_or_interpreter_and_extra_bytecode_are_unavailable(self):
        self.assertEqual(self.create('0'*64)['status'], 'unavailable')
        (self.home/'python314.dll').write_bytes(b'changed private fixture')
        self.assertEqual(self.create()['status'], 'unavailable')

    def test_unknown_interpreter_member_is_preserved_and_not_blessed(self):
        extra = self.home/'Lib/site-packages/__pycache__/authored.pyc'
        extra.parent.mkdir(parents=True)
        extra.write_bytes(b'unknown bytecode preserved')
        result = self.create()
        self.assertEqual(result['status'], 'unavailable')
        self.assertEqual(result['files'], {})
        self.assertEqual(extra.read_bytes(), b'unknown bytecode preserved')

    def test_unknown_authored_module_is_preserved_and_not_blessed(self):
        extra = self.port/'tools/unreviewed_authored.py'
        extra.write_text('# preserve this human file\n', encoding='utf-8')
        result = self.create()
        self.assertEqual(result['status'], 'unavailable')
        self.assertEqual(result['files'], {})
        self.assertTrue(extra.is_file())

    def test_optional_manager_is_independent_of_preparation_availability(self):
        manager=self.root/'authored manager.exe';payload=struct.pack('<8sIIQ',b'MONADOP1',1,1,0);payload_sha=hashlib.sha256(payload).hexdigest()
        image=b'MZ'+b'authored never executed'.ljust(4094,b' ')+payload+struct.pack('<8sQQ32s',b'M2BNDL01',4096,len(payload),bytes.fromhex(payload_sha));manager.write_bytes(image)
        options=dict(manager=manager,manager_sha256=hashlib.sha256(image).hexdigest(),manager_payload_sha256=payload_sha,manager_manifest_sha256=self.identity)
        with mock.patch.object(context.sys,'executable',str(self.home/'python.exe')):
            result=context.create(self.port,self.output,installation=self.output,manifest=self.manifest,manifest_sha256='0'*64,**options)
            self.assertEqual(result['status'],'unavailable')
            self.assertEqual(result['manager']['sha256'],options['manager_sha256'])
            manager.unlink()
            result=context.create(self.port,self.output,installation=self.output,manifest=self.manifest,manifest_sha256=self.identity,**options)
            self.assertEqual(result['status'],'available',result['reason'])
            self.assertNotIn('manager',result)


if __name__ == '__main__':
    unittest.main()
