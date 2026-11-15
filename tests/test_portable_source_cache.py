"""Atomic archive-derived portable-source cache fixtures; no network/compiler."""
import hashlib
import importlib.util
import io
import json
import sys
from pathlib import Path
import tarfile
import tempfile
import unittest
from unittest import mock

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('portable_source_cache',ROOT/'Monado_Linux/scripts/setup_portable_baseline.py')
cache=importlib.util.module_from_spec(spec);spec.loader.exec_module(cache)

@unittest.skipUnless(sys.platform == 'linux', 'Linux portable cache permissions and links')
class PortableSourceCache(unittest.TestCase):
    def setUp(self):
        temporary=tempfile.TemporaryDirectory(prefix='monado source cache ');self.addCleanup(temporary.cleanup)
        self.base=Path(temporary.name);self.archives=[];self.lock={'sources':[]};self.identity='a'*64
        for component,root in (('SDL3','SDL3-3.4.8'),('FFmpeg','ffmpeg-8.0.1')):
            archive=self.base/(component+'.tar.gz')
            with tarfile.open(archive,'w:gz') as tar:
                data=b'authored source fixture\n';entry=tarfile.TarInfo(root+'/file.c');entry.size=len(data);entry.mode=0o644
                tar.addfile(entry,io.BytesIO(data))
            self.archives.append(archive);self.lock['sources'].append({'name':component,'sha256':hashlib.sha256(archive.read_bytes()).hexdigest()})
    def prepare(self):return cache.prepare_sources(self.base,self.lock,self.archives,self.identity)
    def test_atomic_publish_and_verified_reuse_preserve_legacy_cache(self):
        legacy=self.base/'sources/SDL3-3.4.8';legacy.mkdir(parents=True);(legacy/'keep').write_text('legacy')
        selected,digest=self.prepare();self.assertEqual(self.prepare(),(selected,digest));self.assertEqual((legacy/'keep').read_text(),'legacy')
        self.assertFalse(list((self.base/'sources').glob('.source-extract-*')))
    def test_changed_source_and_forged_inventory_are_not_repaired_or_accepted(self):
        selected,_=self.prepare();source=selected/'SDL3-3.4.8/file.c';source.write_bytes(b'changed')
        marker=selected/cache.SOURCE_MANIFEST;doc=json.loads(marker.read_text());row=doc['files']['SDL3-3.4.8/file.c'];row.update(bytes=7,sha256=hashlib.sha256(b'changed').hexdigest());marker.write_text(json.dumps(doc))
        with self.assertRaisesRegex(ValueError,'incomplete or changed'):self.prepare()
        self.assertEqual(source.read_bytes(),b'changed')
    def test_interrupted_extraction_never_publishes_partial_cache_and_retry_works(self):
        def interrupted(archive,path,*args,**kwargs):
            (Path(path)/'partial').write_text('incomplete');raise InterruptedError('authored interruption')
        with mock.patch.object(tarfile.TarFile,'extractall',interrupted),self.assertRaises(InterruptedError):self.prepare()
        self.assertFalse((self.base/'sources'/('verified-'+self.identity[:16])).exists())
        self.assertFalse(list((self.base/'sources').glob('.source-extract-*')))
        self.assertTrue(self.prepare()[0].is_dir())
    def test_existing_partial_verified_cache_is_preserved_and_rejected(self):
        target=self.base/'sources'/('verified-'+self.identity[:16]);target.mkdir(parents=True);(target/'keep').write_text('partial')
        with self.assertRaises((OSError,ValueError)):self.prepare()
        self.assertEqual((target/'keep').read_text(),'partial')

if __name__=='__main__':unittest.main()
