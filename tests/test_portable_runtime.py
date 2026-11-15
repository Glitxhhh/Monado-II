"""Game-free runtime transport: real authored ELF, no game or vendor execution."""
from pathlib import Path
import hashlib
import importlib.util
import io
import json
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('runtime_bundle_under_test',ROOT/'tools/portable_runtime.py')
bundle=importlib.util.module_from_spec(spec);spec.loader.exec_module(bundle)
ABI='a'*64;SOURCE='b'*64

@unittest.skipUnless(sys.platform=='linux','Linux authored runtime bundle')
class PortableRuntime(unittest.TestCase):
    def setUp(self):
        self.t=tempfile.TemporaryDirectory(prefix='monado-runtime-bundle-');self.addCleanup(self.t.cleanup)
        self.base=Path(self.t.name);self.build=self.base/'build';self.build.mkdir()
        self.binary=self.build/'xcde-linux';cc=ROOT/'Monado_Linux/.deps/bin/clang-cc'
        if not cc.is_file():self.skipTest('private Clang needed')
        source=self.base/'authored.c';source.write_text('int main(void){return 0;}\n')
        p=subprocess.run([str(cc),str(source),'-o',str(self.binary)],capture_output=True,text=True,timeout=60)
        self.assertEqual(p.returncode,0,p.stderr)
        self.receipt={'schema':1,'status':'passed','split_game':True,'gamecode_abi':{'id':ABI},
          'executable':{'path':str(self.binary),'sha256':bundle.digest(self.binary)},
          'sources_before':{'runtime/authored.c':'c'*64},'sources_after':{'runtime/authored.c':'c'*64},
          'objects':{'runtime/authored.c':{'path':'authored.o','source_sha256':'c'*64,'object_sha256':'d'*64},
                     'generated/main.c':{'path':'generated-main.o'}},
          'generated':{'generated/main.c':'e'*64},'input':{'path':'PRIVATE INPUT'},
          'commands':[{'name':'link-graphics','exit':0,'argv':['clang','authored.o','-o',str(self.binary)]}],
          'runtime_resources':{},'dlss_enabled':False,'fsr3_enabled':False,'framegen_enabled':False}
        self.save()
    def save(self):(self.build/'receipt.json').write_text(json.dumps(self.receipt))
    def export(self):
        target=self.base/'runtime.tar.gz';m=bundle.export_bundle(self.binary,target,SOURCE);return target,m
    def mutate(self,archive,fn):
        with tarfile.open(archive,'r:gz') as t:rows=[(m,t.extractfile(m).read()) for m in t]
        p=self.base/'mutant.tar.gz'
        with tarfile.open(p,'w:gz') as t:
            for m,data in rows:
                name,data=fn(m.name,data);info=tarfile.TarInfo(name);info.size=len(data);t.addfile(info,io.BytesIO(data))
        return p
    def test_roundtrip_excludes_game_and_private_build_inputs(self):
        (self.build/'lasgame.so').write_bytes(b'PRIVATE LOCAL GAME CODE')
        self.receipt['runtime_resources']['lasgame.so']={'sha256':bundle.digest(self.build/'lasgame.so')};self.save()
        archive,m=self.export();out=self.base/'imported'
        accepted=bundle.import_bundle(archive,out,m['archive_sha256'],ABI,SOURCE)
        self.assertEqual(accepted['abi_id'],ABI);self.assertEqual(bundle.digest(out/'xcde-linux'),bundle.digest(self.binary))
        self.assertFalse((out/'lasgame.so').exists())
        self.assertNotIn('PRIVATE INPUT',(out/'receipt.json').read_text())
        self.assertNotIn('generated/main.c',(out/'receipt.json').read_text())
    def test_external_hash_source_and_abi_required(self):
        archive,m=self.export()
        for sha,abi,source in [('0'*64,ABI,SOURCE),(m['archive_sha256'],'0'*64,SOURCE),(m['archive_sha256'],ABI,'0'*64)]:
            with self.assertRaises(ValueError):bundle.import_bundle(archive,self.base/'out',sha,abi,source)
        self.assertFalse((self.base/'out').exists())
    def test_changed_member_and_parent_escape_rejected(self):
        archive,m=self.export()
        bad=self.mutate(archive,lambda name,data:(name,data+b'changed') if name=='xcde-linux' else (name,data))
        with self.assertRaises(ValueError):bundle.import_bundle(bad,self.base/'out',bundle.digest(bad),ABI,SOURCE)
        bad.unlink()
        bad=self.mutate(archive,lambda name,data:('../escaped',data) if name=='xcde-linux' else (name,data))
        with self.assertRaises(ValueError):bundle.import_bundle(bad,self.base/'out',bundle.digest(bad),ABI,SOURCE)
        self.assertFalse((self.base/'escaped').exists())
    def test_existing_destination_and_symlink_never_overwritten(self):
        archive,m=self.export();out=self.base/'out';out.mkdir();(out/'human').write_text('keep')
        with self.assertRaises(ValueError):bundle.import_bundle(archive,out,m['archive_sha256'],ABI,SOURCE)
        self.assertEqual((out/'human').read_text(),'keep')
        link=self.base/'link';link.symlink_to(archive)
        with self.assertRaises(ValueError):bundle.import_bundle(link,self.base/'new',m['archive_sha256'],ABI,SOURCE)
    def test_game_link_provenance_refused(self):
        self.receipt['commands'][0]['argv'].insert(1,'generated-main.o');self.save()
        with self.assertRaisesRegex(ValueError,'game object'):self.export()
    def test_missing_or_stripped_symbol_table_refused(self):
        p=subprocess.run(['/usr/bin/strip',str(self.binary)],capture_output=True,text=True,timeout=15)
        self.assertEqual(p.returncode,0,p.stderr);self.receipt['executable']['sha256']=bundle.digest(self.binary);self.save()
        with self.assertRaisesRegex(ValueError,'symbol table'):self.export()
