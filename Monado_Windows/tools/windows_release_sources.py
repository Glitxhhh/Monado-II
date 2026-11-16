"""Collect exact MSYS2 corresponding source for a Windows setup's native files.

No package recipe is executed and no binary is installed. Original hash-locked
binary archives identify each shipped file and pin its PKGBUILD through
.BUILDINFO. Official all-source archives carry that recipe, patches and upstream
inputs. The result is an independently hashable release companion, not approval
to publish a game or proprietary SDK. Incomplete evidence is never complete.
"""
from __future__ import annotations

import argparse
import collections
import contextlib
import ctypes
import gzip
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import posixpath
import re
import shutil
import stat
import struct
import subprocess
import tarfile
import urllib.parse
import urllib.request
import zipfile

PORT = Path(__file__).resolve().parents[1]
ORIGIN = 'https://repo.msys2.org/mingw/sources/'
HEX = re.compile(r'[0-9a-f]{64}\Z')
MAX_MEMBER = 1024 * 1024 * 1024
LICENSE = re.compile(r'^(?:copying|copyright|licen[cs]e|notice)(?:[._-].*)?$', re.I)
REVIEW_PATH = PORT/'tools/windows-release-native-source-review.json'
# These packages contribute source headers and permissive notices only. Keep
# those exact sources rather than unrelated optional examples/vocabulary/tools.
HEADER_ONLY = {
    'mingw-w64-whisper.cpp': 'MIT; exact shipped headers retained; optional upstream test-model containers are unnecessary.',
    'mingw-w64-llama.cpp': 'MIT; exact shipped headers retained; optional GGUF vocabulary containers have zero tensors and are unnecessary.',
    'mingw-w64-tcl': 'Permissive Tcl and bundled header notices retained; optional upstream platform executables are unnecessary.',
    'mingw-w64-vulkan-memory-allocator': 'MIT; the full shipped implementation header is source; optional sample executables are unnecessary.',
}
RELINK_README = b'''Windows portable frontend relinking
===================================

Work in a COPY of this relink directory. No game objects are included. The seven
frontend .o files, resource object, original SDL3/libiconv archives and relative
Zig 0.14.1 link recipe are retained. The native source companion contains the
matching SDL3/libiconv sources, patches, recipes and licenses. The project source
companion contains the authored frontend, ImGui, fonts and artwork sources.
The separate Python/tool source companion covers the bundled Zig toolchain.

To modify libiconv, rebuild its matching source for x86_64 Windows UCRT, using
the retained package recipe/build metadata as a starting point. Replace
static-libs/libiconv.a in your COPY with the compatible rebuilt archive. The
same procedure can replace SDL3. Retain the supplied project objects unless
also rebuilding the project frontend from its source companion.

In PowerShell, from this relink directory, use Zig 0.14.1 (the setup's bundled
ziglang tool or the matching official toolchain):

  $zig = 'C:\\path\\to\\zig.exe'
  $recipe = Get-Content .\\relink.json -Raw | ConvertFrom-Json
  $linkArgs = @($recipe.argv)
  & $zig @linkArgs
  if ($LASTEXITCODE -ne 0) { throw 'Frontend relinking failed' }

This produces builder-base.exe. The base alone does not contain the build
tools/source payload. Attach the unchanged payload from the published original
setup (verify its published SHA256 first):

  python -I -S -B .\\relink_windows_setup.py --original-setup 'C:\\path\\to\\MonadoII-Windows-Setup.exe' --base-executable .\\builder-base.exe --output .\\MonadoII-Windows-Setup-Relinked.exe

The repacker uses Python's standard library only. It verifies the original
embedded payload hash and framing, copies that payload byte-for-byte, and writes
a new footer using the rebuilt base's offset. It does not execute the setup,
extract its payload, download anything, or replace an existing output file.
The modified setup's full checksum differs from the publisher's original.
Do not redistribute game libraries, game input, saves, caches or vendor SDKs.

LGPL 2.1 section 6: https://www.gnu.org/licenses/old-licenses/lgpl-2.1.en.html
'''


def source_review():
    """Checked-in review pins coverage, source disposition and actual bytes.

    A report supplied by a caller is evidence to verify, never an authority that
    can remove packages or turn off corresponding-source requirements.
    """
    raw=REVIEW_PATH.read_bytes();review=json.loads(raw)
    if (review.get('schema')!=1 or review.get('kind')!='windows-native-source-review' or
            review.get('dependency_lock_sha256')!=file_identity(PORT/'tools/windows-deps.lock.json')['sha256']):
        raise ValueError('Native source review differs from the pinned dependency lock')
    return review,raw


def encoded(value):
    return (json.dumps(value, indent=2, sort_keys=True, ensure_ascii=False)+'\n').encode('utf-8')


