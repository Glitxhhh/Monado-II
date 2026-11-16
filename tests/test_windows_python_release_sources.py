"""Authored source-companion fixtures; no downloaded code or game execution."""
from __future__ import annotations
from contextlib import redirect_stdout
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import stat
import tarfile
import tempfile
import unittest
from unittest import mock
import zipfile

ROOT=Path(__file__).absolute().parents[1]
SPEC=importlib.util.spec_from_file_location('authored_python_release_sources',ROOT/'tools/windows_python_release_sources.py')
sources=importlib.util.module_from_spec(SPEC);SPEC.loader.exec_module(sources)


def zip_bytes(files):
    result=io.BytesIO()
    with zipfile.ZipFile(result,'w') as archive:
        for name,value in files:archive.writestr(name,value)
    return result.getvalue()


def tar_bytes(files):
    result=io.BytesIO()
    with tarfile.open(fileobj=result,mode='w:gz') as archive:
        for name,value in files:
            info=tarfile.TarInfo(name);info.size=len(value)
            archive.addfile(info,io.BytesIO(value))
    return result.getvalue()


class ArchiveContracts(unittest.TestCase):
    def test_same_payload_mapping_and_original_distribution_notices(self):
        spec={'id':'cpython','version':'1.0.0'}
        raw=zip_bytes([('python314._pth',b'import site'),('LICENSE.txt',b'original notices'),
            ('Lib/__pycache__/secret.pyc',b'never copied'),('python314.zip',b'authored bytes')])
        files,notices=sources.binary_members(raw,spec)
        self.assertIn('python/python314._pth.disabled',files)
        self.assertNotIn('python/python314._pth',files)
        self.assertEqual(len(files),3)
        self.assertEqual(notices,{'LICENSE.txt':b'original notices'})

    def test_unsafe_duplicate_and_linked_zip_members_fail_closed(self):
        for paths in [('a','A'),('../outside','LICENSE.txt'),('a:stream','LICENSE.txt')]:
            with self.subTest(paths=paths),self.assertRaises(ValueError):
                sources.binary_members(zip_bytes([(n,b'authored') for n in paths]),{'id':'cpython'})
        output=io.BytesIO()
        with zipfile.ZipFile(output,'w') as archive:
            item=zipfile.ZipInfo('LICENSE');item.create_system=3;item.external_attr=(stat.S_IFLNK|0o777)<<16
            archive.writestr(item,b'outside')
        with self.assertRaises(ValueError):sources.binary_members(output.getvalue(),{'id':'cpython'})

    def test_source_requires_actual_code_recipe_and_license_regular_members(self):
        spec={'prefix':'source/','source':['engine.c'],'recipes':['build.py'],'licenses':['COPYING']}
        raw=tar_bytes([('source/engine.c',b'authored C'),('source/build.py',b'raise RuntimeError("NEVER EXECUTE")')])
        with self.assertRaisesRegex(ValueError,'COPYING'):sources.source_members(raw,spec)
        result=io.BytesIO()
        with tarfile.open(fileobj=result,mode='w:gz') as archive:
            for name in ('engine.c','build.py','COPYING'):
                member=tarfile.TarInfo('source/'+name)
                if name=='COPYING':member.type=tarfile.SYMTYPE;member.linkname='../../outside'
                else:member.size=1
                archive.addfile(member,None if name=='COPYING' else io.BytesIO(b'x'))
        with self.assertRaisesRegex(ValueError,'regular'):sources.source_members(result.getvalue(),spec)

    def test_duplicate_json_and_non_official_download_redirect_are_rejected(self):
        with self.assertRaises(ValueError):sources.document(b'{"files":{},"files":{}}')
        for url in ('http://www.python.org/source','https://www.python.org.evil.invalid/source',
                    'https://user@www.python.org/source','https://www.python.org/source?latest=true'):
            with self.subTest(url=url),self.assertRaises(ValueError):sources.checked_url(url)


