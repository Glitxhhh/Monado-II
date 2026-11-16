#!/usr/bin/env python3
"""Build reviewed native FSR4 preparation tools from hash-pinned LOCAL archives.

Never downloads a vendor SDK/model, installs packages, accepts terms or changes
drivers. Local vkd3d/DXVK support is copied with its explicit provenance receipt.
"""
from __future__ import annotations
import argparse, contextlib, hashlib, io, json, os, re, sys, tarfile
from pathlib import Path, PurePosixPath
sys.path.insert(0,str(Path(__file__).resolve().parent))
import setup_windows_fsr4 as component
from setup_windows_fsr4 import guard, sha, encoded, document, Image, ROOT
from fsr4_process_windows import run, Cancelled

LOCK=ROOT/'fsr4-preparation-windows.lock.json'
REQUIRED={'fsr4cap.exe','amdxc64.dll','dxil-spirv.exe','libdxil-spirv-c-shared.dll','spirv-as.exe','spirv-dis.exe','spirv-val.exe','fsr4-validate.exe','d3d12.dll','d3d12core.dll','dxgi.dll'}
SUPPORT={'d3d12.dll','d3d12core.dll','dxgi.dll'}
SYSTEM={'kernel32.dll','ntdll.dll','msvcrt.dll','ucrtbase.dll','advapi32.dll','bcrypt.dll','user32.dll','gdi32.dll','ole32.dll','shell32.dll','shlwapi.dll','version.dll','winmm.dll','ws2_32.dll','dxguid.dll','vulkan-1.dll','d3dcompiler_47.dll','cfgmgr32.dll','setupapi.dll','imm32.dll','opengl32.dll','dwmapi.dll'}

def progress(stage,current,total,message):
    print('MONADO_FSR4_PROGRESS '+json.dumps(dict(stage=stage,current=current,total=total,message=message)),flush=True)

@contextlib.contextmanager
def sources():
    with component.sources() as (runtime,_),guard.locked_directory(ROOT) as root,contextlib.ExitStack() as leases:
        raw=root.read_regular(LOCK.name,max_bytes=1024*1024);lock=document(raw)
        if lock.get('schema')!=1 or lock.get('platform')!='windows-amd64' or lock.get('model_identity')!=runtime['model_identity']:
            raise ValueError('Unsupported Windows preparation source lock')
        for name,item in lock['files'].items():
            stream=leases.enter_context(root.open_regular(name,max_bytes=4*1024*1024));data=stream.read()
            if len(data)!=item['bytes'] or sha(data)!=item['sha256']:raise ValueError('Preparation source changed: '+name)
        yield lock,raw

@contextlib.contextmanager
def fresh(output):
    output=Path(guard.canonical_path(output))
    with guard.locked_directory(output.parent,owner=True) as parent:
        output.mkdir()
        with parent.child(output.name,owner=True) as result:yield result

def unpack(data,directory):
    """One verified archive root; bounded regular files only, Windows-safe names."""
    seen=set();total=0;root=None
    with tarfile.open(fileobj=io.BytesIO(data),mode='r:gz') as archive:
        for member in archive:
            name=member.name
            if '\\' in name or '\x00' in name:raise ValueError('Unsafe archive separator')
            path=PurePosixPath(name);parts=path.parts
            if path.is_absolute() or not parts or any(p in ('','.','..') or ':' in p for p in parts):raise ValueError('Unsafe archive member')
            if root is None:root=parts[0]
            if parts[0]!=root:raise ValueError('Archive has multiple roots')
            if len(parts)==1:
                if member.isdir():continue
                raise ValueError('Archive root must be a directory')
            relative='/'.join(parts[1:]);key=relative.casefold()
            if key in seen:raise ValueError('Duplicate/case-colliding source member')
            seen.add(key)
            if len(seen)>20000:raise ValueError('Too many source members')
            if member.isdir():
                with directory.child(relative,create=True):pass
                continue
            if not member.isfile() or member.size>16*1024*1024:raise ValueError('Nonregular/oversized source member')
            total+=member.size
            if total>300*1024*1024:raise ValueError('Expanded source exceeds cap')
            payload=archive.extractfile(member).read(member.size+1)
            if len(payload)!=member.size:raise ValueError('Truncated archive member')
            leaf=PurePosixPath(relative)
            with directory.child(str(leaf.parent),create=True) if str(leaf.parent)!='.' else contextlib.nullcontext(directory) as parent:
                parent.write_new(leaf.name,payload)