def identity(data):
    return {'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}


def file_identity(path):
    with Path(path).open('rb') as stream:
        return {'bytes': os.fstat(stream.fileno()).st_size,
                'sha256': hashlib.file_digest(stream, 'sha256').hexdigest()}


def safe_name(value):
    value = value.removeprefix('./')
    if (not value or '\\' in value or value.startswith('/') or
            any(p in ('', '.', '..') or ':' in p for p in value.split('/'))):
        raise ValueError('Unsafe source/archive member: '+value)
    return value


def fields(raw):
    result = collections.defaultdict(list)
    for row in raw.decode('utf-8').splitlines():
        row = row.strip()
        if row and not row.startswith('#') and ' = ' in row:
            key, value = row.split(' = ', 1)
            result[key].append(value)
    return dict(result)


def one(table, key):
    value = table.get(key)
    if not isinstance(value, list) or len(value) != 1 or not value[0]:
        raise ValueError('Missing/ambiguous package metadata: '+key)
    return value[0]


@contextlib.contextmanager
def ztar(path):
    import zstandard
    with Path(path).open('rb') as raw, zstandard.ZstdDecompressor().stream_reader(raw) as reader:
        with tarfile.open(fileobj=reader, mode='r|') as archive:
            yield archive


def payload_manifest(executable, expected):
    """Read only the authenticated manifest, without extraction/execution."""
    if file_identity(executable)['sha256'] != expected:
        raise ValueError('Setup executable differs from its explicit identity')
    header, record, footer = struct.Struct('<8sIIQ'), struct.Struct('<IIQQ32s'), struct.Struct('<8sQQ32s')
    with Path(executable).open('rb') as stream:
        stream.seek(-footer.size, 2)
        magic, offset, size, sha = footer.unpack(stream.read(footer.size))
        if magic != b'M2BNDL01' or offset+size+footer.size != stream.seek(0, 2):
            raise ValueError('Invalid setup payload footer')
        stream.seek(offset)
        digest, remaining = hashlib.sha256(), size
        while remaining:
            block = stream.read(min(1024*1024, remaining))
            if not block: raise ValueError('Truncated setup payload')
            digest.update(block); remaining -= len(block)
        if digest.digest() != sha: raise ValueError('Setup payload changed')
        stream.seek(offset)
        magic, version, count, total = header.unpack(stream.read(header.size))
        if magic != b'MONADOP1' or version != 1 or not 1 <= count <= 100000:
            raise ValueError('Invalid setup payload header')
        result, records, previous, summed = None, {}, '', 0
        for _ in range(count):
            length, flag, rawsize, storedsize, checksum = record.unpack(stream.read(record.size))
            if not 0 < length <= 4096 or flag not in (0, 1) or max(rawsize,storedsize) > MAX_MEMBER:
                raise ValueError('Invalid payload member bounds')
            name = safe_name(stream.read(length).decode('ascii'))
            if name.casefold() <= previous: raise ValueError('Duplicate/unordered payload member')
            previous = name.casefold(); records[name] = {'bytes':rawsize,'sha256':checksum.hex()}; summed += rawsize
            if name == 'payload-manifest.json':
                if rawsize > 16*1024*1024: raise ValueError('Manifest exceeds bound')
                data = stream.read(storedsize)
                if flag:
                    if os.name != 'nt': raise ValueError('Compressed native payload inspection requires Windows')
                    from ctypes import wintypes as W
                    cabinet = ctypes.WinDLL('cabinet', use_last_error=True)
                    cabinet.CreateDecompressor.argtypes = [W.DWORD,ctypes.c_void_p,ctypes.POINTER(ctypes.c_void_p)]
                    cabinet.Decompress.argtypes = [ctypes.c_void_p,ctypes.c_void_p,ctypes.c_size_t,ctypes.c_void_p,ctypes.c_size_t,ctypes.POINTER(ctypes.c_size_t)]
                    cabinet.CloseDecompressor.argtypes = [ctypes.c_void_p]
                    handle, actual = ctypes.c_void_p(), ctypes.c_size_t()
                    if not cabinet.CreateDecompressor(4,None,ctypes.byref(handle)): raise ctypes.WinError(ctypes.get_last_error())
                    try:
                        source, target = ctypes.create_string_buffer(data), ctypes.create_string_buffer(rawsize)
                        if not cabinet.Decompress(handle,source,len(data),target,rawsize,ctypes.byref(actual)): raise ctypes.WinError(ctypes.get_last_error())
                        if actual.value != rawsize: raise ValueError('Invalid decompressed manifest length')
                        data = target.raw[:actual.value]
                    finally: cabinet.CloseDecompressor(handle)
                if identity(data) != records[name]: raise ValueError('Payload manifest checksum differs')
                result = json.loads(data)
            else:
                stream.seek(storedsize,1)
        if stream.tell() != offset+size or summed != total or result is None:
            raise ValueError('Incomplete payload')
        if result.get('files') != {k:v for k,v in records.items() if k!='payload-manifest.json'}:
            raise ValueError('Payload manifest membership differs')
        return result


def original_package(path, package, selected):
    if file_identity(path) != {'bytes':package['bytes'],'sha256':package['sha256']}:
        raise ValueError('Original binary package differs: '+package['name'])
    metadata, members, links, owned = {}, {}, {}, {}
    with ztar(path) as archive:
        for member in archive:
            name = safe_name(member.name)
            if member.size > MAX_MEMBER: raise ValueError('Oversized package member')
            if name in ('.PKGINFO','.BUILDINFO'):
                metadata[name] = archive.extractfile(member).read()
            elif member.isfile():
                with archive.extractfile(member) as source:
                    members[name] = {'bytes':member.size,'sha256':hashlib.file_digest(source,'sha256').hexdigest()}
            elif member.issym() or member.islnk():
                target = member.linkname
                target = target if member.islnk() else posixpath.join(posixpath.dirname(name),target)
                links[name] = safe_name(posixpath.normpath(target))
    def resolved(name, seen=()):
        if name in seen: raise ValueError('Cyclic package alias')
        return resolved(links[name],(*seen,name)) if name in links else members.get(name)
    for name, expected in selected.items():
        actual = resolved('ucrt64/'+name)
        if actual is not None:
            if actual != expected: raise ValueError('Binary package/payload member mismatch: '+name)
            owned[name] = expected
    pkg, build = fields(metadata['.PKGINFO']), fields(metadata['.BUILDINFO'])
    if one(pkg,'pkgname') != package['name'] or one(pkg,'pkgver') != package['version']:
        raise ValueError('Binary package metadata differs from lock')
    if one(build,'pkgbase') != one(pkg,'pkgbase') or one(build,'pkgver') != package['version']:
        raise ValueError('Binary build metadata differs')
    return {'package':package,'pkginfo':pkg,'buildinfo':build,'metadata':metadata,'owned':owned}


def source_basename(value):
    value = value.split('::',1)[0] if '::' in value else urllib.parse.urlsplit(value).path.rstrip('/').rsplit('/',1)[-1]
    return safe_name(value.removesuffix('.git'))


def validate_source(path, base, version, recipe_sha):
    metadata, hashes, directories, licenses = {}, {}, set(), []
    with ztar(path) as archive:
        for member in archive:
            name = safe_name(member.name)
            if name.split('/')[0] != base: raise ValueError('Source archive escapes its package base')
            if member.size > MAX_MEMBER: raise ValueError('Oversized source archive input')
            if member.isdir(): directories.add(name); continue
            if member.isfile():
                is_metadata=name in (base+'/PKGBUILD',base+'/.SRCINFO')
                is_license=LICENSE.fullmatch(PurePosixPath(name).name)
                if is_metadata or is_license:
                    if member.size > 4*1024*1024: raise ValueError('Oversized license text')
                    data=archive.extractfile(member).read()
                    digest=hashlib.sha256(data).hexdigest()
                    if is_metadata:metadata[name.rsplit('/',1)[1]]=data
                    if is_license:licenses.append((name,data))
                else:
                    with archive.extractfile(member) as stream:
                        digest=hashlib.file_digest(stream,'sha256').hexdigest()
                hashes[name] = {'bytes':member.size,'sha256':digest}
    if hashlib.sha256(metadata['PKGBUILD']).hexdigest() != recipe_sha:
        raise ValueError('Source PKGBUILD does not match original binary .BUILDINFO')
    info = fields(metadata['.SRCINFO'])
    source_version = one(info,'pkgver')+'-'+one(info,'pkgrel')
    if info.get('epoch', ['0'])[0] != '0': source_version = one(info,'epoch')+'~'+source_version
    if one(info,'pkgbase') != base or source_version != version:
        raise ValueError('All-source archive version differs from original binary')
    inputs, additional_checksums = [], {}
    for suffix in ('','_x86_64'):
        sources = info.get('source'+suffix,[])
        algorithms = {a:info[a+'sums'+suffix] for a in ('sha256','sha512','sha1','b2','md5') if a+'sums'+suffix in info}
        if sources and (not algorithms or any(len(v)!=len(sources) for v in algorithms.values())):
            raise ValueError('Source inputs lack complete checksum declarations')
        for index, url in enumerate(sources):
            name = source_basename(url); key = base+'/'+name; entry = {'name':name,'source':url,'declared_checksums':{a:v[index] for a,v in algorithms.items()}}
            expected = {a:v[index] for a,v in algorithms.items() if v[index]!='SKIP'}
            if key in hashes:
                entry.update(kind='file',identity=hashes[key])
                if expected.get('sha256') and hashes[key]['sha256'] != expected['sha256']:
                    raise ValueError('Source input checksum differs: '+name)
                for algorithm,value in expected.items():
                    if algorithm!='sha256':additional_checksums.setdefault(key,{})[algorithm]=value
                if not expected and not name.endswith(('.asc','.sig')):
                    raise ValueError('Unpinned non-signature source input: '+name)
            elif key in directories and ('git+' in url or url.startswith(('hg+','svn+'))):
                # The exact official all-source archive preserves the VCS
                # object store. A moving branch/tag alone is not qualified.
                fragment=urllib.parse.urlsplit(url.split('::')[-1]).fragment
                if not fragment.startswith(('commit=','tag=')):
                    raise ValueError('VCS input lacks an immutable revision/tag: '+url)
                git_prefix=key+'/'
                if 'git+' in url and not (
                        any(n.startswith(git_prefix+'objects/') or n.startswith(git_prefix+'.git/objects/') for n in hashes) and
                        (git_prefix+'HEAD' in hashes or git_prefix+'.git/HEAD' in hashes)):
                    raise ValueError('All-source archive lacks the Git object store: '+name)
                entry.update(kind='vcs-object-store',directory=key,revision=fragment)
            else: raise ValueError('All-source archive omits declared source input: '+name)
            inputs.append(entry)
    if not inputs: raise ValueError('All-source archive declares no source inputs')
    if additional_checksums:
        with ztar(path) as archive:
            for member in archive:
                if member.name in additional_checksums:
                    checks={a:hashlib.new('blake2b' if a=='b2' else a) for a in additional_checksums[member.name]}
                    with archive.extractfile(member) as stream:
                        while block:=stream.read(1024*1024):
                            for digest in checks.values():digest.update(block)
                    if any(checks[a].hexdigest()!=value for a,value in additional_checksums[member.name].items()):
                        raise ValueError('Source input checksum differs: '+member.name)
    return {'metadata':metadata,'inputs':inputs,'member_count':len(hashes),'licenses':licenses}


def nested_notices(path):
    """Expose notices already supplied inside retained upstream source tarballs."""
    notices=[]
    with ztar(path) as archive:
        for member in archive:
            if not member.isfile() or not member.name.lower().endswith(('.tar.gz','.tgz','.tar.bz2','.tar.xz','.tar')):
                continue
            with archive.extractfile(member) as source,tarfile.open(fileobj=source,mode='r|*') as upstream:
                for inner in upstream:
                    if inner.isfile() and LICENSE.fullmatch(PurePosixPath(inner.name).name):
                        if inner.size>4*1024*1024:raise ValueError('Oversized upstream notice')
                        notices.append((member.name+'!'+inner.name,upstream.extractfile(inner).read()))
    return notices


def git_source_evidence(path, entry, work):
    """Match makepkg6.1's Git archive checksum without running project code.

    Preserve Git objects/refs only in a fresh private inspection folder. Ignore
    source-repository configuration/hooks and all inherited Git overrides. The
    Git commands are read-only; no checkout, hooks, filters, fetch or build runs.
    """
    executable=shutil.which('git')
    if not executable:raise ValueError('Git is required to verify original VCS source checksums')
    target=work/(file_identity(path)['sha256'][:16]+'-'+entry['name'])
    if target.exists():raise ValueError('Git inspection folder must be fresh: '+str(target))
    target.mkdir(parents=True)
    prefix=entry['directory']+'/'
    with ztar(path) as archive:
        for member in archive:
            if not member.name.startswith(prefix) or not member.isfile():continue
            relative=member.name[len(prefix):]
            if not (relative=='HEAD' or relative=='packed-refs' or relative.startswith(('refs/','objects/'))):continue
            if relative in ('objects/info/alternates','objects/info/http-alternates') or relative.startswith('refs/replace/'):
                raise ValueError('External/replaced Git source objects are not self-contained')
            safe_name(relative);destination=target/relative;destination.parent.mkdir(parents=True,exist_ok=True)
            with archive.extractfile(member) as source,destination.open('xb') as output:shutil.copyfileobj(source,output)
    (target/'config').write_text('[core]\nrepositoryformatversion = 0\nbare = true\n',encoding='ascii')
    (target/'refs').mkdir(exist_ok=True)
    environment={k:v for k,v in os.environ.items() if not k.upper().startswith('GIT')}
    environment.update(GIT_CONFIG_NOSYSTEM='1',GIT_CONFIG_GLOBAL=os.devnull,GIT_CONFIG_SYSTEM=os.devnull,
                       GIT_NO_REPLACE_OBJECTS='1',GIT_OPTIONAL_LOCKS='0',GIT_NO_LAZY_FETCH='1')
    base=[executable,'--no-replace-objects','-c','safe.directory='+str(target),'-c','core.abbrev=no',
          '-c','core.hooksPath='+str(target/'disabled-hooks'),'-c','gc.auto=0','-c','maintenance.auto=false',
          '-c','protocol.allow=never','--git-dir='+str(target)]
    revision=entry['revision'].split('=',1)[1]
    if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._/+-]{0,199}',revision):raise ValueError('Invalid pinned Git revision')
    def run(arguments):
        value=subprocess.run([*base,*arguments],cwd=PORT,env=environment,capture_output=True,timeout=180)
        if value.returncode:raise ValueError('Read-only Git source validation failed: '+value.stderr.decode('utf-8',errors='replace')[-2000:])
        return value.stdout
    commit=run(['rev-parse','--verify',revision+'^{commit}']).decode().strip()
    if not re.fullmatch('[0-9a-f]{40,64}',commit):raise ValueError('Pinned source Git commit is unavailable')
    expected={a:v for a,v in entry['declared_checksums'].items() if v!='SKIP'}
    checks={a:hashlib.new('blake2b' if a=='b2' else a) for a in expected}
    checks.setdefault('sha256',hashlib.sha256())
    with subprocess.Popen([*base,'archive','--format','tar',revision],cwd=PORT,env=environment,stdout=subprocess.PIPE,stderr=subprocess.PIPE) as process:
        assert process.stdout is not None
        size=0
        while block:=process.stdout.read(1024*1024):
            size+=len(block)
            if size>4*1024**3:
                process.terminate();process.wait();raise ValueError('Git source archive exceeds bound')
            for digest in checks.values():digest.update(block)
        error=process.stderr.read();code=process.wait()
    if code or any(checks[a].hexdigest()!=value for a,value in expected.items()):
        raise ValueError('Pinned Git source archive checksum differs: '+entry['name']+' '+error.decode('utf-8',errors='replace')[-1000:])
    return {'commit':commit,'git_archive_bytes':size,'git_archive_sha256':checks['sha256'].hexdigest(),
            'declared_checksums_verified':bool(expected),'pinned_commit_verified':True,
            'git_version':subprocess.check_output([executable,'--version'],env=environment,text=True).strip(),
            'head_tree':run(['ls-tree','-r','-l',commit]).decode('utf-8',errors='strict')}


