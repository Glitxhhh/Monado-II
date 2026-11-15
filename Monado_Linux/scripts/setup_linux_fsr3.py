#!/usr/bin/env python3
"""[XCDE] LINUX-103: source-built native Vulkan FSR3.1 upscaler.

Uses the already hash-locked official MIT SDK source and audited shaderc/Reflect
compiler from setup_linux_fsr. No DLL extraction, game data or host installation.
This component registers only the official upscaler, leaving FG's proven module
and ordinary Linux presentation unchanged. Native LDR/low-res/unjittered/normal
depth shaders are generated locally; unsupported input conventions fail.
"""
import argparse
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import threading
import time

import setup_linux_fsr as common

ROOT=common.ROOT
NAME='libmonado-fsr3.so'
PASSES=('prepare_inputs','luma_pyramid','shading_change_pyramid','shading_change',
        'prepare_reactivity','luma_instability','accumulate','accumulate','rcas','debug_view','autogen_reactive')
REGISTRY=common.REGISTRY.replace('ffx_provider_framegeneration.h','ffx_provider_fsr3upscale.h').replace(
    'ffxProvider_FrameGeneration','ffxProvider_FSR3Upscale').replace('monado_ffx_abi_version','monado_fsr3_abi_version').replace(
    'monado_ffx_sdk_commit','monado_fsr3_sdk_commit')


def emit_blob(lines,tag,spv,bindings):
    """Use actual SPIRV-Reflect descriptors, preserving their set/count/type."""
    kinds=('constantBuffer','srvTexture','uavTexture','srvBuffer','uavBuffer','sampler','rtAccelerationStructure')
    groups=[[b for b in bindings if b['kind']==k] for k in kinds]
    data=spv.read_bytes();lines.append('alignas(4) static const uint8_t '+tag+'[]={'+','.join(map(str,data))+'};')
    arrays=[]
    for k,group in zip(kinds,groups):
        if not group:arrays+=['nullptr']*4;continue
        p=tag+'_'+k
        lines.append('static const char* '+p+'_name[]={'+','.join(json.dumps(x['name']) for x in group)+'};')
        arrays.append(p+'_name')
        for field in ('binding','count','set'):
            lines.append('static const uint32_t '+p+'_'+field+'[]={'+','.join(str(x[field]) for x in group)+'};')
            arrays.append(p+'_'+field)
    lines.append('static const FfxShaderBlob b'+tag+'={'+','.join([tag,'sizeof('+tag+')',*[str(len(g)) for g in groups],*arrays])+'};')

