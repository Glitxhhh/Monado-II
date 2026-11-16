"""Native real-ImGui/SDL3 authored virtual gamepad recovery, dummy video only."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import uuid

PORT=Path(__file__).resolve().parents[1]
SCENARIOS=('delayed','focus-retry','replacement','healthy','all','manual','release','retry-backoff','all-addition','bounded-errors')

def main(argv=None):
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--dependency-root',type=Path);a=p.parse_args(argv)
    if os.name!='nt':p.error('Native Windows is required')
    prefix=a.dependency_root or Path(json.loads((PORT/'.deps/windows-deps.json').read_text())['prefix'])
    if not (prefix/'lib/libSDL3.dll.a').is_file():p.error('Pinned native SDL3 import library required')
    output=PORT/'runs'/('imgui-controller-'+str(time.time_ns())+'-'+uuid.uuid4().hex[:8]);output.mkdir()
    imgui=PORT/'third_party/imgui'
    sources=[PORT/'tests/native/test_controller_imgui_recovery_windows.cpp',*[imgui/name for name in ('imgui.cpp','imgui_draw.cpp','imgui_tables.cpp','imgui_widgets.cpp')]]
    bound=sources+[imgui/'backends/imgui_impl_sdl3.cpp',imgui/'backends/imgui_impl_sdl3.h',imgui/'imgui.h',imgui/'imgui_internal.h',
        PORT/'runtime/platform/sdl_controller_recovery.h',PORT/'runtime/platform/sdl_controller_diagnostics.h',Path(__file__)]
    receipt={'scope':'native authored virtual pads and actual ImGui SDL backend; no game/human input/profile interaction',
        'platform':'windows-amd64','sources':{str(path.relative_to(PORT)):hashlib.sha256(path.read_bytes()).hexdigest() for path in bound},
        'dependency_prefix':str(prefix),'cases':[],'status':'running'}
    environment=dict(os.environ,PATH=str(prefix/'bin')+os.pathsep+os.environ.get('PATH',''),SDL_VIDEODRIVER='dummy',SDL_AUDIODRIVER='dummy',
        ZIG_GLOBAL_CACHE_DIR=str(PORT/'.deps/zig-global-cache'),ZIG_LOCAL_CACHE_DIR=str(output/'zig-cache'))
    binary=output/'imgui-controller.exe'
    command=[sys.executable,'-B','-m','ziglang','c++','-std=c++17','-O1','-UNDEBUG','-march=x86_64_v2','-ffp-contract=off','-fno-strict-aliasing',
        '-I'+str(imgui),'-I'+str(prefix/'include'),*map(str,sources),str(prefix/'lib/libSDL3.dll.a'),'-static','-o',str(binary)]
    receipt['compile']=command
    try:
        print('Compiling real native ImGui SDL3 controller fixture',flush=True)
        result=subprocess.run(command,env=environment,capture_output=True,text=True,timeout=180)
        (output/'compile.log').write_text(result.stdout+result.stderr)
        if result.returncode:raise RuntimeError('Native controller fixture compilation failed; see '+str(output/'compile.log'))
        receipt['binary_sha256']=hashlib.sha256(binary.read_bytes()).hexdigest()
        for scenario in SCENARIOS:
            result=subprocess.run([str(binary),scenario],cwd=output,env=environment,capture_output=True,text=True,timeout=20)
            (output/(scenario+'.log')).write_text(result.stdout+result.stderr)
            passed=result.returncode==0 and 'native ImGui controller recovery: PASS '+scenario in result.stdout
            receipt['cases'].append({'scenario':scenario,'exit':result.returncode,'passed':passed})
            print(('PASS ' if passed else 'FAIL ')+scenario,flush=True)
            if not passed:raise RuntimeError('Native controller scenario failed: '+scenario)
        receipt['status']='passed';return 0
    except (OSError,RuntimeError,subprocess.TimeoutExpired) as error:
        receipt.update(status='failed',error=str(error));print(str(error),file=sys.stderr);return 1
    finally:
        (output/'receipt.json').write_text(json.dumps(receipt,indent=2)+'\n');print('Native controller evidence: '+str(output),flush=True)

if __name__=='__main__':sys.exit(main())