def graphite_reviewed_source(path, cache):
    """Remove one unrelated website font installer, retaining all build source.

    The original upstream tarball and recipes are authenticated first. Preserve
    original recipes as provenance; the distributed recipe changes only its
    local source location/checksum. No source program is executed or edited.
    """
    import zstandard
    base='mingw-w64-graphite2'; nested=base+'/graphite2-1.3.15.tgz'
    drop='graphite2-1.3.15/site/assets/resources/CuneiformGraphiteFont.zip'
    expected_drop={'bytes':2557085,'sha256':'5f80989524d6b63c99a0e04e38edf25906ec097044d7afb44aedb5260dc83c7c'}
    original_identity=file_identity(path); members=[]
    with ztar(path) as archive:
        for member in archive:
            members.append((member,archive.extractfile(member).read() if member.isfile() else None))
    raw=dict((m.name,data) for m,data in members)
    old_tar=raw[nested]
    if hashlib.sha256(old_tar).hexdigest()!='c6bc8b4252724665297f7cad0c55897285c673f9b8e6db3522ace833593fe0b1':
        raise ValueError('Graphite original upstream source differs')
    output=io.BytesIO(); removed=None; preserved={}
    with tarfile.open(fileobj=io.BytesIO(old_tar),mode='r:gz') as source,tarfile.open(fileobj=output,mode='w') as target:
        for member in source:
            data=source.extractfile(member).read() if member.isfile() else None
            if member.name==drop:
                if data is None or identity(data)!=expected_drop:raise ValueError('Graphite omitted website asset changed')
                removed={'member':drop,**identity(data)};continue
            if member.isfile():preserved[member.name]=identity(data)
            if member.isfile() and (member.name.endswith(('.cmake','.py','.sh')) or PurePosixPath(member.name).name in ('CMakeLists.txt','Makefile','configure')):
                if b'CuneiformGraphiteFont.zip' in data:raise ValueError('Website installer unexpectedly participates in the build')
            target.addfile(member,io.BytesIO(data) if data is not None else None)
    if removed is None:raise ValueError('Expected Graphite website asset was not present')
    new_tar=gzip.compress(output.getvalue(),mtime=0)
    with tarfile.open(fileobj=io.BytesIO(new_tar),mode='r:gz') as check:
        actual={m.name:identity(check.extractfile(m).read()) for m in check if m.isfile()}
    if actual!=preserved:raise ValueError('Graphite library/test source changed during repacking')
    old_hash=hashlib.sha256(old_tar).hexdigest();new_hash=hashlib.sha256(new_tar).hexdigest()
    recipe,srcinfo=raw[base+'/PKGBUILD'],raw[base+'/.SRCINFO']
    if recipe.count(old_hash.encode())!=1 or srcinfo.count(old_hash.encode())!=1:raise ValueError('Ambiguous original Graphite source checksum')
    new_recipe=recipe.replace(old_hash.encode(),new_hash.encode()).replace(
        b'"${msys2_repository_url}/releases/download/${pkgver}/${_realname}-${pkgver}.tgz"',b'"${_realname}-${pkgver}.tgz"')
    new_info=srcinfo.replace(old_hash.encode(),new_hash.encode()).replace(
        b'https://github.com/silnrsi/graphite/releases/download/1.3.15/graphite2-1.3.15.tgz',b'graphite2-1.3.15.tgz')
    provenance={'schema':1,'reason':'Remove unrelated non-build website font-demo installer; all library/test source and patches retained.',
                'original_archive':original_identity,'original_upstream':identity(old_tar),'provided_upstream':identity(new_tar),
                'original_pkgbuild_sha256':hashlib.sha256(recipe).hexdigest(),'provided_pkgbuild_sha256':hashlib.sha256(new_recipe).hexdigest(),
                'removed_members':[removed],'preserved_source_members_sha256':identity(encoded(preserved))['sha256']}
    replacements={nested:new_tar,base+'/PKGBUILD':new_recipe,base+'/.SRCINFO':new_info}
    rebuilt=io.BytesIO()
    with tarfile.open(fileobj=rebuilt,mode='w') as target:
        for member,data in members:
            data=replacements.get(member.name,data)
            if data is not None:member.size=len(data)
            target.addfile(member,io.BytesIO(data) if data is not None else None)
        for name,data in ((base+'/ORIGINAL-PKGBUILD',recipe),(base+'/ORIGINAL-SRCINFO',srcinfo),(base+'/SOURCE-REPACK.json',encoded(provenance))):
            member=tarfile.TarInfo(name);member.size=len(data);member.mode=0o644;target.addfile(member,io.BytesIO(data))
    data=zstandard.ZstdCompressor(level=19).compress(rebuilt.getvalue())
    cache.mkdir(parents=True,exist_ok=True);destination=cache/'mingw-w64-graphite2-1.3.15-1.source-reviewed.src.tar.zst'
    if destination.exists():
        if file_identity(destination)!=identity(data):raise ValueError('Prior reviewed Graphite source differs; preserve it')
    else:
        with destination.open('xb') as stream:stream.write(data)
    return destination,provenance


