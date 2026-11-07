#!/usr/bin/env python3
"""[XCDE] LINUX-018: actual backend on authored containers, mocked GPU/cache boundary; no game/input/UI/GPU."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import resource
import shlex
import signal
import subprocess
import time
ROOT=Path(__file__).resolve().parent.parent
SOURCES=('scripts/precompile_backend_tests_linux.py','tests/native/test_precompile_linux.c',
    'tests/native/test_precompile_path_linux.c','tests/native/test_precompile_bounds_linux.c','runtime/nvn/vulkan/precompile.c','runtime/nvn/vulkan/precompile_status.h',
    'runtime/nvn/vulkan/shaders.h','runtime/nvn/vulkan/vk.h')
def digest(p): return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def parse(path):
    lines=path.read_text().splitlines(); assert lines.pop(0)=='monado_precompile_work 1'
    result=dict(line.split(' ',1) for line in lines)
    for k in tuple(result):
        if k not in ('session','identity','phase','state','gpu_cache_uuid'): result[k]=int(result[k])
    return result

def main():
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--output',type=Path,required=True)
    ap.add_argument('--sanitize',action='store_true',help='also run actual Clang ASan parser checks with installed libasan and a live negative control')
    args=ap.parse_args();out=args.output.absolute()
    if platform.system()!='Linux' or platform.machine()!='x86_64': ap.error('Linux x86-64 required')
    if out!=out.resolve() or out.exists() or not out.is_relative_to(ROOT/'runs'): ap.error('fresh ordinary runs/ output required')
    out.mkdir(parents=True);os.umask(0o077);resource.setrlimit(resource.RLIMIT_CORE,(0,0))
    env={'PATH':'/usr/bin:/bin','LANG':'C.UTF-8','LC_ALL':'C.UTF-8','TMPDIR':str(out)}
    cc=ROOT/'.deps/bin/clang-cc';sysroot=Path((ROOT/'.deps/graphics/active-sysroot.txt').read_text().strip());lib=sysroot/'usr/lib/x86_64-linux-gnu'
    receipt={'scope':__doc__,'status':'running','host':list(platform.uname()),'compiler_sha256':digest(cc),
        'sources_before':{p:digest(ROOT/p) for p in SOURCES},'commands':[],'cases':[]}
    def save(): (out/'receipt.json').write_text(json.dumps(receipt,indent=2)+'\n')
    def run(name,argv,cwd=ROOT,expected=0,environment=None):
        row={'name':name,'argv':list(map(str,argv)),'cwd':str(cwd),'expected_exit':expected,'timeout_seconds':30}
        receipt['commands'].append(row);save();t=time.monotonic()
        child=subprocess.Popen(row['argv'],cwd=cwd,env=environment or env,stdin=subprocess.DEVNULL,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,start_new_session=True)
        row['pid']=child.pid;save()
        try: data,_=child.communicate(timeout=30)
        except (subprocess.TimeoutExpired,KeyboardInterrupt):
            os.killpg(child.pid,signal.SIGKILL);data,_=child.communicate(timeout=5);row['interrupted']=True
        row.update(exit=child.returncode,seconds=time.monotonic()-t);(out/(name+'.log')).write_bytes(data);save()
        if row.get('interrupted') or child.returncode!=expected: raise RuntimeError(name+': '+data.decode(errors='replace'))
        return data.decode()
    try:
        pkg=shlex.split(run('pkg-cflags',[ROOT/'.deps/bin/pkg-config-graphics','--cflags','sdl3','vulkan']))
        flags=['-O1','-UNDEBUG','-march=x86_64_v2','-ffp-contract=off','-fno-strict-aliasing','-pthread',
            '-ffunction-sections','-fdata-sections','-I'+str(ROOT/'runtime'),*pkg]
        def compile(name,source,compiler=cc,extra=()):
            exe=out/name;run('compile-'+name,[compiler,*flags,*extra,ROOT/source,lib/'libz.so','-Wl,--gc-sections','-Wl,-rpath,'+str(lib),'-o',exe]);return exe
        exe=compile('backend-test',SOURCES[1]);receipt['binary_sha256']=digest(exe)
        guard=compile('path-test',SOURCES[2]);state=out/'path-guard';state.mkdir();run('path-guard',[guard],state);assert not (state/'cache').exists()
        for case in ('parser','allocator-fill','incidental-magic','unsupported-version','recognized-bad-sections','constants-fallback','constants-short','blob-bounds','constants-overflow','metadata-overflow'):
            state=out/case;state.mkdir();run(case,[exe,case],state);assert not (state/'cache').exists()
        cases={'success':0,'compressed':0,'no-seed':0,'unsupported':0,'stale-cancel':0,'extra-failure':2,'seed-failure':2,
            'variant-failure':2,'cache-failure':2,'symlink':2,'thread-fail':1,'invalid-identity':1,'no-programs':1,'cancel':130,'early-cancel':130,'signal-cancel':130,'crash':3}
        for case,code in cases.items():
            state=out/case;state.mkdir();log=run(case,[exe,case],state,code)
            work=state/'cache/precompile_work.txt'
            if case=='invalid-identity': assert not (state/'cache').exists();continue
            result=parse(work);receipt['cases'].append({'case':case,'result':result});save()
            assert result['session']=='b'*32 and result['identity']=='a'*64
            assert not (state/'cache/precompile_done.txt').exists() and not (state/'cache/precompile_skip.txt').exists() and not (state/'cache/precompile_inflight.bin').exists()
            if code==0:
                assert result['state']=='complete' and result['phase']=='done' and result['failed']==result['skipped']==result['errors']==0
                for kind in ('files','seed','variants'): assert result[kind+'_done']==result[kind+'_total']
                assert result['programs_done']==result['programs']==1
            elif code==2: assert result['state']=='partial' and result['failed']+result['errors']>0
            elif code==130: assert result['state']=='cancelled'
            elif code==1: assert result['state']=='failed'
            else: assert result['state']=='running'
            if case in ('success','compressed','no-seed','unsupported','stale-cancel'): assert result['built']==result['attempted']==5
            if case=='no-seed': assert result['seed_total']==result['variants_total']==0
            if case=='unsupported': assert result['unsupported']==1
            if case=='extra-failure': assert result['attempted']==5 and result['failed']==2
            if case in ('seed-failure','variant-failure'): assert result['attempted']==7 and result['failed']==1 and result['seed_done']==result['variants_done']==1
            if case=='crash':
                run('rerun-after-crash',[exe,'success'],state);again=parse(work);assert again['state']=='complete' and again['attempted']==5
        if args.sanitize:
            asan=Path(run('find-libasan',['/usr/bin/gcc','-print-file-name=libasan.so']).strip())
            assert asan.is_file();receipt['libasan_sha256']=digest(asan)
            def asan_compile(name,source,extra=()):
                obj=out/(name+'.o');binary=out/name
                run('compile-'+name,[cc,*flags,'-Xclang','-fsanitize=address','-fno-omit-frame-pointer',*extra,'-c',source,'-o',obj])
                undefined=run('asan-symbols-'+name,['nm','-u',obj]);assert '__asan_init' in undefined
                run('link-'+name,[cc,obj,asan,lib/'libz.so','-pthread','-Wl,--gc-sections','-Wl,-rpath,'+str(lib),'-o',binary]);return binary
            control=out/'address-control.c'
            control.write_text('#include <stdlib.h>\nint main(int n,char **v){volatile char *p=malloc(4);volatile int i=n+7;p[i]=1;free((void*)p);return 0;}\n')
            instrumented=asan_compile('address-control',control)
            asanenv={**env,'ASAN_OPTIONS':'detect_leaks=0:abort_on_error=1'}
            log=run('address-control-negative',[instrumented],expected=-signal.SIGABRT,environment=asanenv)
            assert 'AddressSanitizer: heap-buffer-overflow' in log
            sanitized=asan_compile('backend-sanitized',ROOT/SOURCES[1])
            for case in ('parser','allocator-fill','incidental-magic','unsupported-version','recognized-bad-sections','constants-fallback','constants-short','blob-bounds','constants-overflow','metadata-overflow','fuzz'):
                state=out/('asan-'+case);state.mkdir();run('asan-'+case,[sanitized,case],state,environment=asanenv)
            baseline=out/'precompile-before.c';baseline.write_bytes((ROOT/'runs/20261105-linux-precompile/precompile-before.c').read_bytes())
            receipt['parser_baseline_sha256']=digest(baseline)
            bad=asan_compile('bounds-before',ROOT/'tests/native/test_precompile_bounds_linux.c',
                ['-I'+str(ROOT/'runtime/nvn/vulkan'),'-DPRECOMPILE_SOURCE="'+str(baseline)+'"'])
            log=run('bounds-before-negative',[bad],expected=-signal.SIGABRT,environment=asanenv)
            assert 'AddressSanitizer' in log and 'spch_inputs' in log
            fixed=asan_compile('bounds-after',ROOT/'tests/native/test_precompile_bounds_linux.c')
            run('bounds-after',[fixed],environment=asanenv)
        receipt['sources_after']={p:digest(ROOT/p) for p in SOURCES}
        if receipt['sources_before']!=receipt['sources_after']: raise RuntimeError('sources changed during tests')
        receipt['status']='passed';print('backend parser/orchestration/cancel/cache/result cases passed')
    except Exception as e: receipt.update(status='failed',error=str(e));raise
    finally: save();print(out)
if __name__=='__main__': main()
