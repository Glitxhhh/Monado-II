"""Collect exact source/license companions for the pinned Windows Python tools.

This never installs, imports or executes downloaded code. Binary inputs are the
six original checked archives, not a populated interpreter or ambient packages.
Only explicitly pinned HTTPS source archives can be downloaded. A complete
report means source/recipe/notice content is present and hash-verified; it does
not claim a reproducible binary rebuild, legal approval, or game qualification.
"""
from __future__ import annotations

import argparse
from contextlib import ExitStack
from email.parser import BytesParser
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path, PurePosixPath
import re
import stat
import sys
import tarfile
import urllib.parse
import urllib.request
import zipfile

ROOT = Path(__file__).absolute().parents[1]
LOCK = Path(__file__).with_suffix('.lock.json')
MAX_FILE = 512 * 1024 * 1024
MAX_REPORT = 32 * 1024 * 1024
HEX = re.compile(r'[0-9a-f]{64}\Z')
NOTICE = re.compile(r'^(?:licen[cs]e|copying|copyright|notice|patents)(?:[._-].*|s)?$', re.I)
HOSTS = {'www.python.org', 'files.pythonhosted.org', 'ziglang.org', 'codeload.github.com'}


def encoded(value):
    return (json.dumps(value, sort_keys=True, indent=2, ensure_ascii=False)+'\n').encode('utf-8')


def identity(data):
    return {'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}


def relative(value):
    if (not isinstance(value,str) or not value or not value.isascii() or
            str(PurePosixPath(value)) != value or value.startswith('/') or
            any(c in value for c in '\\:<>"|?*') or
            any(c in value for c in '\x00\r\n')):
        raise ValueError('Unsafe companion member: '+str(value))
    for part in value.split('/'):
        if (part in ('','.','..') or part.endswith(('.', ' ')) or
                re.fullmatch(r'(?:CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\..*)?',part,re.I)):
            raise ValueError('Unsafe Windows companion member: '+value)
    return value


def document(raw):
    def unique(pairs):
        result={}
        for key,value in pairs:
            if key in result: raise ValueError('Duplicate companion metadata key: '+key)
            result[key]=value
        return result
    value=json.loads(raw,object_pairs_hook=unique)
    if not isinstance(value,dict): raise ValueError('Expected companion metadata object')
    return value


def table(files):
    if not isinstance(files,dict) or not 1 <= len(files) <= 100000:
        raise ValueError('Invalid bounded companion file inventory')
    folded=set()
    for key,item in files.items():
        relative(key)
        if key.casefold() in folded: raise ValueError('Case-colliding companion member')
        folded.add(key.casefold())
        if (not isinstance(item,dict) or set(item)!={'bytes','sha256'} or
                type(item['bytes']) is not int or not 0<=item['bytes']<=MAX_FILE or
                not isinstance(item['sha256'],str) or not HEX.fullmatch(item['sha256'])):
            raise ValueError('Invalid companion file identity: '+key)
    return files


def guard_module():
    path=ROOT/'tools/windows_file_guard.py'
    spec=importlib.util.spec_from_file_location('monado_python_sources_guard',path)
    module=importlib.util.module_from_spec(spec);sys.modules[spec.name]=module
    spec.loader.exec_module(module)
    return module


def read(path, *, maximum=MAX_FILE, guard=None, owner=False):
    guard=guard or guard_module()
    with guard.open_regular(path,max_bytes=maximum,owner=owner) as stream:
        data=stream.read(maximum+1)
    if len(data)>maximum:raise ValueError('Archive exceeds its bounded snapshot size')
    return data


def load_lock(*, guard=None):
    raw=read(LOCK,maximum=1024*1024,guard=guard)
    value=document(raw)
    if value.get('schema')!=1 or value.get('platform')!='windows-amd64':
        raise ValueError('Expected the reviewed Windows Python source lock')
    binaries=value.get('binary_archives',[]);sources=value.get('source_artifacts',[])
    if len(binaries)!=6 or {x['id'] for x in binaries}!={'cpython','ziglang','capstone','unicorn','lz4','zstandard'}:
        raise ValueError('The original six pinned binary inputs are required')
    if len(sources)!=17 or len({x['id'] for x in sources})!=len(sources):
        raise ValueError('Incomplete corresponding-source artifact lock')
    source_ids={x['id'] for x in sources}
    for item in binaries+sources:
        relative(item['filename'])
        if '/' in item['filename'] or not HEX.fullmatch(item['sha256']) or not 0<item['bytes']<=MAX_FILE:
            raise ValueError('Invalid pinned archive identity')
        checked_url(item['url'])
    for item in binaries:
        if not set(item['source_artifacts']) <= source_ids: raise ValueError('Unknown source artifact binding')
    for item in sources:
        if not item['prefix'].endswith('/'): raise ValueError('Expected exact source archive prefix')
        relative(item['prefix'][:-1])
        if any(not item.get(k) for k in ('source','recipes','licenses')):
            raise ValueError('Source, recipe and license witnesses are all mandatory')
        for key in ('source','recipes','licenses'):
            for member in item[key]: relative(member)
    return value,hashlib.sha256(raw).hexdigest()


def checked_url(url):
    parsed=urllib.parse.urlsplit(url)
    if (parsed.scheme!='https' or parsed.hostname not in HOSTS or parsed.username or
            parsed.password or parsed.port not in (None,443) or parsed.query or parsed.fragment):
        raise ValueError('Source download is not an explicit pinned official HTTPS URL')
    return url


class PinnedRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self,request,fp,code,msg,headers,newurl):
        checked_url(newurl)
        return super().redirect_request(request,fp,code,msg,headers,newurl)


