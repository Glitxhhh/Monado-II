#!/usr/bin/env python3
"""Explicit local Windows FSR4 Lite synthetic capture/translation preparation.

No downloads, game input, driver installation, terms acceptance or automatic
selection. Requires the exact supplied AMD SDK and checked local native tools.
"""
from __future__ import annotations
import argparse, contextlib, hashlib, json, os, re, sys, time
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent))
import setup_windows_fsr4 as component
import setup_windows_fsr4_tools as tools
from setup_windows_fsr4 import ROOT, guard, sha, document, encoded, Image
from fsr4_process_windows import run, Cancelled

DIMENSIONS=((1280,720,1920,1080),(640,360,1920,1080),(2560,1440,3840,2160),(1280,720,3840,2160))
MAX_LOG=4*1024*1024

def checkpoint(cancel_file):
    if cancel_file is None:return
    try:data=guard.read_regular(cancel_file,max_bytes=1024,owner=True)
    except OSError as error:
        if getattr(error,'winerror',None) in (2,3) or getattr(error,'errno',None) in (2,3):return
        raise
    raise Cancelled('Optional model preparation cancelled; previous selection is unchanged')

def verify_component(path):
    path=Path(guard.canonical_path(path))
    with guard.locked_directory(path.parent,owner=True) as root,root.open_regular(path.name,max_bytes=64*1024*1024,owner=True) as stream:
        data=stream.read();identity=sha(data);side=root.read_regular(path.name+'.sha256',max_bytes=65,owner=True)
        if side not in (identity.encode(),(identity+'\n').encode()):raise ValueError('Native FSR4 component hash sidecar differs')
        image=Image(path)
        if image.exports()!=['monado_fsr4_get_api']:raise ValueError('Native FSR4 component export differs')
        receipt=document(root.read_regular('receipt.json',max_bytes=1024*1024,owner=True))
        if receipt.get('schema')!=1 or receipt.get('status')!='passed' or receipt.get('abi')!=2 or receipt.get('format')!='windows-amd64-pe' or receipt.get('dll_sha256')!=identity:raise ValueError('Native FSR4 component needs its passed ABI2 build receipt')
        return {'component':str(path),'dll_sha256':identity}

def verify_prepared(output,component=None):
    """Read-only exact completed52-file validation for a cooperating GUI worker."""
    with tools.sources() as (preparation,prep_raw),component_module_sources() as (lock,_),guard.locked_directory(output,owner=True) as root:
        raw=root.read_regular('receipt.json',max_bytes=4*1024*1024,owner=True);receipt=document(raw)
        required=component_module_verify(receipt,lock)
        prep=receipt.get('preparation')
        if not isinstance(prep,dict) or prep.get('platform')!='windows-amd64' or prep.get('lock_sha256')!=sha(prep_raw) or prep.get('capture_complete')!=4 or prep.get('validated_shaders')!=48 or prep.get('gpu_qualified') is not True:raise ValueError('Native preparation completion/identity differs')
        gpu_commands=[c for c in prep.get('commands',[]) if isinstance(c,dict) and c.get('stage')=='gpu-readback']
        if len(gpu_commands)!=1 or gpu_commands[0].get('exit')!=0 or prep.get('gpu_validator_sha256') is None:raise ValueError('Successful native GPU validator execution is required')
        if root.read_regular('models.id',max_bytes=65,owner=True)!=(lock['model_identity']+'\n').encode():raise ValueError('Model completion identity missing or changed')
        for name in sorted(required):
            item=receipt['resources'][name];data=root.read_regular(name,max_bytes=component_max_spirv,owner=True)
            if item!={'bytes':len(data),'sha256':sha(data)}:raise ValueError('Prepared resource differs: '+name)
            if name.endswith('.spv') and data[:4]!=b'\x03\x02\x23\x07':raise ValueError('Invalid prepared SPIR-V')
            if root.read_regular(name+'.sha256',max_bytes=65,owner=True)!=(sha(data)+'\n').encode():raise ValueError('Prepared resource sidecar differs')
        result=dict(asset_root=str(root.path),receipt_path=str(root.path/'receipt.json'),receipt_sha256=sha(raw),model_identity=lock['model_identity'],gpu_qualified=True)
        if component is not None:result.update(verify_component(component))
        return result

