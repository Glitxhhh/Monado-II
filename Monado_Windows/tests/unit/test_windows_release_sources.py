"""Exact original-package/build-recipe/source-input bindings; authored archives."""
from pathlib import Path
import hashlib
import importlib.util
import io
import json
import os
import shutil
import subprocess
import tarfile
import tempfile
import unittest
from unittest import mock
import zstandard

SPEC=importlib.util.spec_from_file_location('release_sources',Path(__file__).resolve().parents[2]/'tools/windows_release_sources.py')
sources=importlib.util.module_from_spec(SPEC);SPEC.loader.exec_module(sources)


def archive(path,members):
    raw=io.BytesIO()
    with tarfile.open(fileobj=raw,mode='w') as target:
        for name,data in members.items():
            info=tarfile.TarInfo(name)
            if data is None:info.type=tarfile.DIRTYPE;target.addfile(info)
            else:info.size=len(data);target.addfile(info,io.BytesIO(data))
    path.write_bytes(zstandard.ZstdCompressor().compress(raw.getvalue()))
    return path


class ExactSources(unittest.TestCase):
    def setUp(self):
        self.temporary=tempfile.TemporaryDirectory();self.addCleanup(self.temporary.cleanup)
        self.root=Path(self.temporary.name);self.recipe=b'# Authored fixture, never executed\npkgver=1.2\n'
        self.recipe_sha=hashlib.sha256(self.recipe).hexdigest()
    def source(self,*,checksum=None,omit=False,skip=False,algorithm='sha256'):
        upstream=b'authored upstream source bytes, no compiled code'
        digest=hashlib.new(algorithm,upstream).hexdigest() if checksum is None else checksum
        info=('pkgbase = mingw-w64-authored\npkgver = 1.2\npkgrel = 3\nsource = https://example.invalid/upstream.tar\n'+algorithm+'sums = '+('SKIP' if skip else digest)+'\n').encode()
        members={'mingw-w64-authored':None,'mingw-w64-authored/.SRCINFO':info,'mingw-w64-authored/PKGBUILD':self.recipe,'mingw-w64-authored/LICENSE':b'Authored permissive license fixture'}
        if not omit:members['mingw-w64-authored/upstream.tar']=upstream
        return archive(self.root/'source.tar.zst',members)
    def test_exact_recipe_version_and_upstream_checksum(self):
        value=sources.validate_source(self.source(),'mingw-w64-authored','1.2-3',self.recipe_sha)
        self.assertEqual(value['inputs'][0]['identity']['sha256'],hashlib.sha256(b'authored upstream source bytes, no compiled code').hexdigest())
        self.assertEqual(value['licenses'][0][1],b'Authored permissive license fixture')
    def test_recipe_from_another_binary_build_is_rejected(self):
        with self.assertRaisesRegex(ValueError,'PKGBUILD'):sources.validate_source(self.source(),'mingw-w64-authored','1.2-3','0'*64)
    def test_missing_upstream_input_is_not_corresponding_source(self):
        with self.assertRaisesRegex(ValueError,'omits declared'):sources.validate_source(self.source(omit=True),'mingw-w64-authored','1.2-3',self.recipe_sha)
    def test_upstream_changed_checksum_is_rejected(self):
        with self.assertRaisesRegex(ValueError,'checksum differs'):sources.validate_source(self.source(checksum='0'*64),'mingw-w64-authored','1.2-3',self.recipe_sha)
    def test_non_sha256_checksum_is_actually_checked(self):
        sources.validate_source(self.source(algorithm='sha512'),'mingw-w64-authored','1.2-3',self.recipe_sha)
        with self.assertRaisesRegex(ValueError,'checksum differs'):sources.validate_source(self.source(algorithm='sha512',checksum='0'*128),'mingw-w64-authored','1.2-3',self.recipe_sha)
    def test_unchecked_download_is_not_marked_complete(self):
        with self.assertRaisesRegex(ValueError,'Unpinned'):sources.validate_source(self.source(skip=True),'mingw-w64-authored','1.2-3',self.recipe_sha)
    def test_binary_payload_owned_member_bound_to_original_package(self):
        binary=b'MZ authored fixture, never executed'
        pkg=b'pkgname = mingw-w64-ucrt-x86_64-authored\npkgbase = mingw-w64-authored\npkgver = 1.2-3\n'
        build=('pkgbase = mingw-w64-authored\npkgver = 1.2-3\npkgbuild_sha256sum = '+self.recipe_sha+'\n').encode()
        path=archive(self.root/'binary.tar.zst',{'.PKGINFO':pkg,'.BUILDINFO':build,'ucrt64/bin/authored.dll':binary})
        package={'name':'mingw-w64-ucrt-x86_64-authored','version':'1.2-3',**sources.file_identity(path)}
        wanted={'bin/authored.dll':sources.identity(binary)}
        self.assertEqual(sources.original_package(path,package,wanted)['owned'],wanted)
        with self.assertRaisesRegex(ValueError,'member mismatch'):sources.original_package(path,package,{'bin/authored.dll':sources.identity(b'changed')})
    def test_archive_escape_is_rejected_without_extraction(self):
        path=archive(self.root/'escape.tar.zst',{'../outside':b'authored'})
        with self.assertRaisesRegex(ValueError,'Unsafe'):sources.validate_source(path,'mingw-w64-authored','1.2-3',self.recipe_sha)
    def test_static_archive_identity_is_distinct_from_payload_mapping(self):
        self.assertEqual(sources.source_basename('authored::git+https://example.invalid/repo.git#commit=123'),'authored')
        self.assertEqual(sources.source_basename('https://example.invalid/upstream.tar?download=1'),'upstream.tar')

    def qualification(self):
        """An independent reviewed fixture lock, never a caller report flag."""
        folder=self.root/'qualified';folder.mkdir()
        data={'metadata/authored/BUILDINFO':('pkgbuild_sha256sum = '+self.recipe_sha+'\n').encode(),
              'archives/authored.src.tar.zst':self.source().read_bytes(),
              'licenses/native/authored/LICENSE':b'Authored license text'}
        files={n:sources.identity(raw) for n,raw in data.items()}
        source={'path':'archives/authored.src.tar.zst',**files['archives/authored.src.tar.zst']}
        package={'name':'authored','source_required':True,'pkgbuild_sha256':self.recipe_sha,
                 'source_archive':source,'source_inputs':[]}
        payload={'native/ucrt64/bin/authored.dll':sources.identity(b'authored DLL fixture')}
        qualified={'release_version':'0.1.0','binary_inputs':payload,'static_inputs':{},'packages':[package],
                   'artifacts':[source],'files':files,
                   'notices':{'licenses/native/authored/LICENSE':{'path':'licenses/native/authored/LICENSE',**files['licenses/native/authored/LICENSE']}},
                   'admissibility':{'status':'passed'}}
        review={'qualified':json.loads(json.dumps(qualified))};raw=b'Independent reviewed fixture bytes'
        for name,content in data.items():
            target=folder/name;target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes(content)
        (folder/'review').mkdir();(folder/'review/native-source-review.json').write_bytes(raw)
        qualified['files']=dict(files,**{'review/native-source-review.json':sources.identity(raw)})
        report=dict(qualified,schema=1,kind='windows-native-release-sources',platform='windows-amd64',status='complete',unresolved=[])
        path=folder/'native-sources.json';path.write_bytes(sources.encoded(report))
        return path,report,review,raw,payload

    def test_verified_real_source_files_not_report_flags(self):
        path,report,review,raw,payload=self.qualification()
        with mock.patch.object(sources,'source_review',return_value=(review,raw)):
            sources.verify_report(path,payload_files=payload,release_version='0.1.0')
            archive_path=path.parent/report['artifacts'][0]['path'];archive_path.write_bytes(b'changed upstream')
            with self.assertRaisesRegex(ValueError,'changed'):sources.verify_report(path,payload_files=payload,release_version='0.1.0')

    def test_empty_packages_cannot_waive_corresponding_source(self):
        path,report,review,raw,payload=self.qualification();report['packages']=[];path.write_bytes(sources.encoded(report))
        with mock.patch.object(sources,'source_review',return_value=(review,raw)),self.assertRaisesRegex(ValueError,'coverage'):
            sources.verify_report(path,payload_files=payload,release_version='0.1.0')

    def test_source_required_false_cannot_waive_gpl(self):
        path,report,review,raw,payload=self.qualification();report['packages'][0]['source_required']=False;path.write_bytes(sources.encoded(report))
        with mock.patch.object(sources,'source_review',return_value=(review,raw)),self.assertRaisesRegex(ValueError,'coverage'):
            sources.verify_report(path,payload_files=payload,release_version='0.1.0')

    def test_removed_archive_or_license_cannot_relabel_complete(self):
        path,report,review,raw,payload=self.qualification()
        for key,value in [('artifacts',[]),('notices',{})]:
            changed=dict(report,**{key:value});path.write_bytes(sources.encoded(changed))
            with mock.patch.object(sources,'source_review',return_value=(review,raw)),self.assertRaisesRegex(ValueError,'coverage'):
                sources.verify_report(path,payload_files=payload,release_version='0.1.0')

    def test_unreviewed_model_file_cannot_enter_source_companion(self):
        path,report,review,raw,payload=self.qualification();name='unapproved-model.bin';content=b'authored forbidden-file fixture'
        (path.parent/name).write_bytes(content);report['files'][name]=sources.identity(content);path.write_bytes(sources.encoded(report))
        with mock.patch.object(sources,'source_review',return_value=(review,raw)),self.assertRaisesRegex(ValueError,'inventory'):
            sources.verify_report(path,payload_files=payload,release_version='0.1.0')

    def test_static_libraries_cannot_change_after_source_review(self):
        path,report,review,raw,payload=self.qualification()
        with mock.patch.object(sources,'source_review',return_value=(review,raw)),self.assertRaisesRegex(ValueError,'statically'):
            sources.verify_report(path,payload_files=payload,release_version='0.1.0',static_files={'lib/libiconv.a':sources.identity(b'changed static code')})

    def test_sealed_frontend_retains_usable_relink_and_payload_repacker(self):
        path,report,review,raw,payload=self.qualification();frontend=self.root/'frontend';frontend.mkdir()
        authored=['tools/builder/windows_builder.cpp','third_party/imgui/imgui.cpp','third_party/imgui/imgui_draw.cpp',
                  'third_party/imgui/imgui_tables.cpp','third_party/imgui/imgui_widgets.cpp',
                  'third_party/imgui/backends/imgui_impl_sdl3.cpp','third_party/imgui/backends/imgui_impl_sdlrenderer3.cpp']
        def member(name,content,**extra):
            target=frontend/name;target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes(content)
            return dict(path=name,**sources.identity(content),**extra)
        objects=[member('objects/'+name.replace('/','_')+'.o',('Authored non-executable object fixture '+name).encode(),source=name) for name in authored]
        resource=member('objects/builder.res',b'Authored resource fixture')
        static=[member('static-libs/'+name,b'Authored static-library fixture '+name.encode(),dependency_path='lib/'+name) for name in ('libSDL3.a','libiconv.a')]
        static_inputs={i['dependency_path']:{k:i[k] for k in ('bytes','sha256')} for i in static}
        system=['m','kernel32','user32','gdi32','winmm','imm32','ole32','oleaut32','version','uuid','advapi32','setupapi','shell32','dinput8','bcrypt','cabinet','comctl32']
        argv=['c++','-march=x86_64_v2','-static',*(i['path'] for i in objects),resource['path'],*(i['path'] for i in static),*('-l'+n for n in system),'-Wl,--subsystem,windows','-municode','-o','builder-base.exe']
        recipe=dict(schema=1,compiler='Zig 0.14.1',working_directory='.',output='builder-base.exe',objects=objects,resource=resource,static_libraries=static,system_libraries=system,argv=argv)
        base=member('builder-base.exe',b'MZ Authored base fixture never executed')
        receipt=dict(relink_recipe=recipe,base_executable=base,source_inputs={'authored':'fixture'},compiler={'version':'0.14.1'})
        (frontend/'relink.json').write_bytes(sources.encoded(recipe));(frontend/'frontend-receipt.json').write_bytes(sources.encoded(receipt))
        report['static_inputs']=static_inputs;review['qualified']['static_inputs']=static_inputs;path.write_bytes(sources.encoded(report))
        with mock.patch.object(sources,'source_review',return_value=(review,raw)):
            sealed=sources.seal_relink(path,frontend,self.root/'sealed')
            result=sources.verify_report(sealed,payload_files=payload,release_version='0.1.0',static_files=static_inputs,relink_root=frontend)
            self.assertIn('relink/relink_windows_setup.py',result['files']);self.assertIn('relink/README.txt',result['files'])
            self.assertIn(b'--original-setup',(sealed.parent/'relink/README.txt').read_bytes())
            changed=json.loads(json.dumps(recipe));changed['argv'].insert(3,'unretained-object.o')
            with self.assertRaisesRegex(ValueError,'unreviewed link inputs'):sources.validate_relink_recipe(changed,static_inputs)
            (sealed.parent/'relink/README.txt').unlink()
            with self.assertRaisesRegex(ValueError,'missing files'):sources.verify_report(sealed,payload_files=payload,release_version='0.1.0')

    @unittest.skipUnless(shutil.which('git'),'Read-only Git checksum fixture requires Git')
    def test_git_source_checksum_ignores_downloaded_config_and_rejects_alternates(self):
        repo=self.root/'original.git';template=self.root/'empty-template';template.mkdir()
        env={k:v for k,v in os.environ.items() if not k.upper().startswith('GIT')}
        env.update(GIT_CONFIG_NOSYSTEM='1',GIT_CONFIG_GLOBAL=os.devnull,GIT_CONFIG_SYSTEM=os.devnull,
                   GIT_AUTHOR_NAME='Authored fixture',GIT_AUTHOR_EMAIL='fixture@example.invalid',
                   GIT_COMMITTER_NAME='Authored fixture',GIT_COMMITTER_EMAIL='fixture@example.invalid',
                   GIT_AUTHOR_DATE='2000-01-01T00:00:00+0000',GIT_COMMITTER_DATE='2000-01-01T00:00:00+0000')
        def git(*args,input=None):
            return subprocess.check_output(['git','--git-dir='+str(repo),*args],input=input,env=env,stderr=subprocess.PIPE)
        subprocess.run(['git','init','--bare','--template='+str(template),str(repo)],env=env,check=True,capture_output=True)
        blob=git('hash-object','-w','--stdin',input=b'Authored source fixture\n').decode().strip()
        tree=git('mktree',input=('100644 blob '+blob+'\tREADME\n').encode()).decode().strip()
        commit=git('commit-tree',tree,input=b'Authored source\n').decode().strip()
        (repo/'refs/heads/fixture').write_text(commit+'\n',encoding='ascii');(repo/'HEAD').write_text('ref: refs/heads/fixture\n',encoding='ascii')
        expected=hashlib.sha256(git('archive','--format','tar',commit)).hexdigest()
        prefix='mingw-w64-authored/source.git/'
        members={'mingw-w64-authored':None,'mingw-w64-authored/source.git':None}
        for p in repo.rglob('*'):
            relative=p.relative_to(repo).as_posix();members[prefix+relative]=None if p.is_dir() else p.read_bytes()
        # Source config could alter archive output or include external config.
        # Neither it nor hooks may enter the fresh verification repository.
        members[prefix+'config']=b'[tar "tar"]\ncommand = echo UNTRUSTED_SOURCE_COMMAND\n[include]\npath = missing-external-config\n'
        source=archive(self.root/'git-source.tar.zst',members)
        entry={'name':'source.git','directory':prefix.rstrip('/'),'revision':'commit='+commit,
               'declared_checksums':{'sha256':expected}}
        proof=sources.git_source_evidence(source,entry,self.root/'read-only-proof')
        self.assertEqual(proof['commit'],commit);self.assertEqual(proof['git_archive_sha256'],expected)
        changed=dict(entry,declared_checksums={'sha256':'0'*64})
        with self.assertRaisesRegex(ValueError,'checksum differs'):
            sources.git_source_evidence(source,changed,self.root/'wrong-checksum')
        members[prefix+'objects/info/alternates']=b'/untrusted/external/object-store\n'
        source=archive(self.root/'alternate-source.tar.zst',members)
        with self.assertRaisesRegex(ValueError,'not self-contained'):
            sources.git_source_evidence(source,entry,self.root/'forbidden-alternates')


if __name__=='__main__':unittest.main()