def patch_class_bindings(directory,patch):
    """Exact-context accepted patch, no external patch tool, offsets or fuzz."""
    text=patch.decode('utf8').splitlines(keepends=True);changes=[];index=0
    while index<len(text):
        if not text[index].startswith('--- a/'):index+=1;continue
        name=text[index][6:].strip();index+=1
        if text[index].strip()!='+++ b/'+name:raise ValueError('Unsupported patch rename')
        index+=1;original=directory.read_regular(name,max_bytes=4*1024*1024);lines=original.decode('utf8').splitlines(keepends=True);out=[];position=0
        while index<len(text) and not text[index].startswith('diff --git '):
            if not text[index].startswith('@@ '):index+=1;continue
            match=re.match(r'@@ -(\d+),(\d+) \+(\d+),(\d+) @@',text[index]);index+=1
            if not match:raise ValueError('Unsupported patch hunk')
            start=int(match[1])-1
            if start<position:raise ValueError('Overlapping patch hunk')
            out.extend(lines[position:start]);position=start;old=[];new=[]
            while index<len(text) and text[index][:1] in (' ','+','-') and not text[index].startswith(('--- ','+++ ')):
                line=text[index];index+=1
                if line[0] in (' ','-'):old.append(line[1:])
                if line[0] in (' ','+'):new.append(line[1:])
            if len(old)!=int(match[2]) or len(new)!=int(match[4]) or lines[position:position+len(old)]!=old:raise ValueError('Exact class-binding patch context differs')
            out.extend(new);position+=len(old)
        out.extend(lines[position:]);changes.append((name,original,''.join(out).encode('utf8')))
    if not changes:raise ValueError('Empty class-binding patch')
    for name,before,after in changes:directory.atomic_replace(name,after,expected_sha256=sha(before))

def system_import(name):return name.lower() in SYSTEM or name.lower().startswith(('api-ms-win-','ext-ms-win-'))

def verify_tools(path,require_capture_support=True):
    """Read-only validation; retains no success assertion about SDK/GPU execution."""
    with sources() as (lock,raw),guard.locked_directory(path,owner=True) as directory:
        receipt_raw=directory.read_regular('receipt.json',max_bytes=1024*1024,owner=True);record=document(receipt_raw)
        kinds={'windows-fsr4-tools'} if require_capture_support else {'windows-fsr4-tools','windows-fsr4-translator-tools'}
        if record.get('schema')!=1 or record.get('status')!='passed' or record.get('kind') not in kinds or record.get('lock_sha256')!=sha(raw):raise ValueError('Native preparation tools require the current passed recipe receipt with local D3D support')
        files=record.get('tools')
        required=REQUIRED if require_capture_support else REQUIRED-SUPPORT
        if not isinstance(files,dict) or not required<=set(files) or len(files)>40:raise ValueError('Native preparation tool membership differs')
        partial=record.get('kind')=='windows-fsr4-translator-tools'
        missing=SUPPORT-{name.lower() for name in files}
        if record.get('missing_capture_support')!=sorted(missing) or (not partial and missing):raise ValueError('Native capture dependency declaration differs')
        for name,item in files.items():
            if not re.fullmatch(r'[A-Za-z0-9_.+-]+\.(exe|dll)',name):raise ValueError('Unsafe tool name')
            with directory.open_regular('bin/'+name,max_bytes=64*1024*1024,owner=True) as stream:
                data=stream.read()
                if item!={'bytes':len(data),'sha256':sha(data)}:raise ValueError('Preparation tool changed: '+name)
                image=Image(directory.path/'bin'/name)
                for dependency in image.imports():
                    if dependency.lower() not in {x.lower() for x in files} and not system_import(dependency) and not(partial and dependency.lower() in missing):raise ValueError('Unreceipted native tool dependency: '+dependency)
        return dict(record,output=str(directory.path),receipt_sha256=sha(receipt_raw))