def download(item):
    opener=urllib.request.build_opener(PinnedRedirect())
    request=urllib.request.Request(checked_url(item['url']),headers={'User-Agent':'Monado-II-source-companion/1'})
    with opener.open(request,timeout=45) as response:
        checked_url(response.geturl())
        data=response.read(item['bytes']+1)
    if identity(data)!={k:item[k] for k in ('bytes','sha256')}:
        raise ValueError('Official source archive differs from the reviewed exact identity: '+item['id'])
    return data


def source_members(raw,spec):
    """Inspect fixed witnesses without extracting or executing archive paths."""
    result={}
    wanted=set(spec['source']+spec['recipes']+spec['licenses'])
    with tarfile.open(fileobj=io.BytesIO(raw),mode='r:*') as archive:
        matches={};count=0
        for member in archive:
            count+=1
            if count>200000: raise ValueError('Unbounded source archive member count')
            if member.name.startswith(spec['prefix']):
                short=member.name[len(spec['prefix']):]
                if short in wanted:
                    if short in matches: raise ValueError('Duplicate required source archive member: '+short)
                    if not member.isfile() or not 0<member.size<=MAX_FILE:
                        raise ValueError('Required source/recipe/license is not a bounded regular archive file: '+short)
                    matches[short]=member
        missing=wanted-set(matches)
        if missing: raise ValueError('Missing actual source/recipe/license members: '+', '.join(sorted(missing)))
        for short,member in matches.items():
            with archive.extractfile(member) as stream:
                value=stream.read(member.size+1)
            if len(value)!=member.size: raise ValueError('Source archive member size changed')
            result[short]=value
    return result


def binary_members(raw,spec):
    """Match the actual setup payload's original archive mapping exactly."""
    result={};notices={};folded=set();metadata=[];total=0
    with zipfile.ZipFile(io.BytesIO(raw)) as archive:
        if len(archive.infolist())>100000: raise ValueError('Unbounded pinned binary archive')
        for member in archive.infolist():
            if member.is_dir(): continue
            key=relative(member.filename)
            if key.casefold() in folded or stat.S_ISLNK(member.external_attr>>16):
                raise ValueError('Case-colliding or linked pinned archive member')
            folded.add(key.casefold())
            if not 0<=member.file_size<=MAX_FILE: raise ValueError('Oversized pinned archive member')
            if '/__pycache__/' in key or key.endswith('.pyc'): continue
            data=archive.read(member);total+=len(data)
            if total>4*1024**3: raise ValueError('Pinned archive expands beyond its bound')
            if len(data)!=member.file_size: raise ValueError('Pinned archive member size differs')
            mapped=key+'.disabled' if key=='python314._pth' else key
            prefix='python/' if spec['id']=='cpython' else 'python/Lib/site-packages/'
            result[prefix+mapped]=identity(data)
            if NOTICE.fullmatch(PurePosixPath(key).name):
                notices[key]=data
            if key.endswith('.dist-info/METADATA'): metadata.append(data)
        if spec['id']!='cpython':
            if len(metadata)!=1: raise ValueError('Expected one exact pinned wheel metadata record')
            record=BytesParser().parsebytes(metadata[0])
            if record['Name'].lower()!=spec['id'] or record['Version']!=spec['version']:
                raise ValueError('Pinned wheel package/version differs')
        if not notices and spec['id']!='unicorn': raise ValueError('Pinned archive lacks distribution notices')
    return result,notices