def fetch_source(base, version, cache, download):
    filename = safe_name(base+'-'+version+'.src.tar.zst')
    path, url = cache/filename, ORIGIN+urllib.parse.quote(filename)
    if not path.exists():
        if not download: raise ValueError('Missing exact source archive: '+filename)
        temporary = path.with_name(path.name+'.download-'+os.urandom(4).hex())
        with urllib.request.urlopen(url,timeout=120) as source, temporary.open('xb') as target:
            while block:=source.read(1024*1024):
                target.write(block)
                if target.tell()>2*1024**3: raise ValueError('Source archive download exceeds bound')
        temporary.rename(path)
    return path,url


def verify_report(path, *, payload_files, release_version, static_files=None, relink_root=None):
    """Verify retained real source/notice files and exact final binary coverage."""
    path=Path(path);report=json.loads(path.read_bytes());root=path.parent
    if (report.get('schema')!=1 or report.get('kind')!='windows-native-release-sources' or
            report.get('platform')!='windows-amd64' or report.get('release_version')!=release_version or
            report.get('status')!='complete' or report.get('unresolved') or
            report.get('admissibility',{}).get('status')!='passed'):
        raise ValueError('Native source qualification is incomplete')
    expected={n:v for n,v in payload_files.items() if n.startswith('native/ucrt64/')}
    if report.get('binary_inputs')!=expected:
        raise ValueError('Final native payload differs from the source-qualified binary closure')
    if static_files is not None and report.get('static_inputs')!=static_files:
        raise ValueError('Final statically linked libraries differ from qualified sources')
    review,review_raw=source_review()
    qualification=review['qualified']
    # Exact package ownership, original BUILDINFO/PKGINFO identities, all source
    # archives/inputs and notices are independently pinned by reviewed source.
    # In particular [] and source_required=False cannot waive GPL coverage.
    for key,value in qualification.items():
        if key!='files' and report.get(key)!=value:
            raise ValueError('Native source coverage differs from the reviewed dependency closure: '+key)
    expected_files=dict(qualification['files'])
    expected_files['review/native-source-review.json']=identity(review_raw)
    if report.get('relink'):
        proof=report['relink'];recipe=proof.get('recipe_data',{})
        validate_relink_recipe(recipe,report['static_inputs'])
        for item in recipe['objects']+[recipe['resource']]+recipe['static_libraries']:
            expected_files['relink/'+safe_name(item['path'])]={k:item[k] for k in ('bytes','sha256')}
        expected_files['relink/relink.json']=proof['recipe']
        repacker=PORT/'tools/relink_windows_setup.py'
        expected_files['relink/relink_windows_setup.py']=file_identity(repacker)
        expected_files['relink/README.txt']=identity(RELINK_README)
        expected_files['relink/README.md']=file_identity(PORT/'tools/RELINK_WINDOWS_SETUP.md')
    files=report.get('files')
    if files!=expected_files:raise ValueError('Native source file inventory differs from required reviewed sources/notices')
    actual=set();pending=[root]
    allowed_directories={'.'}
    for name in files:
        parent=PurePosixPath(name).parent
        while str(parent)!='.':allowed_directories.add(str(parent));parent=parent.parent
    while pending:
        directory=pending.pop()
        if directory.is_symlink() or directory.is_junction():raise ValueError('Native source directory is a reparse point')
        with os.scandir(directory) as entries:
            for entry in entries:
                node=Path(entry.path);relative=node.relative_to(root).as_posix();info=entry.stat(follow_symlinks=False)
                if entry.is_symlink() or getattr(info,'st_file_attributes',0)&0x400:
                    raise ValueError('Native source companion contains a reparse member')
                if stat.S_ISDIR(info.st_mode):
                    if relative not in allowed_directories:raise ValueError('Native source companion contains an unknown directory')
                    pending.append(node)
                elif stat.S_ISREG(info.st_mode):actual.add(relative)
                else:raise ValueError('Native source companion contains a special member')
    if actual!=set(files)|{path.name}:raise ValueError('Native source companion has unknown/missing files')
    for name,expected_file in files.items():
        safe_name(name);candidate=root/name
        if candidate.is_symlink() or candidate.is_junction() or candidate.stat().st_nlink!=1 or file_identity(candidate)!=expected_file:
            raise ValueError('Native source/notice/relink file changed: '+name)
    by_archive={a['path']:a for a in report['artifacts']}
    for package in report['packages']:
        if not package.get('source_required'):continue
        source=package.get('source_archive',{})
        if source.get('path') not in by_archive or files.get(source['path'])!={k:source[k] for k in ('bytes','sha256')}:
            raise ValueError('A required native package has no retained exact source archive')
        build=fields((root/('metadata/'+package['name']+'/BUILDINFO')).read_bytes())
        if one(build,'pkgbuild_sha256sum')!=package['pkgbuild_sha256']:
            raise ValueError('Retained original build recipe identity differs')
        for item in package.get('source_inputs',[]):
            if item['kind']=='vcs-object-store' and not item.get('git_evidence',{}).get('pinned_commit_verified'):
                raise ValueError('Required Git source revision/checksum is unqualified')
    for item in report.get('notices',{}).values():
        if files.get(item['path'])!={k:item[k] for k in ('bytes','sha256')}:
            raise ValueError('Required native license notice is not retained')
    if relink_root is not None:
        relink_root=Path(relink_root)
        proof=report.get('relink')
        if not proof:raise ValueError('Statically linked frontend lacks its relink companion')
        recipe=json.loads((relink_root/'relink.json').read_bytes())
        if file_identity(relink_root/'relink.json')!=proof['recipe'] or recipe!=proof['recipe_data']:
            raise ValueError('Final frontend relink recipe differs from the companion')
        for item in recipe['objects']+[recipe['resource']]+recipe['static_libraries']:
            if file_identity(relink_root/item['path'])!={k:item[k] for k in ('bytes','sha256')}:
                raise ValueError('Final frontend relink member changed')
        receipt=json.loads((relink_root/'frontend-receipt.json').read_bytes())
        if receipt['source_inputs']!=proof['source_inputs'] or receipt['compiler']!=proof['compiler']:
            raise ValueError('Final frontend compiler/source identities differ from relink evidence')
        if receipt['base_executable']!=proof['base_executable'] or file_identity(relink_root/receipt['base_executable']['path'])!={k:proof['base_executable'][k] for k in ('bytes','sha256')}:
            raise ValueError('Final native frontend executable differs from relink evidence')
    return report