# Aliases keep the public component= parameter unambiguous.
component_module_sources=component.sources
component_module_verify=component.verify_model_record
component_max_spirv=component.MAX_SPIRV

@contextlib.contextmanager
def checked_captures(paths):
    """Bound every recorder output and hold all read handles during extraction."""
    with contextlib.ExitStack() as leases:
        total=0
        for path in paths:
            directory=leases.enter_context(guard.locked_directory(path,owner=True));names=list(directory.path.iterdir())
            if not 1<=len(names)<=4096:raise ValueError('Capture file count exceeds cap')
            for name in names:
                if name.name!='trace.txt' and not re.fullmatch(r'(?:cs_[0-9a-f]{16}\.dxil|(?:data|root)_[0-9a-f]{16}\.bin)',name.name):raise ValueError('Unknown capture member')
                stream=leases.enter_context(directory.open_regular(name.name,max_bytes=64*1024*1024,owner=True));total+=os.fstat(stream.fileno()).st_size
                if total>512*1024*1024:raise ValueError('Capture total exceeds cap')
            trace=directory.read_regular('trace.txt',max_bytes=16*1024*1024,owner=True)
            if b'\x00' in trace:raise ValueError('Invalid capture trace encoding')
            trace.decode('utf8')
        yield

def prepare(output,sdk,tool_root,component_path,cancel_file=None):
    if os.name!='nt':raise OSError('Native Windows preparation required')
    # All missing/untrusted external prerequisites fail before creating output.
    with tools.sources() as (prep_lock,prep_raw),component.sources() as (lock,_):
        tool_record=tools.verify_tools(tool_root);component_record=verify_component(component_path);component_path=Path(component_record['component'])
        sdk=Path(guard.canonical_path(sdk));sdk_data=guard.read_regular(sdk,max_bytes=64*1024*1024,owner=True)
        if sha(sdk_data)!=lock['sdk']['sha256']:raise ValueError('Supplied AMD SDK differs from the exact accepted SDK hash; nothing executed')
        if cancel_file is not None:cancel_file=Path(guard.canonical_path(cancel_file))
        check=lambda:checkpoint(cancel_file)
        check()
        with tools.fresh(output) as out,guard.locked_directory(tool_root,owner=True) as checked_tools,contextlib.ExitStack() as leases:
            with out.child('logs',create=True,owner=True),out.child('work',create=True,owner=True) as work,out.child('translated',create=True,owner=True) as translated:
                commands=[];capture_paths=[]
                receipt=dict(schema=1,status='running',model='int8_lite',model_identity=lock['model_identity'],sdk=lock['sdk'],resources={},
                    preparation=dict(platform='windows-amd64',lock_sha256=sha(prep_raw),recipe_sha256=sha(Path(__file__).read_bytes()),tools_receipt_sha256=tool_record['receipt_sha256'],sdk_sha256=sha(sdk_data),capture_complete=0,validated_shaders=0,gpu_qualified=False,commands=commands),
                    scope='local synthetic SDK compute and structural SPIR-V validation; no gameplay or GPU readback qualification')
                out.write_new('preparation-start.json',encoded(receipt))
                env={k:v for k,v in os.environ.items() if not k.upper().startswith(('PYTHON','FSR4','DXIL_','VKD3D_','DXVK_','WINE','PROTON'))}
                env.update(PYTHONNOUSERSITE='1',PYTHONDONTWRITEBYTECODE='1',PYTHONUNBUFFERED='1',FSR4CAP_MODEL='lite',VKD3D_DISABLE_EXTENSIONS='VK_KHR_cooperative_matrix,VK_NV_cooperative_matrix2',VKD3D_SHADER_CACHE_PATH=str(work.path),DXVK_STATE_CACHE_PATH=str(work.path),__GL_SHADER_DISK_CACHE_PATH=str(work.path),TEMP=str(work.path),TMP=str(work.path))
                env['PATH']=str(checked_tools.path/'bin')+os.pathsep+str(Path(os.environ.get('SystemRoot','C:/Windows'))/'System32')
                def execute(name,argv,cwd,timeout=240):
                    check();item=run(argv,cwd=cwd,log=out.path/'logs'/(name+'.log'),environment=env,timeout=timeout,checkpoint=check);item['stage']=name;commands.append(item)
                    if item['exit']:raise RuntimeError(name+' failed (exit '+str(item['exit'])+'); see '+item['log'])
                try:
                    for name in tool_record['tools']:
                        stream=leases.enter_context(checked_tools.open_regular('bin/'+name,max_bytes=64*1024*1024,owner=True))
                        item=tool_record['tools'][name]
                        if os.fstat(stream.fileno()).st_size!=item['bytes'] or hashlib.file_digest(stream,'sha256').hexdigest()!=item['sha256']:raise ValueError('Preparation tool changed before its execution lease: '+name)
                    tools.progress('capture',0,4,'Recording synthetic local Lite graphs; no game input')
                    for index,(rw,rh,ow,oh) in enumerate(DIMENSIONS):
                        with work.child('set-'+str(index),create=True,owner=True) as capture:
                            capture.write_new('source-sdk.dll',sdk_data)
                            for name in ('fsr4cap.exe','amdxc64.dll',*sorted(tools.SUPPORT)):
                                capture.write_new(name,checked_tools.read_regular('bin/'+name,max_bytes=64*1024*1024,owner=True))
                            execute('select-lite-'+str(index),[sys.executable,'-I','-S','-B','-X','utf8',ROOT/'third_party/fsr4cap/driver_model.py','force',capture.path/'source-sdk.dll',capture.path/'amd_fidelityfx_upscaler_dx12.dll','lite'],capture.path)
                            with contextlib.ExitStack() as images:
                                for name in ('fsr4cap.exe','amdxc64.dll',*sorted(tools.SUPPORT),'amd_fidelityfx_upscaler_dx12.dll'):
                                    images.enter_context(capture.open_regular(name,max_bytes=64*1024*1024,owner=True))
                                execute('capture-'+str(index),[capture.path/'fsr4cap.exe','4.1.',str(rw)+'x'+str(rh),str(ow)+'x'+str(oh),'3'],capture.path)
                            message=capture.read_regular('fsr4cap.log',max_bytes=MAX_LOG,owner=True)
                            if not message.rstrip().endswith(b'frames done'):raise ValueError('Synthetic recorder did not complete its frames')
                            source=capture.path/('capture_'+str(rw)+'x'+str(rh)+'_'+str(ow)+'x'+str(oh));capture_paths.append(source)
                            # Extraction sees only the four fixed ordinary capture directories.
                            with checked_captures([source]),guard.locked_directory(source,owner=True) as original,work.child(source.name,create=True,owner=True) as copy:
                                entries=list(original.path.iterdir())
                                if len(entries)>4096:raise ValueError('Too many captured files')
                                for entry in entries:copy.write_new(entry.name,original.read_regular(entry.name,max_bytes=64*1024*1024,owner=True))
                            receipt['preparation']['capture_complete']=index+1;tools.progress('capture',index+1,4,'Captured Lite set '+lock['sets'][index])
                    with checked_captures([work.path/p.name for p in capture_paths]):
                        tools.progress('translate',0,0,'Checking exact dispatch/constants rules and translating Lite DXIL')
                        execute('translate-models',[sys.executable,'-I','-S','-B','-X','utf8',ROOT/'third_party/fsr4cap/extract.py',checked_tools.path/'bin/dxil-spirv.exe',work.path,translated.path],work.path,timeout=600)
                    resources={};validated=0
                    for tier in lock['sets']:
                        for leaf in [*[p+'.spv' for p in lock['passes']],'initializer.bin']:
                            name='lite/'+tier+'/'+leaf;data=translated.read_regular(name,max_bytes=component.MAX_SPIRV,owner=True)
                            if leaf.endswith('.spv'):
                                if not 20<=len(data)<=component.MAX_SPIRV or len(data)%4 or data[:4]!=b'\x03\x02\x23\x07':raise ValueError('Malformed translated SPIR-V')
                                execute('validate-'+tier+'-'+leaf,[checked_tools.path/'bin/spirv-val.exe','--target-env','vulkan1.3',translated.path/name],work.path)
                                validated+=1;tools.progress('validate',validated,48,'Validated real translated shader')
                            elif len(data)!=27392:raise ValueError('Lite initializer size differs')
                            with out.child('lite/'+tier,create=True,owner=True) as destination:
                                destination.write_new(leaf,data);destination.write_new(leaf+'.sha256',(sha(data)+'\n').encode())
                            resources[name]={'bytes':len(data),'sha256':sha(data)}
                    receipt['resources']=resources;receipt['preparation']['validated_shaders']=validated
                    tools.progress('gpu',0,1,'Checking all four Lite sets with native finite/detail/reset/sharpening readback')
                    with contextlib.ExitStack() as model_leases:
                        for name in resources:model_leases.enter_context(out.open_regular(name,max_bytes=component.MAX_SPIRV,owner=True))
                        model_leases.enter_context(guard.open_regular(component_path,max_bytes=64*1024*1024,owner=True))
                        if verify_component(component_path)!=component_record:raise ValueError('Native FSR4 component changed before GPU execution')
                        execute('gpu-readback',[checked_tools.path/'bin/fsr4-validate.exe',component_path,out.path],work.path,timeout=600)
                    gpu_log=out.read_regular('logs/gpu-readback.log',max_bytes=MAX_LOG,owner=True)
                    if not gpu_log.rstrip().endswith(b'MONADO_FSR4_GPU_QUALIFIED all4sets frames33 finite detail reset sharpening readback; no gameplay/performance qualification'):raise ValueError('Native GPU validator did not report its complete fixed qualification')
                    receipt['preparation'].update(gpu_qualified=True,gpu_validator_sha256=tool_record['tools']['fsr4-validate.exe']['sha256']);receipt['status']='passed'
                    component.verify_model_record(receipt,lock)
                    out.write_new('receipt.json',encoded(receipt));out.write_new('models.id',(lock['model_identity']+'\n').encode())
                    tools.progress('complete',1,1,'Local Lite models passed native GPU component checks; selected upscaler is unchanged')
                except BaseException as error:
                    receipt.update(status='cancelled' if isinstance(error,(Cancelled,KeyboardInterrupt)) else 'failed',error=str(error))
                    out.write_new('failed.json',encoded(receipt));raise
            return verify_prepared(out.path,component_path)

def main(argv=None):
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--prepare',action='store_true');p.add_argument('--output',type=Path,required=True)
    for name in ('sdk','tools','component','cancel-file'):p.add_argument('--'+name,type=Path)
    a=p.parse_args(argv)
    try:
        if a.prepare and any(getattr(a,n) is None for n in ('sdk','tools','component')):raise ValueError('--prepare requires explicit local --sdk --tools --component')
        print(json.dumps(prepare(a.output,a.sdk,a.tools,a.component,a.cancel_file) if a.prepare else verify_prepared(a.output,a.component),indent=2));return 0
    except (OSError,ValueError,RuntimeError,KeyError) as error:p.exit(130 if isinstance(error,Cancelled) else 1,str(error)+'\n')
if __name__=='__main__':main()