def build(output,jobs=4):
    lock=json.loads(common.LOCK.read_text());original=common.verify_source(lock)
    out=Path(output).absolute()
    if out!=out.resolve() or out.exists() or not out.is_relative_to(ROOT/'.deps/fsr3/build'):
        raise ValueError('fresh ordinary output beneath .deps/fsr3/build required')
    out.mkdir(parents=True);[(out/n).mkdir() for n in ('obj','logs','shaders')]
    sysroot=Path((ROOT/'.deps/graphics/active-sysroot.txt').read_text().strip());lib=sysroot/'usr/lib/x86_64-linux-gnu'
    cc=ROOT/'.deps/bin/clang-cc';cxx=ROOT/'.deps/bin/clang-cxx'
    env={'PATH':'/usr/bin:/bin','LANG':'C.UTF-8','LC_ALL':'C.UTF-8','LD_LIBRARY_PATH':str(lib),'TMPDIR':str(out)}
    receipt={'schema':1,'status':'running','scope':'native FSR3.1.4 upscaling only, authored SDK shaders; no game data',
             'sdk_commit':lock['commit'],'lock_sha256':common.digest(common.LOCK),'setup_sha256':common.digest(__file__),
             'common_setup_sha256':common.digest(common.__file__),'source':str(original),'output':str(out),'commands':[],
             'dependencies':{'sysroot':str(sysroot),'sysroot_marker_sha256':common.digest(sysroot/'.monado-deps.json')}}
    mutex=threading.RLock()
    def save():
        with mutex:(out/'receipt.json').write_text(json.dumps(receipt,indent=2)+'\n')
    def run(name,args):
        t=time.monotonic();r=subprocess.run(list(map(str,args)),cwd=ROOT,env=env,stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=180)
        (out/'logs'/(name+'.stdout.log')).write_bytes(r.stdout);(out/'logs'/(name+'.stderr.log')).write_bytes(r.stderr)
        with mutex:
            receipt['commands'].append({'name':name,'argv':list(map(str,args)),'cwd':str(ROOT),'exit':r.returncode,'seconds':time.monotonic()-t});save()
        if r.returncode:raise RuntimeError(name+' failed: '+r.stderr.decode(errors='replace')[:2400])
    try:
        save();src=out/'source';receipt['source_patches']={}
        for name,item in lock['files'].items():
            data=(original/name).read_bytes();changed=data
            if Path(name).suffix in ('.h','.hpp','.cpp','.c'):
                changed=changed.replace(b'FidelityFx/',b'FidelityFX/').replace(
                    b'#define FFX_API_ENTRY __declspec(dllexport)',b'#define FFX_API_ENTRY __attribute__((visibility("default")))')
                if name.endswith('ffx_fsr3upscaler.h'):
                    changed=changed.replace(b'FFX_FSR3UPSCALER_CONTEXT_SIZE (FFX_SDK_DEFAULT_CONTEXT_SIZE)',
                        b'FFX_FSR3UPSCALER_CONTEXT_SIZE (2 * FFX_SDK_DEFAULT_CONTEXT_SIZE)')
                if name=='sdk/src/backends/vk/ffx_vk.cpp':
                    changed=common.patch_vulkan_arena(changed).replace(b'return FFX_ERROR_BACKEND_API_ERROR;', b'return (fprintf(stderr,"[fsr3] Vulkan backend failure line %d\\n", __LINE__), FFX_ERROR_BACKEND_API_ERROR);')
                if name=='ffx-api/src/ffx_provider.h':
                    changed=changed.replace(b'if (FFX_OK != (_expr)) return FFX_API_RETURN_ERROR_RUNTIME_ERROR',
                        b'if (auto _code = (_expr); _code != FFX_OK) { fprintf(stderr,"[fsr3] %s failed: 0x%x\\n", #_expr, unsigned(_code)); return FFX_API_RETURN_ERROR_RUNTIME_ERROR; }')
                if name=='ffx-api/src/backends.cpp':
                    changed=changed.replace(b'void* scratchBuffer = alloc.alloc(scratchBufferSize);',
                        b'if (!scratchBufferSize) return FFX_API_RETURN_ERROR_MEMORY;\n            void* scratchBuffer = alloc.alloc(scratchBufferSize);\n            if (!scratchBuffer) return FFX_API_RETURN_ERROR_MEMORY;')

            target=src/name;target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes(changed)
            if changed!=data:receipt['source_patches'][name]={'before_sha256':item['sha256'],'after_sha256':common.digest(target)}
        # The SDK's unqualified abs(float) must resolve to float, not abs(int):
        # otherwise the Lanczos LUT silently loses stationary image detail.
        (out/'compat.h').write_text(common.COMPAT+'\n#include <cmath>\nusing std::abs;\n')
        (out/'registry.cpp').write_text(REGISTRY.replace('@COMMIT@',lock['commit']))
        (out/'compiler.cpp').write_text(common.COMPILER)
        reflect=src/'sdk/tools/ffx_shader_compiler/libs/SPIRV-Reflect';inc=src/'sdk/include';api=src/'ffx-api/include';shared=src/'sdk/src/backends/shared'
        run('reflect',[cc,'-O1','-fPIC','-I'+str(reflect),'-c',reflect/'spirv_reflect.c','-o',out/'obj/reflect.o'])
        run('shader-compiler',[cxx,'-std=c++17','-O1','-I'+str(sysroot/'usr/include'),'-I'+str(reflect),
            out/'compiler.cpp',out/'obj/reflect.o',lib/'libshaderc.so','-Wl,-rpath,'+str(lib),'-o',out/'shader-compiler'])
        # Native subgroup size and full precision preserve portability. We compile
        # both analytical and LUT history filters; no opaque vendor SPIR-V is used.
        specs=[]
        for p,name in enumerate(PASSES):
            for lut in ((0,1) if p in (6,7) else (0,)):
                tag='p'+str(p)+'_'+str(lut);spv=out/'shaders'/(tag+'.spv');meta=spv.with_suffix('.json')
                defs=['FFX_GPU=1','FFX_GLSL=1','FFX_HALF=0','FFX_FSR3UPSCALER_OPTION_HDR_COLOR_INPUT=0',
                    'FFX_FSR3UPSCALER_OPTION_LOW_RESOLUTION_MOTION_VECTORS=1','FFX_FSR3UPSCALER_OPTION_JITTERED_MOTION_VECTORS=0',
                    'FFX_FSR3UPSCALER_OPTION_INVERTED_DEPTH=0','FFX_FSR3UPSCALER_OPTION_REPROJECT_USE_LANCZOS_TYPE='+str(lut),
                    'FFX_FSR3UPSCALER_OPTION_APPLY_SHARPENING='+str(int(p==7))]
                shader=src/('sdk/src/backends/vk/shaders/fsr3upscaler/ffx_fsr3upscaler_'+name+'_pass.glsl')
                specs.append((p,lut,tag,spv,meta,shader,defs))
        def compile_shader(spec):
            p,lut,tag,spv,meta,shader,defs=spec
            run('shader-'+tag,[out/'shader-compiler',inc/'FidelityFX/gpu',shader,spv,meta,*defs]);common.strip_source_debug(spv)
        with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:list(pool.map(compile_shader,specs))
        lines=['// Locally generated from the hash-locked MIT AMD SDK; no game-derived shader data.',
               '#include "ffx_shader_blobs.h"','#include <cstring>','#include <cstdio>']
        for p,lut,tag,spv,meta,*_ in specs:emit_blob(lines,tag,spv,json.loads(meta.read_text()))
        table=','.join('&bp'+str(p)+'_0' for p in range(len(PASSES)))
        luttable=','.join('&bp'+str(p)+('_1' if p in (6,7) else '_0') for p in range(len(PASSES)))
        lines+=['extern "C" FfxErrorCode ffxGetPermutationBlobByIndex(FfxEffect effect,FfxPass pass,FfxBindStage,uint32_t options,FfxShaderBlob*out){',
            'if(effect!=FFX_EFFECT_FSR3UPSCALER||!out||pass>=11||(options&30)!=4){fprintf(stderr,\"[fsr3] unsupported shader request effect=%u pass=%u flags=%u\\n\",unsigned(effect),unsigned(pass),options);return FFX_ERROR_INVALID_ARGUMENT;}',
            'const FfxShaderBlob*normal[]={'+table+'};const FfxShaderBlob*lut[]={'+luttable+'};',
            'std::memcpy(out,(options&1)?lut[pass]:normal[pass],sizeof(*out));return FFX_OK;}',
            'extern "C" FfxErrorCode ffxIsWave64(FfxEffect,uint32_t,bool&wave){wave=false;return FFX_OK;}']
        (out/'blobs.cpp').write_text('\n'.join(lines)+'\n')
        flags=['-std=c++17','-O1','-UNDEBUG','-fPIC','-fno-strict-aliasing','-ffp-contract=off','-ffunction-sections',
            '-fdata-sections','-fvisibility=hidden','-pthread','-fms-extensions','-DFFX_BACKEND_VK=1','-DFFX_GCC=1',
            '-include',out/'compat.h','-I'+str(inc),'-I'+str(api),'-I'+str(shared),'-I'+str(src/'sdk/src/shared'),
            '-I'+str(src/'sdk/src/components'),'-I'+str(src/'ffx-api/src'),'-I'+str(sysroot/'usr/include')]
        sources=[out/'registry.cpp',out/'blobs.cpp',src/'ffx-api/src/ffx_api.cpp',src/'ffx-api/src/backends.cpp',
            src/'ffx-api/src/ffx_provider_fsr3upscale.cpp',src/'ffx-api/src/validation.cpp',src/'sdk/src/shared/ffx_assert.cpp',
            src/'sdk/src/shared/ffx_message.cpp',src/'sdk/src/shared/ffx_breadcrumbs_list.cpp',src/'sdk/src/shared/ffx_object_management.cpp',
            src/'sdk/src/components/fsr3upscaler/ffx_fsr3upscaler.cpp',src/'sdk/src/backends/vk/ffx_vk.cpp']
        objects=[]
        for i,s in enumerate(sources):
            obj=out/'obj'/('sdk-'+str(i)+'.o');run('compile-'+str(i),[cxx,*flags,'-c',s,'-o',obj]);objects.append(obj)
        expected={'ffxCreateContext','ffxDestroyContext','ffxConfigure','ffxQuery','ffxDispatch','monado_fsr3_abi_version','monado_fsr3_sdk_commit'}
        (out/'exports.map').write_text('{ global: '+'; '.join(sorted(expected))+'; local: *; };\n')
        run('link',[cxx,'-shared','-pthread',*objects,'-Wl,--gc-sections','-Wl,-z,defs','-Wl,--version-script,'+str(out/'exports.map'),lib/'libvulkan.so','-o',out/NAME])
        run('exports',['nm','-D','--defined-only',out/NAME]);run('dynamic',['readelf','-d',out/NAME])
        symbols={line.split()[-1] for line in (out/'logs/exports.stdout.log').read_text().splitlines() if line.split()}
        if symbols!=expected:raise ValueError('unexpected FSR3 component exports: '+str(symbols))
        common.verify_source(lock)
        shutil.copyfile(original/'LICENSE.txt',out/'AMD-FidelityFX-LICENSE.txt')
        receipt.update(status='passed',shader_count=len(specs),library={'path':str(out/NAME),'bytes':(out/NAME).stat().st_size,
            'sha256':common.digest(out/NAME)},license_sha256=common.digest(out/'AMD-FidelityFX-LICENSE.txt'))
        marker=ROOT/'.deps/fsr3/active-build.txt'
        if marker.is_symlink():raise ValueError('FSR3 marker must be ordinary')
        temp=marker.with_name('.active-build.'+str(os.getpid()));temp.write_text(str(out)+'\n');os.replace(temp,marker)
    except Exception as e:receipt.update(status='failed',error=str(e));raise
    finally:save()
    return receipt