def validate_relink_recipe(recipe,static_inputs):
    if (recipe.get('schema')!=1 or recipe.get('working_directory')!='.' or recipe.get('compiler')!='Zig 0.14.1' or
            recipe.get('output')!='builder-base.exe' or len(recipe.get('objects',[]))!=7):
        raise ValueError('Expected the checked branded native frontend relink recipe')
    static={item['dependency_path']:{k:item[k] for k in ('bytes','sha256')} for item in recipe['static_libraries']}
    if static!=static_inputs:raise ValueError('Frontend static archives do not match source-qualified libraries')
    allowed={'tools/builder/windows_builder.cpp','third_party/imgui/imgui.cpp','third_party/imgui/imgui_draw.cpp',
             'third_party/imgui/imgui_tables.cpp','third_party/imgui/imgui_widgets.cpp',
             'third_party/imgui/backends/imgui_impl_sdl3.cpp','third_party/imgui/backends/imgui_impl_sdlrenderer3.cpp'}
    if {item.get('source') for item in recipe['objects']}!=allowed:raise ValueError('Relink companion contains unexpected non-frontend objects')
    system=['m','kernel32','user32','gdi32','winmm','imm32','ole32','oleaut32',
            'version','uuid','advapi32','setupapi','shell32','dinput8','bcrypt','cabinet','comctl32']
    if recipe.get('system_libraries')!=system or recipe['resource']['path']!='objects/builder.res':
        raise ValueError('Frontend relink resource/system-library closure differs')
    if any(item['path']!='objects/'+item['source'].replace('/','_')+'.o' for item in recipe['objects']):
        raise ValueError('Frontend relink object path differs from its authored source')
    if any(item['path']!='static-libs/'+PurePosixPath(item['dependency_path']).name for item in recipe['static_libraries']):
        raise ValueError('Frontend relink static-library path differs')
    expected=['c++','-march=x86_64_v2','-static',*(i['path'] for i in recipe['objects']),recipe['resource']['path'],
              *(i['path'] for i in recipe['static_libraries']),*('-l'+name for name in system),
              '-Wl,--subsystem,windows','-municode','-o','builder-base.exe']
    if recipe.get('argv')!=expected:raise ValueError('Frontend relink recipe has missing/unreviewed link inputs')


