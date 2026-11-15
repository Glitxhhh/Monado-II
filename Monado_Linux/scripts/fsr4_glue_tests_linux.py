#!/usr/bin/env python3
"""LINUX-103: FSR4 production glue, capability and fallback contracts; no game/GPU required."""
from pathlib import Path
import argparse,hashlib,json,os,subprocess,time
import renderer_tests_linux as renderer
ROOT=Path(__file__).resolve().parents[1]
def main():
 p=argparse.ArgumentParser();p.add_argument('--output',type=Path,required=True);p.add_argument('--ubsan',action='store_true');p.add_argument('--real-device',action='store_true');a=p.parse_args();out=a.output.absolute()
 if out.exists() or out!=out.resolve() or not out.is_relative_to(ROOT/'runs'):p.error('fresh ordinary output under port runs required')
 out.mkdir(parents=True);record={'status':'running','scope':'FSR4 production glue, no GPU/game/window','commands':[]}
 sysroot=Path((ROOT/'.deps/graphics/active-sysroot.txt').read_text().strip());env={'PATH':'/usr/bin:/bin','LANG':'C.UTF-8','UBSAN_OPTIONS':'halt_on_error=1'}
 if os.environ.get('VK_DRIVER_FILES'):env['VK_DRIVER_FILES']=os.environ['VK_DRIVER_FILES']
 if os.environ.get('HOME'):env['HOME']=os.environ['HOME']
 env['LD_LIBRARY_PATH']=str(sysroot/'usr/lib/x86_64-linux-gnu')
 def save():(out/'receipt.json').write_text(json.dumps(record,indent=2)+'\n')
 def run(name,command):
  start=time.monotonic();r=subprocess.run(list(map(str,command)),cwd=out,env=env,capture_output=True,timeout=60);(out/(name+'.stdout.log')).write_bytes(r.stdout);(out/(name+'.stderr.log')).write_bytes(r.stderr);record['commands'].append({'name':name,'argv':list(map(str,command)),'exit':r.returncode,'seconds':time.monotonic()-start});save()
  if r.returncode:raise RuntimeError(name+': '+r.stdout.decode()[-500:]+r.stderr.decode()[-3000:])
  return r.stdout.decode()
 try:
  temporal=(ROOT/'runtime/nvn/vulkan/temporal.c').read_text();fixture=(ROOT/'tests/native/test_fsr4_glue_linux.c').read_text();fixture=fixture.replace('// GENERATED_TEMPORAL_SELECTION',renderer.extract(temporal,'tmp_enabled')+renderer.extract(temporal,'tmp_linear_output')+renderer.extract(temporal,'use_dlss')+renderer.extract(temporal,'temporal_provider'));(out/'glue.c').write_text(fixture)
  device=(ROOT/'runtime/nvn/vulkan/device.c').read_text();fixture=(ROOT/'tests/native/test_fsr4_capabilities_linux.c').read_text().replace('// GENERATED_DEVICE_PREPARATION',renderer.extract(device,'fsr4_device_prepare'));(out/'capabilities.c').write_text(fixture)
  flags=['-std=gnu11','-O1','-UNDEBUG','-ffp-contract=off','-fno-strict-aliasing','-ffunction-sections','-fdata-sections','-DXCDE_LINUX_FSR3','-I'+str(ROOT/'runtime'),'-I'+str(sysroot/'usr/include')]
  if a.ubsan:flags+=['-fsanitize=undefined','-fno-sanitize-recover=all']
  # Generated source remains in the private run directory: absolute includes
  # replace only known authored fixture relative paths, never shell strings.
  for name in ['glue','capabilities']:
   file=out/(name+'.c');file.write_text(file.read_text().replace('../../runtime/',str(ROOT/'runtime')+'/'))
   for enabled in [False,True]:
    suffix=name+('-enabled' if enabled else '-disabled');binary=out/suffix
    run('compile-'+suffix,[ROOT/'.deps/bin/clang-cc',*flags,*(['-DXCDE_LINUX_FSR4'] if enabled else []),file,'-Wl,--gc-sections','-lm','-ldl','-o',binary])
    state=out/(suffix+'-state');state.mkdir(mode=0o700);print(run(suffix,[binary,state]),end='')
  for fsr3 in [False,True]:
   for fsr4 in [False,True]:
    name='framegen-inputs-'+str(int(fsr3))+str(int(fsr4));binary=out/name
    run('compile-'+name,[ROOT/'.deps/bin/clang-cc',*[f for f in flags if f!='-DXCDE_LINUX_FSR3'],*(['-DXCDE_LINUX_FSR3'] if fsr3 else []),*(['-DXCDE_LINUX_FSR4'] if fsr4 else []),ROOT/'tests/native/test_temporal_framegen_linux.c','-lm','-o',binary])
    print(run(name,[binary]),end='')
  if a.real_device:
   import shlex
   import setup_linux_fsr4 as component
   import prepare_linux_fsr4 as models
   import setup_linux_fsr as fidelityfx
   module=component.verify_existing();fg=fidelityfx.verify_existing()
   record['fsr4_component']=module;record['framegen_component']=fg
   bridge=(ROOT/'tests/native/fsr4_pipeline_bridge.c').read_text().replace('// GENERATED_TEMPORAL_SELECTION',renderer.extract(temporal,'tmp_enabled')+renderer.extract(temporal,'tmp_linear_output')+renderer.extract(temporal,'use_dlss')+renderer.extract(temporal,'temporal_provider')).replace('// GENERATED_DEVICE_PREPARATION',renderer.extract(device,'fsr4_device_prepare')).replace('../../runtime/',str(ROOT/'runtime')+'/');(out/'bridge.c').write_text(bridge)
   cc=ROOT/'.deps/bin/clang-cc';cxx=ROOT/'.deps/bin/clang-cxx'
   run('compile-bridge',[cc,*flags,'-DXCDE_LINUX_FSR4','-c',out/'bridge.c','-o',out/'bridge.o'])
   run('compile-guard',[cc,'-O1','-ffunction-sections','-fdata-sections','-c',ROOT/'runtime/platform/linux_state_guard.c','-o',out/'guard.o'])
   shader_flags=shlex.split(run('shader-flags',[ROOT/'.deps/bin/pkg-config-graphics','--cflags','--libs','shaderc','sdl3']))
   run('compile-shaders',[cc,'-O1','-UNDEBUG','-ffunction-sections','-fdata-sections',ROOT/'tests/native/test_temporal_shaders_linux.c',*shader_flags,'-Wl,--gc-sections','-o',out/'shader-fixture'])
   shaders=out/'shaders';shaders.mkdir();print(run('shaders',[out/'shader-fixture',shaders]),end='')
   vk_flags=shlex.split(run('vk-flags',[ROOT/'.deps/bin/pkg-config-graphics','--cflags','--libs','vulkan']))
   run('compile-pipeline',[cxx,'-std=c++17','-O1','-UNDEBUG','-ffp-contract=off','-fno-strict-aliasing','-ffunction-sections','-fdata-sections',*(['-fsanitize=undefined','-fno-sanitize-recover=all'] if a.ubsan else []),'-DFG_LINUX_PROVIDER','-I'+fg['header_dir'],ROOT/'tests/native/test_fsr4_pipeline_linux.cpp',ROOT/'runtime/nvn/vulkan/framegen_linux.cpp',out/'bridge.o',out/'guard.o',*vk_flags,'-Wl,--gc-sections','-ldl','-pthread','-o',out/'pipeline-fixture'])
   assets=models.verify_assets();record['fsr4_assets']=assets
   state=out/'gpu-state';state.mkdir(mode=0o700)
   print(run('gpu',[out/'pipeline-fixture',state,module['library_path'],assets['asset_root'],shaders,fg['library_path']]),end='')
   record['gpu_environment']=env
  sources=['runtime/nvn/vulkan/temporal.c','runtime/nvn/vulkan/temporal.h','runtime/nvn/vulkan/temporal_fsr4_linux.inc','runtime/nvn/vulkan/fsr4_api.h','runtime/nvn/vulkan/fsr4_policy.h','runtime/nvn/vulkan/device.c','runtime/nvn/vulkan/vk.h','tests/native/test_fsr4_glue_linux.c','tests/native/test_fsr4_capabilities_linux.c','tests/native/test_temporal_framegen_linux.c','runtime/nvn/vulkan/temporal_fg_linux.inc','tests/native/fsr4_pipeline_bridge.c','tests/native/test_fsr4_pipeline_linux.cpp','tests/native/test_fsr4_linux.cpp','tests/native/temporal_gpu_encode.h','scripts/fsr4_glue_tests_linux.py'];record['sources']={s:hashlib.sha256((ROOT/s).read_bytes()).hexdigest() for s in sources};record['status']='passed'
 except Exception as e:record.update(status='failed',error=str(e));raise
 finally:save();print(out/'receipt.json')
if __name__=='__main__':main()