def verify_existing():
    lock=json.loads(common.LOCK.read_text());common.verify_source(lock);marker=ROOT/'.deps/fsr3/active-build.txt'
    if marker.is_symlink() or not marker.is_file():raise ValueError('native FSR3 component not built')
    out=Path(marker.read_text().strip())
    if not out.is_absolute() or out!=out.resolve() or not out.is_relative_to(ROOT/'.deps/fsr3/build'):raise ValueError('invalid FSR3 active marker')
    receipt_path=out/'receipt.json'
    if receipt_path.is_symlink():raise ValueError('FSR3 receipt must be ordinary')
    r=json.loads(receipt_path.read_text())
    if r.get('status')!='passed' or r.get('sdk_commit')!=lock['commit'] or r.get('lock_sha256')!=common.digest(common.LOCK) or r.get('setup_sha256')!=common.digest(__file__) or r.get('common_setup_sha256')!=common.digest(common.__file__):raise ValueError('FSR3 dependency recipe/lock changed')
    library=out/NAME;license=out/'AMD-FidelityFX-LICENSE.txt'
    for p,size,sha in ((library,r['library']['bytes'],r['library']['sha256']),(license,license.stat().st_size,r['license_sha256'])):
        if p.is_symlink() or not p.is_file() or p.stat().st_size!=size or common.digest(p)!=sha:raise ValueError('FSR3 component identity changed')
    sysroot=Path(r['dependencies']['sysroot'])
    if common.digest(sysroot/'.monado-deps.json')!=r['dependencies']['sysroot_marker_sha256']:raise ValueError('FSR3 graphics dependency identity changed')
    return {'library_path':str(library),'header_dir':str(out/'source/ffx-api/include'),'receipt_path':str(receipt_path),
        'library_sha256':r['library']['sha256'],'source_commit':lock['commit'],'license_path':str(license),
        'license_sha256':r['license_sha256'],'abi_version':1,'sdk_version':'1.1.4','provider_version':'3.1.4'}

def main():
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--build',action='store_true');ap.add_argument('--output',type=Path)
    ap.add_argument('--jobs',type=int,choices=range(1,9),default=4);a=ap.parse_args()
    try:
        if a.build:
            if not a.output:raise ValueError('--build requires --output')
            print(json.dumps(build(a.output,a.jobs),indent=2))
        else:print(json.dumps(verify_existing(),indent=2))
    except (OSError,ValueError,RuntimeError,subprocess.SubprocessError) as e:ap.exit(1,str(e)+'\n')
if __name__=='__main__':main()