@unittest.skipUnless(os.name=='nt','native Windows held-file/ownership source fixture')
class NativeCompanion(unittest.TestCase):
    def setUp(self):
        # Independent authored data in the physical temporary directory; no
        # source checkout, production prefix, cache, profile or input is changed.
        parent=Path(tempfile.gettempdir()).resolve()
        self.temp=tempfile.TemporaryDirectory(prefix='authored-python-sources-',dir=parent)
        self.base=Path(self.temp.name);self.binary=self.base/'binary';self.local=self.base/'sources'
        self.binary.mkdir();self.local.mkdir();self.lock={'binary_archives':[],'source_artifacts':[]}
        for package in ('cpython','ziglang','capstone','unicorn','lz4','zstandard'):
            filename=package+('.zip' if package=='cpython' else '.whl')
            files=[('LICENSE.txt',b'authored notice')] if package=='cpython' else [
                (package+'-1.0.0.dist-info/METADATA',('Name: '+package+'\nVersion: 1.0.0\n').encode()),
                (package+'-1.0.0.dist-info/LICENSE',b'authored notice'),
                (package+'/__init__.py',b'raise RuntimeError("DO NOT IMPORT OR EXECUTE")')]
            raw=zip_bytes(files);(self.binary/filename).write_bytes(raw)
            self.lock['binary_archives'].append({'id':package,'version':'1.0.0','filename':filename,**sources.identity(raw),
                'url':'https://www.python.org/authored/'+filename,'license_expression':'authored synthetic fixture only',
                'source_artifacts':['unicorn-sdist','unicorn-repository']})
        for ident,prefix in (('unicorn-sdist','src/'),('unicorn-repository','')):
            spec={'id':ident,'filename':ident+'.tar.gz','url':'https://www.python.org/authored/'+ident+'.tar.gz',
                'prefix':'authored/','source':[prefix+'uc.c',prefix+'qemu/target/arm/translate-a64.c'],
                'recipes':['build.py'],'licenses':['COPYING']}
            raw=tar_bytes([('authored/'+name,b'/* authored complete fixture; never execute */') for name in spec['source']]+
                [('authored/build.py',b'raise RuntimeError("DO NOT EXECUTE")'),('authored/COPYING',b'authored license witness')])
            spec.update(sources.identity(raw));self.lock['source_artifacts'].append(spec);(self.local/spec['filename']).write_bytes(raw)
        self.lock_hash=hashlib.sha256(sources.encoded(self.lock)).hexdigest()
        self.patcher=mock.patch.object(sources,'load_lock',return_value=(self.lock,self.lock_hash));self.patcher.start()
        self.guard=sources.guard_module()

    def tearDown(self):
        self.patcher.stop();self.temp.cleanup()

    def audit(self,name='companion',**options):
        with redirect_stdout(io.StringIO()):
            return sources.audit(self.binary,self.base/name,release_version='1.0.0',guard=self.guard,
                source_archives=options.pop('source_archives',self.local),**options)

    def verify(self,report,name='companion',**changes):
        payload=dict(report['binary_inputs']);payload['source/new-branded-ui.cpp']=sources.identity(b'changed authored UI')
        return sources.verify_report(self.base/name/'python-source-report.json',payload_files=changes.pop('payload',payload),
            release_version=changes.pop('version','1.0.0'),guard=self.guard)

    def test_actual_native_complete_content_and_ui_independent_subset(self):
        report=self.audit();self.assertEqual(report['status'],'complete');self.assertEqual(report['unresolved'],[])
        self.assertEqual(self.verify(report),report)
        self.assertFalse(report['source_or_binary_code_executed'])
        self.assertEqual(len(report['artifacts']),2)
        for notice in report['notices'].values():
            self.assertEqual(sources.identity((self.base/'companion'/notice['path']).read_bytes()),
                             {key:notice[key] for key in ('bytes','sha256')})

    def test_missing_sources_is_incomplete_not_a_success_flag(self):
        report=self.audit(source_archives=None)
        self.assertEqual(report['status'],'incomplete');self.assertEqual(len(report['unresolved']),2)
        with self.assertRaises(ValueError):self.verify(report)

    def test_corrupt_local_source_never_falls_back_to_download(self):
        (self.local/self.lock['source_artifacts'][0]['filename']).write_bytes(b'changed source')
        with mock.patch.object(sources,'download',side_effect=AssertionError('must not fetch over corruption')) as fetch:
            report=self.audit(allow_download=True)
        fetch.assert_not_called();self.assertEqual(report['status'],'incomplete')

    def test_wrong_final_python_bytes_or_release_version_rejected(self):
        report=self.audit();payload=dict(report['binary_inputs']);key=next(iter(payload));payload[key]=sources.identity(b'changed')
        with self.assertRaises(ValueError):self.verify(report,payload=payload)
        with self.assertRaises(ValueError):self.verify(report,version='1.0.1')

    def test_changed_license_and_unknown_companion_file_preserved_but_rejected(self):
        report=self.audit();notice=next(iter(report['notices'].values()));path=self.base/'companion'/notice['path']
        path.write_bytes(b'changed notice')
        with self.assertRaises(ValueError):self.verify(report)
        report=self.audit(name='other');unknown=self.base/'other/human-note.txt';unknown.write_bytes(b'preserve')
        with self.assertRaises(ValueError):self.verify(report,name='other')
        self.assertEqual(unknown.read_bytes(),b'preserve')

    def test_missing_original_binary_notice_cannot_be_blessed_by_rewritten_manifest(self):
        report=self.audit();target=next(k for k,v in report['notices'].items() if 'binary_member' in v)
        item=report['notices'].pop(target);report['files'].pop(item['path']);(self.base/'companion'/item['path']).unlink()
        (self.base/'companion/python-source-report.json').write_bytes(sources.encoded(report))
        with self.assertRaisesRegex(ValueError,'distribution notices'):self.verify(report)

    def test_duplicate_package_metadata_and_source_tag_mismatch_rejected(self):
        report=self.audit();report['packages'].append(dict(report['packages'][0]))
        (self.base/'companion/python-source-report.json').write_bytes(sources.encoded(report))
        with self.assertRaisesRegex(ValueError,'provenance'):self.verify(report)
        spec=self.lock['source_artifacts'][1]
        raw=tar_bytes([('authored/'+name,b'different publisher core') for name in spec['source']]+
            [('authored/build.py',b'never execute'),('authored/COPYING',b'license')])
        (self.local/spec['filename']).write_bytes(raw);spec.update(sources.identity(raw))
        report=self.audit(name='different-tag')
        self.assertEqual(report['status'],'incomplete')
        self.assertTrue(any(x['artifact']=='unicorn-source-correspondence' for x in report['unresolved']))

    def test_existing_or_overlapping_output_never_overwritten(self):
        occupied=self.base/'companion';occupied.mkdir();human=occupied/'human';human.write_bytes(b'keep')
        with self.assertRaises(ValueError):self.audit()
        self.assertEqual(human.read_bytes(),b'keep')
        with self.assertRaises(ValueError):
            sources.audit(self.binary,self.binary/'nested',release_version='1.0.0',guard=self.guard)


if __name__=='__main__':unittest.main()