def build(output,archives,support,cmake,ninja,compiler,jobs=4,cancel=lambda:None,translator_only=False,vulkan_prefix=None):
    if not translator_only and support is None:raise ValueError('Full capture tools require explicit local D3D support')
    with sources() as (lock,raw),fresh(output) as out,guard.locked_directory(archives) as archived,guard.locked_directory(support,owner=True) if support is not None else contextlib.nullcontext(None) as supplied,contextlib.ExitStack() as executables:
        with out.child('logs',create=True,owner=True),out.child('bin',create=True,owner=True) as binaries:
            commands=[];toolchain={}
            for name,path in (('compiler',compiler),('cmake',cmake),('ninja',ninja)):
                path=Path(guard.canonical_path(path));stream=executables.enter_context(guard.open_regular(path,max_bytes=512*1024*1024))
                toolchain[name]={'path':str(path),'sha256':hashlib.file_digest(stream,'sha256').hexdigest()};Image(path)
            def execute(stage,argv,cwd=ROOT,timeout=900):
                cancel();progress(stage,0,0,'Building native '+stage)
                environment={k:v for k,v in os.environ.items() if not k.upper().startswith('PYTHON')}
                environment.update(PYTHONNOUSERSITE='1',PYTHONDONTWRITEBYTECODE='1',ZIG_LOCAL_CACHE_DIR=str(out.path/'zig-local'),ZIG_GLOBAL_CACHE_DIR=str(out.path/'zig-global'))
                item=run(argv,cwd=cwd,log=out.path/'logs'/(stage+'.log'),environment=environment,timeout=timeout,checkpoint=cancel)
                commands.append(item)
                if item['exit']:raise RuntimeError(stage+' failed; see '+item['log'])
            locations={'dxil-spirv':'source/dxil-spirv','spirv-headers':'source/dxil-spirv/third_party/spirv-headers','SPIRV-Tools':'source/dxil-spirv/third_party/SPIRV-Tools','SPIRV-Cross':'source/dxil-spirv/third_party/SPIRV-Cross','dxbc-spirv':'source/dxil-spirv/subprojects/dxbc-spirv'}
            for item in lock['dependencies']:
                cancel();name=item['path']+'-'+item['commit']+'.tar.gz';data=archived.read_regular(name,max_bytes=16*1024*1024)
                if len(data)!=item['archive_bytes'] or sha(data)!=item['archive_sha256']:raise ValueError('Pinned source archive differs: '+name)
                with out.child(locations[item['path']],create=True,owner=True) as target:unpack(data,target)
            with out.child('source/dxil-spirv',owner=True) as source:patch_class_bindings(source,guard.read_regular(ROOT/'third_party/fsr4cap/dxil-spirv-class-bindings.patch',max_bytes=65536))
            provenance=None
            if not translator_only:
                provenance=document(supplied.read_regular('support.json',max_bytes=65536,owner=True))
                if provenance.get('schema')!=1 or provenance.get('kind')!='local-vkd3d-dxvk' or set(provenance.get('files',{}))!=SUPPORT or not isinstance(provenance.get('provenance'),str) or not 1<=len(provenance['provenance'])<=2000:raise ValueError('Local D3D support requires explicit support.json provenance and exact three-file inventory')
                for name,item in provenance['files'].items():
                    data=supplied.read_regular(name,max_bytes=64*1024*1024,owner=True)
                    if item!={'bytes':len(data),'sha256':sha(data)}:raise ValueError('Local D3D support content differs')
                    Image(supplied.path/name);binaries.write_new(name,data)
            cap=ROOT/'third_party/fsr4cap'
            execute('capture-tool',[compiler,'cc','-std=c11','-O1','-UNDEBUG','-march=x86_64_v2','-ffp-contract=off','-I'+str(cap/'ffx/api/include'),'-I'+str(cap/'ffx/upscalers/include'),cap/'fsr4cap.c',cap/'capture.c',cap/'rootsig.c','-o',binaries.path/'fsr4cap.exe','-ld3d12','-ldxguid','-static'])
            execute('driver-shim',[compiler,'cc','-std=c11','-O1','-shared',cap/'amdxc64.c','-o',binaries.path/'amdxc64.dll','-static'])
            prefix=Path(vulkan_prefix) if vulkan_prefix else Path(document(guard.read_regular(ROOT/'.deps/windows-deps.json',max_bytes=1024*1024))['prefix'])
            execute('gpu-validator',[compiler,'c++','-std=c++20','-O1','-UNDEBUG','-march=x86_64_v2','-ffp-contract=off','-fno-fast-math','-municode','-I'+str(prefix/'include'),ROOT/'tools/fsr4/validate_windows.cpp',prefix/'lib/libvulkan-1.dll.a','-ladvapi32','-static','-o',binaries.path/'fsr4-validate.exe'])
            source=out.path/'source/dxil-spirv';build_dir=out.path/'compile'
            # A cache -D is shadowed by Windows-Clang's normal module defaults.
            # This documented hook runs after those compiler/platform modules.
            rules=[]
            for language in ('C','CXX'):
                for key,value in [('CREATE','<CMAKE_AR> ar qc <TARGET> <LINK_FLAGS> <OBJECTS>'),('APPEND','<CMAKE_AR> ar q <TARGET> <LINK_FLAGS> <OBJECTS>'),('FINISH','<CMAKE_AR> ar s <TARGET>')]:
                    rules.append('set(CMAKE_'+language+'_ARCHIVE_'+key+' "'+value+'")')
            rule_data=('\n'.join(rules)+'\n').encode();out.write_new('archive-rules.cmake',rule_data)
            archive_rules=['-DCMAKE_AR='+str(compiler),'-DCMAKE_RANLIB='+str(compiler),'-DCMAKE_USER_MAKE_RULES_OVERRIDE='+str(out.path/'archive-rules.cmake')]
            execute('configure',[cmake,'-S',source,'-B',build_dir,'-G','Ninja','-DCMAKE_MAKE_PROGRAM='+str(ninja),'-DCMAKE_BUILD_TYPE=Release','-DCMAKE_C_COMPILER='+str(compiler),'-DCMAKE_C_COMPILER_ARG1=cc','-DCMAKE_CXX_COMPILER='+str(compiler),'-DCMAKE_CXX_COMPILER_ARG1=c++',*archive_rules,'-DCMAKE_C_FLAGS=-march=x86_64_v2 -ffp-contract=off','-DCMAKE_CXX_FLAGS=-march=x86_64_v2 -ffp-contract=off','-DCMAKE_EXE_LINKER_FLAGS=-static','-DBUILD_SHARED_LIBS=OFF','-DSPIRV_SKIP_TESTS=ON','-DSPIRV_SKIP_EXECUTABLES=OFF','-DSPIRV_WERROR=OFF','-DSPIRV_CROSS_ENABLE_TESTS=OFF','-DSPIRV_CROSS_ENABLE_CLI=OFF','-DCMAKE_DISABLE_FIND_PACKAGE_Git=ON','-DPython3_EXECUTABLE='+sys.executable,'-DPYTHON_EXECUTABLE='+sys.executable])
            execute('compile-tools',[cmake,'--build',build_dir,'--parallel',str(jobs),'--target','dxil-spirv','spirv-as','spirv-dis','spirv-val'],timeout=1800)
            for name,relative in [('dxil-spirv.exe','dxil-spirv.exe'),('libdxil-spirv-c-shared.dll','libdxil-spirv-c-shared.dll'),*[(n+'.exe','third_party/SPIRV-Tools/tools/'+n+'.exe') for n in ('spirv-as','spirv-dis','spirv-val')]]:
                binaries.write_new(name,guard.read_regular(build_dir/relative,max_bytes=64*1024*1024,owner=True))
            files={}
            # Zig may also emit .lib/.pdb diagnostics; only this authored exact
            # executable membership is staged/parsed/eligible for execution.
            for name in sorted(REQUIRED-SUPPORT if translator_only else REQUIRED):
                data=binaries.read_regular(name,max_bytes=64*1024*1024,owner=True);Image(binaries.path/name);files[name]={'bytes':len(data),'sha256':sha(data)}
            for name in files:
                for imported in Image(binaries.path/name).imports():
                    if imported.lower() not in {x.lower() for x in files} and not system_import(imported) and not(translator_only and imported.lower() in SUPPORT):raise ValueError('Native tool has an unreceipted dependency: '+imported)
            record=dict(schema=1,status='passed',kind='windows-fsr4-translator-tools' if translator_only else 'windows-fsr4-tools',lock_sha256=sha(raw),archive_rules_sha256=sha(rule_data),tools=files,commands=commands,toolchain=toolchain,support=provenance,missing_capture_support=sorted(SUPPORT if translator_only else []),scope='native source tool build; no SDK/model/GPU qualification')
            out.write_new('receipt.json',encoded(record))
        return verify_tools(out.path,require_capture_support=not translator_only)

def main(argv=None):
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--build',action='store_true');p.add_argument('--output',type=Path,required=True)
    for name in ('archives','support','cmake','ninja','vulkan-prefix'):p.add_argument('--'+name,type=Path)
    p.add_argument('--translator-only',action='store_true')
    p.add_argument('--compiler',type=Path);p.add_argument('--jobs',type=int,choices=range(1,9),default=4)
    a=p.parse_args(argv)
    try:
        if a.build and any(getattr(a,n) is None for n in ('archives','cmake','ninja','compiler')):raise ValueError('--build requires local --archives --cmake --ninja --compiler; full capture also requires --support; nothing is downloaded or installed')
        print(json.dumps(build(a.output,a.archives,a.support,a.cmake,a.ninja,a.compiler,a.jobs,translator_only=a.translator_only,vulkan_prefix=a.vulkan_prefix) if a.build else verify_tools(a.output,require_capture_support=not a.translator_only),indent=2));return 0
    except (OSError,ValueError,RuntimeError,KeyError) as e:p.exit(1,str(e)+'\n')
if __name__=='__main__':main()