def seal_relink(report_path, frontend_root, output):
    """Create a NEW source companion with all current frontend relink inputs."""
    report_path,frontend_root,output=Path(report_path),Path(frontend_root),Path(output)
    report=json.loads(report_path.read_bytes())
    verify_report(report_path,payload_files=report['binary_inputs'],release_version=report['release_version'],static_files=report['static_inputs'])
    if output.exists():raise ValueError('Final relink companion must be fresh')
    recipe_raw=(frontend_root/'relink.json').read_bytes();recipe=json.loads(recipe_raw)
    receipt=json.loads((frontend_root/'frontend-receipt.json').read_bytes())
    validate_relink_recipe(recipe,report['static_inputs'])
    if receipt.get('relink_recipe')!=recipe:
        raise ValueError('Expected the checked branded native frontend relink recipe')
    output.mkdir(parents=True)
    files=dict(report['files'])
    for name,expected in report['files'].items():
        target=output/name;target.parent.mkdir(parents=True,exist_ok=True)
        with (report_path.parent/name).open('rb') as source,target.open('xb') as destination:shutil.copyfileobj(source,destination)
        if file_identity(target)!=expected:raise ValueError('Source changed during final relink sealing')
    members=recipe['objects']+[recipe['resource']]+recipe['static_libraries']
    for item in members:
        relative=safe_name(item['path']);target=output/'relink'/relative;target.parent.mkdir(parents=True,exist_ok=True)
        with (frontend_root/relative).open('rb') as source,target.open('xb') as destination:shutil.copyfileobj(source,destination)
        files['relink/'+relative]=file_identity(target)
        if files['relink/'+relative]!={k:item[k] for k in ('bytes','sha256')}:raise ValueError('Relink member changed during copy')
    (output/'relink/relink.json').write_bytes(recipe_raw);files['relink/relink.json']=identity(recipe_raw)
    repacker=PORT/'tools/relink_windows_setup.py'
    repacker_raw=repacker.read_bytes();(output/'relink/relink_windows_setup.py').write_bytes(repacker_raw)
    files['relink/relink_windows_setup.py']=identity(repacker_raw)
    (output/'relink/README.txt').write_bytes(RELINK_README);files['relink/README.txt']=identity(RELINK_README)
    instructions=(PORT/'tools/RELINK_WINDOWS_SETUP.md').read_bytes()
    (output/'relink/README.md').write_bytes(instructions);files['relink/README.md']=identity(instructions)
    proof={'recipe':identity(recipe_raw),'recipe_data':recipe,'base_executable':receipt['base_executable'],
           'source_inputs':receipt['source_inputs'],'compiler':receipt['compiler'],
           'purpose':'Replace/rebuild libiconv and relink the native frontend; no guest/game objects included.'}
    report=dict(report,files=files,relink=proof)
    target=output/'native-sources.json';target.write_bytes(encoded(report))
    verify_report(target,payload_files=report['binary_inputs'],release_version=report['release_version'],
                  static_files=report['static_inputs'],relink_root=frontend_root)
    return target