def notice_target(artifact,member):
    return relative('licenses/python/'+artifact+'/'+member)


def put(owned,files,path,data):
    relative(path)
    if path in files: raise ValueError('Duplicate companion output member')
    parts=PurePosixPath(path).parts
    with ExitStack() as parents:
        directory=owned
        for part in parts[:-1]: directory=parents.enter_context(directory.child(part,create=True,owner=True))
        directory.write_new(parts[-1],data,owner=True)
    files[path]=identity(data)


def audit(binary_archives,output,*,release_version,source_archives=None,allow_download=False,guard=None):
    guard=guard or guard_module();lock,lock_sha=load_lock(guard=guard)
    if not re.fullmatch(r'[0-9]+\.[0-9]+\.[0-9]+(?:[-.][A-Za-z0-9.-]+)?',release_version):
        raise ValueError('An explicit independent Windows release version is required')
    binaries=Path(guard.canonical_path(Path(binary_archives).absolute()));output=Path(guard.canonical_path(Path(output).absolute()))
    if output.exists(): raise ValueError('Use a fresh source companion output; nothing is overwritten')
    if source_archives is not None: source_archives=Path(guard.canonical_path(Path(source_archives).absolute()))
    if output==binaries or output.is_relative_to(binaries) or binaries.is_relative_to(output):
        raise ValueError('Source companion output must be disjoint from original binary inputs')
    binary_inputs={};notices={};files={};packages=[];artifacts=[];unresolved=[];witness_map={}
    with guard.locked_directory(binaries,owner=False) as binary_root,guard.locked_directory(output.parent,owner=False) as parent:
        with parent.child(output.name,create=True,owner=True) as owned:
            for item in lock['binary_archives']:
                with binary_root.open_regular(item['filename'],max_bytes=item['bytes'],owner=False) as stream:
                    raw=stream.read(item['bytes']+1)
                if identity(raw)!={k:item[k] for k in ('bytes','sha256')}:
                    raise ValueError('Pinned original Python binary archive changed: '+item['filename'])
                members,license_data=binary_members(raw,item)
                if {key.casefold() for key in binary_inputs}&{key.casefold() for key in members}:
                    raise ValueError('Pinned Python archives overlap')
                binary_inputs.update(members)
                for member,data in license_data.items():
                    target=notice_target(item['id']+'-binary',member)
                    path='notices/'+target[len('licenses/python/'):]
                    put(owned,files,path,data)
                    source_member=('python/' if item['id']=='cpython' else 'python/Lib/site-packages/')+member
                    notices[target]={'path':path,**identity(data),'binary_member':source_member}
                packages.append(dict(item,binary_members=len(members)))
            for item in lock['source_artifacts']:
                try:
                    if source_archives is not None:
                        try:
                            raw=read(source_archives/item['filename'],guard=guard,owner=False)
                        except OSError as error:
                            if not allow_download or error.errno not in (2,3): raise
                            raw=download(item)
                        if identity(raw)!={k:item[k] for k in ('bytes','sha256')}:
                            raise ValueError('Pinned source input changed: '+item['id'])
                    elif allow_download:
                        raw=download(item)
                    else:
                        raise FileNotFoundError('Pinned source archive is missing; supply --source-archives or explicitly --download')
                    witnesses=source_members(raw,item)
                    witness_map[item['id']]=witnesses
                    path='sources/'+item['filename'];put(owned,files,path,raw)
                    artifacts.append({'path':path,'url':item['url'],'bytes':len(raw),'sha256':hashlib.sha256(raw).hexdigest(),
                        'purpose':'corresponding-source-recipes-and-licenses','id':item['id'],
                        'members':{name:identity(value) for name,value in sorted(witnesses.items())}})
                    for member in item['licenses']:
                        # SQLite's public-domain declaration is the header's
                        # actual text; retain the complete original header.
                        data=witnesses[member];target=notice_target(item['id']+'-source',member)
                        path='notices/'+target[len('licenses/python/'):];put(owned,files,path,data)
                        notices[target]={'path':path,**identity(data),'artifact':item['id'],'archive_member':member}
                    print('Checked source/license closure: '+item['id'],flush=True)
                except (OSError,ValueError,tarfile.TarError,urllib.error.URLError) as error:
                    unresolved.append({'artifact':item['id'],'reason':str(error)[:2048]})
            if 'unicorn-sdist' in witness_map and 'unicorn-repository' in witness_map:
                for member in ('uc.c','qemu/target/arm/translate-a64.c'):
                    if witness_map['unicorn-sdist']['src/'+member]!=witness_map['unicorn-repository'][member]:
                        unresolved.append({'artifact':'unicorn-source-correspondence','reason':'PyPI engine source differs from its exact upstream tag: '+member})
            index=('Windows Python/tool source and license companion\n'
                'No game code, vendor SDK, game assets, or generated game code is included.\n'
                'No downloaded source/build recipe was executed.\n'
                'Binary archives are official publisher inputs; their exact payload members are recorded.\n'
                'Source/recipe correspondence is publisher-versioned, not a byte-reproducible rebuild claim.\n'
                'Unicorn Python metadata describes BSD bindings; unicorn.dll engine/QEMU is GPLv2.\n'
                'Distribute the complete source companion and notices with the setup executable.\n\n'+
                '\n'.join(item['id']+' '+item['version']+': '+item['license_expression'] for item in lock['binary_archives'])+'\n').encode('utf-8')
            put(owned,files,'NOTICE_INDEX.txt',index)
            report={'schema':1,'kind':'windows-python-release-sources','platform':'windows-amd64','release_version':release_version,
                'status':'complete' if not unresolved else 'incomplete','source_lock_sha256':lock_sha,
                'binary_inputs':dict(sorted(binary_inputs.items())),'binary_inputs_sha256':hashlib.sha256(encoded(binary_inputs)).hexdigest(),
                'artifacts':artifacts,'files':dict(sorted(files.items())),'packages':packages,'notices':notices,
                'unresolved':unresolved,'source_or_binary_code_executed':False,
                'qualification_scope':'Exact source/recipe/license content; no legal approval, reproducible rebuild, native UI/gameplay or GPU qualification claim.'}
            owned.write_new('python-source-report.json',encoded(report),owner=True)
    return report


