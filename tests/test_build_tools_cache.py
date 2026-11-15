"""Hash-pinned local build-tool cache boundaries; no network or tools execute."""
from pathlib import Path
import hashlib
import importlib.util
import io
import json
import stat
import subprocess
import tempfile
import unittest
from unittest import mock
import zipfile

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('monado_buildtools_guard',ROOT/'Monado_Linux/scripts/setup_linux_build_tools.py')
tools=importlib.util.module_from_spec(spec);spec.loader.exec_module(tools)

class BuildToolsCache(unittest.TestCase):
    def setUp(self):
        self.t=tempfile.TemporaryDirectory(prefix='monado-tool-cache-');self.addCleanup(self.t.cleanup)
        self.root=Path(self.t.name);self.lock=self.root/'tools.json'
        self.addCleanup(mock.patch.stopall)
        mock.patch.object(tools,'ROOT',self.root).start();mock.patch.object(tools,'LOCK',self.lock).start()
        self.execute=mock.patch.object(tools.subprocess,'run',return_value=subprocess.CompletedProcess([],0,'fixture 1.0\n','')).start()
    def prepare(self,name='fixture/bin/tool',symlink=False):
        data=io.BytesIO()
        with zipfile.ZipFile(data,'w') as z:
            info=zipfile.ZipInfo(name);info.external_attr=((stat.S_IFLNK|0o777) if symlink else (stat.S_IFREG|0o755))<<16
            z.writestr(info,b'authored tool fixture')
        raw=data.getvalue();row={'name':'fixture','version':'1.0','filename':'tool.whl','url':'https://files.pythonhosted.org/fixture',
                                 'sha256':hashlib.sha256(raw).hexdigest(),'bytes':len(raw),'binary':'fixture/bin/tool'}
        self.lock.write_text(json.dumps({'tools':[row]}))
        mock.patch.object(tools.urllib.request,'urlopen',side_effect=lambda *a,**kw:io.BytesIO(raw)).start()
    def test_roundtrip_reuses_verified_cache_and_preserves_all_files(self):
        self.prepare();a=tools.setup();b=tools.setup();self.assertEqual(a,b);self.assertEqual(self.execute.call_count,2)
        self.assertEqual(a['fixture']['version'],'1.0')
    def test_poisoned_receipt_cannot_bless_modified_executable(self):
        self.prepare();tools.setup();target=self.root/'.deps/build-tools/packages/fixture-1.0'
        (target/'fixture/bin/tool').write_bytes(b'modified executable')
        r=target/'.monado-tool.json';d=json.loads(r.read_text());d['files']={};r.write_text(json.dumps(d))
        with self.assertRaisesRegex(ValueError,'build tool changed'):tools.setup()
        self.assertEqual(self.execute.call_count,1)
    def test_cached_archive_tamper_is_rejected(self):
        self.prepare();tools.setup();p=self.root/'.deps/build-tools/downloads/tool.whl';p.write_bytes(b'bad')
        with self.assertRaisesRegex(ValueError,'differs from lock'):tools.setup()
        self.assertEqual(self.execute.call_count,1)
    def test_parent_escape_archive_never_writes_outside_private_staging(self):
        self.prepare('../escaped')
        with self.assertRaisesRegex(ValueError,'unsafe'):tools.setup()
        self.assertFalse((self.root/'.deps/build-tools/packages/escaped').exists());self.execute.assert_not_called()
    def test_symlink_archive_rejected(self):
        self.prepare(symlink=True)
        with self.assertRaisesRegex(ValueError,'unsafe'):tools.setup()
        self.execute.assert_not_called()
    def test_unknown_existing_directory_never_overwritten(self):
        self.prepare();target=self.root/'.deps/build-tools/packages/fixture-1.0';target.mkdir(parents=True)
        (target/'human').write_text('keep')
        with self.assertRaisesRegex(ValueError,'unowned'):tools.setup()
        self.assertEqual((target/'human').read_text(),'keep');self.execute.assert_not_called()
