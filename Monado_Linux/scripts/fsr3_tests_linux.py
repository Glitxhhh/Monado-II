#!/usr/bin/env python3
"""[XCDE] LINUX-103: bounded native FSR3 build/contracts and optional offscreen GPU fixture."""
import argparse
import json
import os
import resource
from pathlib import Path
import shlex
import subprocess
import time
import setup_linux_fsr3 as setup

ROOT=setup.ROOT
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True)
    p.add_argument('--real-device',action='store_true');p.add_argument('--framegen',action='store_true');a=p.parse_args();out=a.output.absolute()
    if a.framegen and not a.real_device:p.error('--framegen requires --real-device')
    if out!=out.resolve() or out.exists() or not out.is_relative_to(ROOT/'runs'):p.error('fresh ordinary output beneath runs required')
    resource.setrlimit(resource.RLIMIT_CORE,(0,0));os.umask(0o077);out.mkdir(parents=True)
    dep=setup.verify_existing();sysroot=Path((ROOT/'.deps/graphics/active-sysroot.txt').read_text().strip());lib=sysroot/'usr/lib/x86_64-linux-gnu'
    env={'PATH':'/usr/bin:/bin','LANG':'C.UTF-8','LC_ALL':'C.UTF-8','TMPDIR':str(out),'LD_LIBRARY_PATH':str(lib)}
    sources=('scripts/fsr3_tests_linux.py','scripts/setup_linux_fsr3.py','scripts/setup_linux_fsr.py',
        'tests/native/test_fsr3_linux.cpp','tests/native/test_fsr3_memory.cpp','tests/native/test_framegen_linux.cpp',
        'tests/native/test_temporal_shaders_linux.c','tests/native/temporal_gpu_encode.h','runtime/nvn/vulkan/shaders.c',
        'runtime/nvn/vulkan/framegen_linux.cpp','runtime/nvn/vulkan/framegen_linux.h')
    record={'scope':'authored native FSR3 fixture; no game/window/human profile','status':'running','dependency':dep,'commands':[],
        'sources_before':{n:setup.common.digest(ROOT/n) for n in sources}}
    def save():(out/'receipt.json').write_text(json.dumps(record,indent=2)+'\n')
    def run(name,args,seconds=90):
        start=time.monotonic();r=subprocess.run(list(map(str,args)),cwd=ROOT,env=env,stdin=subprocess.DEVNULL,stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=seconds)
        (out/(name+'.stdout.log')).write_bytes(r.stdout);(out/(name+'.stderr.log')).write_bytes(r.stderr)
        record['commands'].append({'name':name,'argv':list(map(str,args)),'cwd':str(ROOT),'exit':r.returncode,'seconds':time.monotonic()-start});save()
        if r.returncode:raise RuntimeError(name+': '+r.stderr.decode(errors='replace')[-4000:])
        return r.stdout.decode()
    try:
        flags=shlex.split(run('pkg-flags',[ROOT/'.deps/bin/pkg-config-graphics','--cflags','--libs','vulkan']))
        cc=ROOT/'.deps/bin/clang-cc';cxx=ROOT/'.deps/bin/clang-cxx'
        run('compile-guard',[cc,'-O1','-UNDEBUG','-ffunction-sections','-fdata-sections','-c',ROOT/'runtime/platform/linux_state_guard.c','-o',out/'guard.o'])
        component=Path(dep['receipt_path']).parent;source=component/'source'
        sdk_flags=['-std=c++17','-O1','-UNDEBUG','-fno-strict-aliasing','-ffp-contract=off','-ffunction-sections',
            '-fdata-sections','-fms-extensions','-DFFX_BACKEND_VK=1','-DFFX_GCC=1','-include',component/'compat.h',
            '-I'+str(source/'sdk/include'),'-I'+str(source/'ffx-api/include'),'-I'+str(source/'sdk/src/backends/shared'),
            '-I'+str(source/'sdk/src/shared'),'-I'+str(source/'sdk/src/components')]
        memory=out/'memory-fixture'
        run('compile-memory',[cxx,*sdk_flags,'-DMONADO_SDK_VK_SOURCE="'+str(source/'sdk/src/backends/vk/ffx_vk.cpp')+'"',
            ROOT/'tests/native/test_fsr3_memory.cpp',*flags,'-Wl,--gc-sections','-o',memory])
        print(run('memory',[memory]),end='')
        shader_flags=shlex.split(run('shader-flags',[ROOT/'.deps/bin/pkg-config-graphics','--cflags','--libs','shaderc','sdl3']))
        shader_fixture=out/'temporal-shader-compiler'
        run('compile-shaders',[cc,'-O1','-UNDEBUG','-ffunction-sections','-fdata-sections',ROOT/'tests/native/test_temporal_shaders_linux.c',
            *shader_flags,'-Wl,--gc-sections','-Wl,-rpath,'+str(lib),'-o',shader_fixture])
        shaders=out/'shaders';shaders.mkdir();run('shaders',[shader_fixture,shaders])
        fg=setup.common.verify_existing()
        exe=out/'fsr3-fixture' 
        run('compile',[cxx,'-std=c++17','-O1','-UNDEBUG','-ffp-contract=off','-fno-strict-aliasing','-ffunction-sections','-fdata-sections',
            '-DFG_LINUX_PROVIDER','-I'+dep['header_dir'],ROOT/'tests/native/test_fsr3_linux.cpp',
            ROOT/'runtime/nvn/vulkan/framegen_linux.cpp',out/'guard.o',*flags,'-Wl,--gc-sections','-Wl,-rpath,'+str(lib),'-ldl','-pthread','-o',exe])
        if a.real_device:
            state=out/'state';state.mkdir(mode=0o700)
            print(run('gpu',[exe,state,dep['library_path'],shaders,*([fg['library_path']] if a.framegen else [])]),end='')
        record['sources_after']={n:setup.common.digest(ROOT/n) for n in sources}
        if record['sources_before']!=record['sources_after']:raise RuntimeError('FSR3 fixture sources changed during run')
        record.update(status='passed',real_device=a.real_device,frame_generation=a.framegen,binary_sha256=setup.common.digest(exe))
    except Exception as e:record.update(status='failed',error=str(e));raise
    finally:save();print(str(out/'receipt.json'))
if __name__=='__main__':main()