def verify_report(path,*,payload_files,release_version,guard=None):
    """Release gate: no network, no execution, no metadata-only approval flag."""
    guard=guard or guard_module();lock,lock_sha=load_lock(guard=guard)
    path=Path(guard.canonical_path(Path(path).absolute()));expected_subset={k:v for k,v in table(payload_files).items() if k.startswith('python/')}
    with guard.locked_directory(path.parent,owner=True) as owned:
        report=document(owned.read_regular(path.name,max_bytes=MAX_REPORT,owner=True))
        if (report.get('schema')!=1 or report.get('kind')!='windows-python-release-sources' or
                report.get('platform')!='windows-amd64' or report.get('release_version')!=release_version or
                report.get('status')!='complete' or report.get('unresolved')!=[] or
                report.get('source_lock_sha256')!=lock_sha or report.get('source_or_binary_code_executed') is not False or
                report.get('binary_inputs')!=expected_subset or
                report.get('binary_inputs_sha256')!=hashlib.sha256(encoded(expected_subset)).hexdigest()):
            raise ValueError('Python source qualification does not bind the final exact Python payload subset')
        files=table(report.get('files'))
        actual=set()
        for directory,dirs,names in os.walk(path.parent,followlinks=False):
            for leaf in dirs:
                with guard.locked_directory(Path(directory)/leaf,owner=True):pass
            for leaf in names:
                relative_name=(Path(directory)/leaf).relative_to(path.parent).as_posix();actual.add(relative_name)
        if actual!=set(files)|{path.name}:raise ValueError('Source companion has missing or undeclared members')
        for name,item in files.items():
            raw=owned.read_regular(name,max_bytes=item['bytes'],owner=True)
            if identity(raw)!=item: raise ValueError('Source/license companion member changed: '+name)
        artifacts=report.get('artifacts')
        if not isinstance(artifacts,list) or len(artifacts)!=len(lock['source_artifacts']):raise ValueError('Incomplete source artifact inventory')
        by_id={item['id']:item for item in artifacts}
        if len(by_id)!=len(artifacts):raise ValueError('Duplicate source artifact identity')
        licenses={}
        witness_map={}
        for spec in lock['source_artifacts']:
            item=by_id.get(spec['id']);expected_path='sources/'+spec['filename']
            if (not item or item.get('path')!=expected_path or item.get('url')!=spec['url'] or
                    {k:item.get(k) for k in ('bytes','sha256')}!={k:spec[k] for k in ('bytes','sha256')} or
                    files.get(expected_path)!={k:spec[k] for k in ('bytes','sha256')}):
                raise ValueError('Corresponding source artifact is not the reviewed publisher input')
            witnesses=source_members(owned.read_regular(expected_path,max_bytes=spec['bytes'],owner=True),spec)
            witness_map[spec['id']]=witnesses
            if item.get('members')!={name:identity(value) for name,value in sorted(witnesses.items())}:
                raise ValueError('Source/recipe/license witnesses changed')
            for member in spec['licenses']:
                licenses[notice_target(spec['id']+'-source',member)]=(spec['id'],member,identity(witnesses[member]))
        notices=report.get('notices')
        if not isinstance(notices,dict) or not licenses.keys()<=notices.keys():raise ValueError('Required source license notices are absent')
        binary_notice_members={name for name in expected_subset if NOTICE.fullmatch(PurePosixPath(name).name)}
        if {item.get('binary_member') for item in notices.values() if 'binary_member' in item}!=binary_notice_members:
            raise ValueError('Final Python payload distribution notices are missing from the companion')
        for target,item in notices.items():
            relative(target)
            if not target.startswith('licenses/python/') or files.get(item.get('path'))!={k:item.get(k) for k in ('bytes','sha256')}:
                raise ValueError('Notice is not bound to checked source companion bytes')
            if target in licenses:
                artifact,member,ident=licenses[target]
                if item.get('artifact')!=artifact or item.get('archive_member')!=member or {k:item[k] for k in ident}!=ident:
                    raise ValueError('Source license mapping is inconsistent')
            elif expected_subset.get(item.get('binary_member'))!={k:item.get(k) for k in ('bytes','sha256')}:
                raise ValueError('Binary notice differs from the final payload member')
        if (not isinstance(report.get('packages'),list) or len(report['packages'])!=len(lock['binary_archives']) or
                [{k:item[k] for k in spec} for item,spec in zip(report['packages'],lock['binary_archives'])]!=lock['binary_archives']):
            raise ValueError('Source package provenance differs from the reviewed lock')
        for member in ('uc.c','qemu/target/arm/translate-a64.c'):
            if witness_map['unicorn-sdist']['src/'+member]!=witness_map['unicorn-repository'][member]:
                raise ValueError('Pinned Unicorn PyPI engine source differs from the upstream tag')
        permitted={'NOTICE_INDEX.txt'}|{item['path'] for item in artifacts}|{item['path'] for item in notices.values()}
        if set(files)!=permitted:raise ValueError('Companion contains unrecognized non-source artifacts')
    return report


def main(argv=None):
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary-archives',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--release-version',required=True)
    parser.add_argument('--source-archives',type=Path)
    parser.add_argument('--download',action='store_true',help='Fetch only missing exact source artifacts from the reviewed lock; corrupt local inputs never fall back; never execute code')
    args=parser.parse_args(argv)
    try:
        result=audit(args.binary_archives,args.output,release_version=args.release_version,
            source_archives=args.source_archives,allow_download=args.download)
        print(json.dumps({'status':result['status'],'report':str(args.output/'python-source-report.json'),'unresolved':result['unresolved']},indent=2))
        return 0 if result['status']=='complete' else 2
    except (Exception,KeyboardInterrupt) as error:
        print(str(error),file=sys.stderr);return 1


if __name__=='__main__':raise SystemExit(main())