def collect(args):
    output, cache = args.output.absolute(), args.source_cache.absolute()
    if output.exists(): raise ValueError('Source companion output must be fresh')
    output.mkdir(parents=True); cache.mkdir(parents=True,exist_ok=True)
    document = payload_manifest(args.setup_exe,args.setup_exe_sha256)
    selected = {n.removeprefix('native/ucrt64/'):v for n,v in document['files'].items() if n.startswith('native/ucrt64/')}
    binary_inputs = {'native/ucrt64/'+n:v for n,v in selected.items()}
    for generated in ('content-inventory.json','receipt.json'): selected.pop(generated,None)
    lock_raw=args.dependency_lock.read_bytes(); lock=json.loads(lock_raw)
    prefix_inventory=json.loads((args.prefix/'content-inventory.json').read_bytes())
    static_inputs={}
    for name in args.static_library:
        safe_name(name)
        static_inputs[name]=file_identity(args.prefix/name)
        if static_inputs[name] != prefix_inventory.get(name): raise ValueError('Static archive differs from locked prefix: '+name)
    selected.update(static_inputs)
    review,review_raw=source_review()
    if (binary_inputs!=review['qualified']['binary_inputs'] or static_inputs!=review['qualified']['static_inputs'] or
            args.version!=review['qualified']['release_version']):
        raise ValueError('This binary/static/version closure needs a new explicit corresponding-source review')
    for name,value in selected.items():
        if value != prefix_inventory.get(name) or file_identity(args.prefix/name)!=value:
            raise ValueError('Native payload/static library differs from checked prefix: '+name)
    files, artifacts, packages, notices, unresolved = {},[],[],{},[]
    report={'schema':1,'kind':'windows-native-release-sources','platform':'windows-amd64','release_version':args.version,
            'status':'incomplete','dependency_lock_sha256':hashlib.sha256(lock_raw).hexdigest(),
            'binary_inputs':binary_inputs,'binary_inputs_sha256':identity(encoded(binary_inputs))['sha256'],
            'static_inputs':static_inputs,'packages':packages,'artifacts':artifacts,'files':files,'notices':notices,'unresolved':unresolved}
    def write(name,data):
        path=output/name;path.parent.mkdir(parents=True,exist_ok=True)
        with path.open('xb') as stream:stream.write(data)
        files[name]=identity(data)
    def copy(name,path,expected=None):
        destination=output/name;destination.parent.mkdir(parents=True,exist_ok=True)
        with path.open('rb') as source,destination.open('xb') as target:shutil.copyfileobj(source,target)
        files[name]=file_identity(destination)
        if expected is not None and files[name]!=expected:
            raise ValueError('Source/notices changed while copying: '+name)
    owners={n:[] for n in selected}; bases={}
    try:
        for package in lock['packages']:
            path=args.binary_cache/package['url'].rsplit('/',1)[-1]
            original=original_package(path,package,selected)
            if not original['owned']:continue
            for name in original['owned']:owners[name].append(package['name'])
            base,version=one(original['pkginfo'],'pkgbase'),package['version']
            recipe=one(original['buildinfo'],'pkgbuild_sha256sum')
            item={'name':package['name'],'version':version,'pkgbase':base,'binary_package':package,
                  'pkgbuild_sha256':recipe,'shipped_files':original['owned'],'source_required':any(not n.startswith('share/licenses/') for n in original['owned'])}
            for name,raw in original['metadata'].items():write('metadata/'+package['name']+'/'+name.removeprefix('.'),raw)
            packages.append(item)
            if item['source_required']:
                if (base,version) in bases and bases[base,version]['recipe']!=recipe:raise ValueError('Split binary recipes differ')
                bases[base,version]={'recipe':recipe,'packages':[]}
            if base in HEADER_ONLY:
                code={n:v for n,v in original['owned'].items() if not n.startswith('share/licenses/')}
                if not code or any(not n.startswith('include/') for n in code):
                    raise ValueError('Header-only source disposition cannot waive compiled code: '+base)
                item['source_required']=False
                item['source_disposition']={'kind':'exact-shipped-header-source','reason':HEADER_ONLY[base]}
                item['header_sources']={}
                for name,value in code.items():
                    target='header-sources/'+package['name']+'/'+name
                    copy(target,args.prefix/name,value);item['header_sources'][target]=value
            for name in original['owned']:
                if name.startswith('share/licenses/') and name not in notices:
                    target='licenses/native/'+name.removeprefix('share/licenses/')
                    copy(target,args.prefix/name,selected[name]);notices[target]={'path':target,**files[target]}
        missing=[n for n,v in owners.items() if not v]
        if missing:raise ValueError('Native files have no original binary-package owner: '+str(missing[:20]))
        for (base,version),requirement in sorted(bases.items()):
            print('Checking corresponding source '+base+' '+version,flush=True)
            path,url=fetch_source(base,version,cache,args.download)
            source_identity=file_identity(path)
            verified=validate_source(path,base,version,requirement['recipe'])
            for entry in verified['inputs']:
                if entry['kind']=='vcs-object-store':
                    proof=git_source_evidence(path,entry,cache/('git-review-'+os.urandom(6).hex()))
                    proof.pop('head_tree',None);proof.pop('git_version',None);entry['git_evidence']=proof
            artifact=None
            if base=='mingw-w64-graphite2':
                provided,provenance=graphite_reviewed_source(path,cache/'reviewed')
                verified=validate_source(provided,base,version,provenance['provided_pkgbuild_sha256'])
                relative='archives/'+provided.name;copy(relative,provided,file_identity(provided))
                artifact={'path':relative,'url':url,**files[relative],'purpose':'reviewed-all-source-non-build-installer-removed',
                          'source_repack':provenance}
                artifacts.append(artifact)
            elif base not in HEADER_ONLY:
                relative='archives/'+path.name;copy(relative,path,source_identity)
                artifact={'path':relative,'url':url,**files[relative],'purpose':'complete-exact-msys2-all-source-with-build-recipe-and-patches'}
                artifacts.append(artifact)
            for package in packages:
                if package['pkgbase']==base and package['version']==version:
                    package.update(source_inputs=verified['inputs'],source_members=verified['member_count'])
                    if artifact is not None:package['source_archive']=artifact
                    else:package['omitted_optional_archive']={'url':url,**source_identity}
            for source_name,raw in verified['licenses']+nested_notices(path):
                target='licenses/native/'+base+'/'+hashlib.sha256(source_name.encode()).hexdigest()[:16]+'-'+re.sub('[^A-Za-z0-9._-]','_',PurePosixPath(source_name).name)
                if target not in files:write(target,raw)
                notices[target]={'path':target,**files[target]}
        report['admissibility']=review['qualified']['admissibility']
        write('review/native-source-review.json',review_raw)
        report['status']='complete'
        (output/'native-sources.json').write_bytes(encoded(report))
        verify_report(output/'native-sources.json',payload_files=binary_inputs,release_version=args.version,static_files=static_inputs)
    except Exception as error:
        unresolved.append(str(error));raise
    finally:
        (output/'native-sources.json').write_bytes(encoded(report))
    return report


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--setup-exe',type=Path,required=True)
    parser.add_argument('--setup-exe-sha256',required=True)
    parser.add_argument('--dependency-lock',type=Path,default=PORT/'tools/windows-deps.lock.json')
    parser.add_argument('--prefix',type=Path,required=True)
    parser.add_argument('--binary-cache',type=Path,default=PORT/'.deps/downloads')
    parser.add_argument('--source-cache',type=Path,default=PORT/'.deps/release-source-archives')
    parser.add_argument('--static-library',action='append',default=[])
    parser.add_argument('--version',default='0.1.0')
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--download',action='store_true')
    args=parser.parse_args()
    if not HEX.fullmatch(args.setup_exe_sha256) or not re.fullmatch(r'[0-9]+\.[0-9]+\.[0-9]+',args.version):parser.error('Exact setup SHA256 and independent Windows version required')
    report=collect(args)
    print(json.dumps({'status':report['status'],'packages':len(report['packages']),'source_archives':len(report['artifacts']),'output':str(args.output)},indent=2))


if __name__=='__main__':main()
