#!/usr/bin/env python3
"""[XCDE] LINUX-017: actual renderer-shim feature negotiation, mocked queries only; no GPU/NGX/game."""
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
import tempfile
import time
ROOT = Path(__file__).resolve().parent.parent
SOURCES = ('scripts/dlss_requirements_tests_linux.py','tests/native/test_dlss_requirements_linux.cpp',
    'runtime/nvn/vulkan/dlss.cpp','runtime/nvn/vulkan/dlss.h','runtime/nvn/vulkan/dlss_linux.h','runtime/nvn/vulkan/vk.h','runtime/settings.h')
CASES = ('supported','unsupported','query-fail','no-bda','existing','already-enabled','missing-f12','no-bda-null','query-fail-existing')
def digest(p): return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def main():
    ap = argparse.ArgumentParser(description=__doc__); ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--baseline-ref', help='optional local Git source: require supported-case rejection before fix')
    args = ap.parse_args()
    if platform.system() != 'Linux' or platform.machine() != 'x86_64': ap.error('native Linux x86-64 required')
    out = args.output.absolute()
    if out != out.resolve() or not out.is_relative_to(ROOT / 'runs') or out.exists(): ap.error('fresh ordinary runs/ directory required')
    resource.setrlimit(resource.RLIMIT_CORE, (0,0)); os.umask(0o077); out.mkdir(parents=True)
    env = {'PATH':'/usr/bin:/bin','LANG':'C.UTF-8','LC_ALL':'C.UTF-8','TMPDIR':str(out)}
    cc = ROOT / '.deps/bin/clang-cxx'; sysroot = Path((ROOT / '.deps/graphics/active-sysroot.txt').read_text().strip())
    receipt = {'scope':__doc__,'status':'running','host':list(platform.uname()),'cwd':str(ROOT),'environment':env,
        'compiler_sha256':digest(cc),'sysroot':str(sysroot),'sources_before':{p:digest(ROOT/p) for p in SOURCES},'commands':[]}
    def save(): (out/'receipt.json').write_text(json.dumps(receipt,indent=2)+'\n')
    def run(name, argv, negative=False):
        argv=list(map(str,argv)); row={'name':name,'argv':argv,'cwd':str(ROOT),'timeout_seconds':60,'expected_failure':negative}
        receipt['commands'].append(row); save(); t=time.monotonic()
        p=subprocess.Popen(argv,cwd=ROOT,env=env,stdin=subprocess.DEVNULL,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,start_new_session=True)
        row['pid']=p.pid; save()
        try: data,_=p.communicate(timeout=60)
        except (subprocess.TimeoutExpired,KeyboardInterrupt):
            os.killpg(p.pid,signal.SIGKILL); data,_=p.communicate(timeout=5); row['interrupted']=True
        row.update(exit=p.returncode,seconds=time.monotonic()-t); (out/(name+'.log')).write_bytes(data); save()
        if row.get('interrupted') or (negative and p.returncode==0) or (not negative and p.returncode):
            raise RuntimeError(f'{name}: exit {p.returncode}: {data.decode(errors="replace")}')
        return data.decode()
    try:
        flags=shlex.split(run('pkg-cflags',[ROOT/'.deps/bin/pkg-config-graphics','--cflags','sdl3','vulkan']))
        flags += ['-std=c++17','-O1','-UNDEBUG','-march=x86_64_v2','-ffp-contract=off','-fno-strict-aliasing',
            '-ffunction-sections','-fdata-sections','-DXCDE_LINUX_DLSS=1','-I'+str(ROOT/'runtime'),'-I'+str(ROOT/'runtime/nvn/vulkan')]
        def compile_test(name,extra=()):
            exe=out/name; run('compile-'+name,[cc,*flags,*extra,ROOT/SOURCES[1],'-Wl,--gc-sections','-o',exe]); return exe
        if args.baseline_ref:
            baseline=out/'baseline-dlss.cpp'; baseline.write_text(run('baseline-source',['git','show',args.baseline_ref+':Monado_Linux/runtime/nvn/vulkan/dlss.cpp']))
            receipt['baseline_sha256']=digest(baseline)
            exe=compile_test('baseline-test',['-DDLSS_SHIM_SOURCE="'+str(baseline)+'"'])
            log=run('baseline-supported-negative',[exe,'supported'],negative=True)
            assert 'Assertion' in log and 'f12' in log
        exe=compile_test('requirements-test'); receipt['binary_sha256']=digest(exe)
        for case in CASES: print(run(case,[exe,case]),end='')
        receipt['sources_after']={p:digest(ROOT/p) for p in SOURCES}
        if receipt['sources_after']!=receipt['sources_before']: raise RuntimeError('sources changed during tests')
        receipt['status']='passed'
    except Exception as e: receipt.update(status='failed',error=str(e)); raise
    finally: save(); print(out)
if __name__=='__main__': main()
