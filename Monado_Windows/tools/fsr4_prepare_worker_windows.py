"""Guarded native FSR4 worker orchestration and verified profile selection.

The trusted caller authenticates this module and the preparation pipeline before
importing either. No candidate/model code is imported. Every retry has a fresh
output; only a fully verified preparation can atomically change the selection.
"""
from contextlib import ExitStack, redirect_stdout
import hashlib
import json
from pathlib import Path
import re
import time
import uuid
import io
import importlib.util
import os
import sys
from contextlib import contextmanager

SELECTION='fsr4-preparation.json'
KIND='native-windows-fsr4-preparation'
HASH=re.compile(r'[0-9a-f]{64}\Z')
IDENTITY='b09b7dd7cbdfb5977df58fbf02d02ca66f860ec26cfadd855abb8eee35c933b6'
BOUND=32768
_UNSET=object()
FIELDS={'schema','kind','status','asset_root','receipt_path','receipt_sha256','model_identity',
        'component','dll_sha256','gpu_qualified','sdk','tools','selection_id','created_utc'}
TOOLS_SELECTION='fsr4-tools.json'
TOOLS_KIND='native-windows-fsr4-tools-selection'
TOOLS_FIELDS={'schema','kind','status','tools','receipt_sha256','component','component_sha256','archives','support','cmake','ninja','compiler','vulkan_prefix','selection_id','created_utc'}

class PreparationError(ValueError):pass

def encoded(value):
    return (json.dumps(value,ensure_ascii=False,sort_keys=True,indent=2)+'\n').encode('utf-8')

def document(raw,*,limit=BOUND):
    if len(raw)>limit:raise PreparationError('FSR4 selection exceeds its bound')
    def unique(pairs):
        result={}
        for name,value in pairs:
            if name in result:raise PreparationError('Duplicate FSR4 selection field')
            result[name]=value
        return result
    try:value=json.loads(raw,object_pairs_hook=unique,parse_constant=lambda _:(_ for _ in ()).throw(PreparationError('Nonfinite FSR4 selection')))
    except (ValueError,UnicodeError,RecursionError) as error:raise PreparationError('Invalid FSR4 selection') from error
    if not isinstance(value,dict):raise PreparationError('FSR4 selection must be an object')
    return value

def optional(guard,name):
    try:return guard.read_regular(name,max_bytes=BOUND,owner=True)
    except OSError as error:
        if error.errno in (2,3) or getattr(error,'winerror',None) in (2,3):return None
        raise

def progress_record(text):
    prefix='MONADO_FSR4_PROGRESS '
    if not text.startswith(prefix):return None
    row=document(text[len(prefix):].encode('utf-8'))
    if (set(row)!={'stage','current','total','message'} or not isinstance(row['stage'],str) or
        not re.fullmatch(r'[a-z0-9-]{1,48}',row['stage']) or
        type(row['current']) is not int or type(row['total']) is not int or
        not 0<=row['current']<=1000000000 or not 0<=row['total']<=1000000000 or
        not isinstance(row['message'],str) or len(row['message'])>2048):
        raise PreparationError('Invalid bounded FSR4 worker progress')
    return {'phase':'fsr4-'+row['stage'],'done':row['current'],'total':row['total'],'message':row['message']}

def run_verified(pipeline,argv,*,emit,log=lambda text:None):
    """Run the already authenticated pipeline; its native children own jobs.

    This avoids a second Python startup and streams real pipeline events while
    the native frontend retains its complete worker/code/interpreter leases.
    """
    flags=[str(item) for item in argv[5:]]
    # --prepare has no value; every remaining explicit option has one.
    if (len(flags)!=11 or flags[0]!='--prepare' or
        flags[1::2]!=['--sdk','--tools','--component','--output','--cancel-file']):
        raise PreparationError('Unexpected fixed FSR4 worker invocation')
    return run_reported(lambda:pipeline.prepare(Path(flags[8]),Path(flags[2]),Path(flags[4]),Path(flags[6]),Path(flags[10])),emit=emit,log=log)

def run_reported(function,*,emit,log=lambda text:None):
    class Lines(io.TextIOBase):
        def __init__(self):self.pending=''
        def write(self,text):
            self.pending+=text
            if len(self.pending)>32768:raise PreparationError('Pipeline output line exceeds its bound')
            while '\n' in self.pending:
                line,self.pending=self.pending.split('\n',1);log(line+'\n')
                row=progress_record(line.strip())
                if row:emit(**row)
            return len(text)
        def flush(self):pass
    stream=Lines()
    with redirect_stdout(stream):
        # The only execution boundary is the installed, content-bound module.
        # No downloaded model/component code is imported into Python.
        result=function()
    if stream.pending:log(stream.pending)
    return result

def canonical(guard,path):return Path(guard.canonical_path(path))

def disjoint(*paths):
    for i,a in enumerate(paths):
        for b in paths[i+1:]:
            if a==b or a.is_relative_to(b) or b.is_relative_to(a):
                raise PreparationError('FSR4 source, tool and fresh output locations must not overlap')

def checked_result(pipeline,assets,component,guard):
    assets=canonical(guard,assets);component=canonical(guard,component)
    value=pipeline.verify_prepared(assets,component=component)
    required={'asset_root','receipt_path','receipt_sha256','model_identity','component','dll_sha256','gpu_qualified'}
    if (not isinstance(value,dict) or not required<=value.keys() or type(value.get('gpu_qualified')) is not bool or
        value.get('model_identity')!=IDENTITY or
        any(not isinstance(value.get(key),str) or not HASH.fullmatch(value[key]) for key in ('receipt_sha256','dll_sha256')) or
        canonical(guard,value['asset_root'])!=assets or canonical(guard,value['component'])!=component or
        canonical(guard,value['receipt_path'])!=assets/'receipt.json'):
        raise PreparationError('FSR4 verifier returned an incompatible or incomplete preparation identity')
    return {key:value[key] for key in required}

def known_selection(value):
    return (set(value)==FIELDS and type(value.get('schema')) is int and value['schema']==1 and
        value.get('kind')==KIND and value.get('status')=='verified' and
        isinstance(value.get('selection_id'),str) and re.fullmatch(r'[0-9a-f]{32}',value['selection_id']) is not None and
        type(value.get('created_utc')) is int and value['created_utc']>=0 and
        type(value.get('gpu_qualified')) is bool and value.get('model_identity')==IDENTITY and
        all(isinstance(value.get(key),str) and HASH.fullmatch(value[key]) for key in ('receipt_sha256','dll_sha256')) and
        all(isinstance(value.get(key),str) and value[key] for key in ('asset_root','receipt_path','component')) and
        all(isinstance(value.get(key),str) for key in ('sdk','tools')))

def known_tools_selection(value):
    return (set(value)==TOOLS_FIELDS and type(value.get('schema')) is int and value['schema']==1 and
        value.get('kind')==TOOLS_KIND and value.get('status')=='verified' and
        isinstance(value.get('receipt_sha256'),str) and HASH.fullmatch(value['receipt_sha256']) is not None and
        isinstance(value.get('component_sha256'),str) and HASH.fullmatch(value['component_sha256']) is not None and
        isinstance(value.get('selection_id'),str) and re.fullmatch(r'[0-9a-f]{32}',value['selection_id']) is not None and
        type(value.get('created_utc')) is int and value['created_utc']>=0 and
        all(isinstance(value.get(key),str) and value[key] for key in ('tools','component','archives','support','cmake','ninja','compiler','vulkan_prefix')))

def verify_tools_selection(profile,*,tools_module,component_verifier,guard):
    with guard.locked_directory(profile,owner=True) as owner:raw=optional(owner,TOOLS_SELECTION)
    if raw is None:return None
    value=document(raw)
    if not known_tools_selection(value):raise PreparationError('Unknown FSR4 tools selection preserved')
    checked=tools_module.verify_tools(canonical(guard,value['tools']))
    if checked.get('receipt_sha256')!=value['receipt_sha256'] or canonical(guard,checked.get('output',''))!=canonical(guard,value['tools']):
        raise PreparationError('FSR4 tools differ from their durable selection')
    component=component_verifier(canonical(guard,value['component']))
    if (canonical(guard,component.get('component',''))!=canonical(guard,value['component']) or component.get('dll_sha256')!=value['component_sha256']):
        raise PreparationError('FSR4 component differs from its durable tools selection')
    return value

def build_tools(*,installation,profile,archives,support,cmake,ninja,compiler,vulkan_prefix,jobs,tools_module,component_module,component_verifier,guard,
                progress,checkpoint=lambda:None,idle=lambda:None,output=None,protected=(),log=lambda text:None):
    """Build explicitly supplied local prerequisites; never activate a model."""
    if type(jobs) is not int or not 1<=jobs<=6:raise PreparationError('Choose one through six preparation workers')
    installation=canonical(guard,installation);profile=canonical(guard,profile)
    sources={key:canonical(guard,value) for key,value in dict(archives=archives,support=support,cmake=cmake,ninja=ninja,compiler=compiler,vulkan_prefix=vulkan_prefix).items()}
    with ExitStack() as stack:
        stack.enter_context(guard.installation_lease(installation));stack.enter_context(guard.installation_lease(profile))
        owner=stack.enter_context(guard.locked_directory(installation,owner=True));state=stack.enter_context(guard.locked_directory(profile,owner=True))
        previous=optional(state,TOOLS_SELECTION)
        if previous is not None and not known_tools_selection(document(previous)):raise PreparationError('Unknown FSR4 tools selection preserved before building')
        for name,path in sources.items():
            if name in ('cmake','ninja','compiler'):
                parent=stack.enter_context(guard.locked_directory(path.parent,owner=False))
                stack.enter_context(parent.open_regular(path.name,max_bytes=512*1024*1024,owner=True))
            else:stack.enter_context(guard.locked_directory(path,owner=True))
        outputs=stack.enter_context(owner.child('fsr4-preparation-tools',create=True,owner=True))
        output=canonical(guard,output) if output is not None else Path(outputs.path)/('tools-'+str(time.time_ns())+'-'+uuid.uuid4().hex[:8])
        if output.parent!=Path(outputs.path) or not re.fullmatch(r'tools-[0-9]{1,24}-[0-9a-f]{8}',output.name):raise PreparationError('FSR4 tools output must be a fresh named attempt')
        for source in (*sources.values(),*map(lambda p:canonical(guard,p),protected)):disjoint(output,source)
        if output.exists():raise PreparationError('Fresh FSR4 tools output already exists')
        components=stack.enter_context(owner.child('fsr4-components',create=True,owner=True))
        component_output=Path(components.path)/('component-'+output.name[len('tools-'):])
        if component_output.exists():raise PreparationError('Fresh native component output already exists')
        for source in (*sources.values(),*map(lambda p:canonical(guard,p),protected)):disjoint(component_output,source)
        checkpoint();idle();progress('fsr4-tools','Building only explicitly selected local preparation sources')
        run_reported(lambda:tools_module.build(output,sources['archives'],sources['support'],sources['cmake'],sources['ninja'],sources['compiler'],
            jobs=jobs,cancel=checkpoint,vulkan_prefix=sources['vulkan_prefix']),
            emit=lambda **row:progress(row['phase'],row['message'],done=row['done'],total=row['total']),log=log)
        checkpoint();checked=tools_module.verify_tools(output)
        if (checked.get('schema')!=1 or checked.get('status')!='passed' or checked.get('kind')!='windows-fsr4-tools' or
            canonical(guard,checked.get('output',''))!=output or not isinstance(checked.get('receipt_sha256'),str) or not HASH.fullmatch(checked['receipt_sha256'])):
            raise PreparationError('Native preparation tools did not produce a fully verified receipt')
        checkpoint();progress('fsr4-component','Building the reviewed native Vulkan component with the same checked compiler')
        run_reported(lambda:component_module.build(component_output,compiler=sources['compiler'],vulkan_prefix=sources['vulkan_prefix'],cancel=checkpoint),
            emit=lambda **row:progress(row['phase'],row['message'],done=row['done'],total=row['total']),log=log)
        checkpoint();component=component_verifier(component_output/'monado-fsr4.dll')
        if (canonical(guard,component.get('component',''))!=component_output/'monado-fsr4.dll' or
            not isinstance(component.get('dll_sha256'),str) or not HASH.fullmatch(component['dll_sha256'])):
            raise PreparationError('Native component did not pass its receipt/export/import verification')
        value=dict(schema=1,kind=TOOLS_KIND,status='verified',tools=str(output),receipt_sha256=checked['receipt_sha256'],
            component=str(component_output/'monado-fsr4.dll'),component_sha256=component['dll_sha256'],
            **{key:str(path) for key,path in sources.items()},selection_id=uuid.uuid4().hex,created_utc=int(time.time()))
        checkpoint();idle()
        if optional(state,TOOLS_SELECTION)!=previous:raise PreparationError('FSR4 tools selection changed while building; current selection preserved')
        if previous is None:state.atomic_write_new(TOOLS_SELECTION,encoded(value),owner=True)
        else:state.atomic_replace(TOOLS_SELECTION,encoded(value),expected_sha256=hashlib.sha256(previous).hexdigest(),owner=True)
        progress('fsr4-tools-complete','Native preparation tools verified; current model and upscaler are unchanged')
        return value

def verify_selection(profile,*,pipeline,guard):
    """Return verified immutable asset/component identity, or None if absent.

    A changed/untrusted selection raises; callers may keep ordinary rendering
    available while reporting optional FSR4 unavailable. Never repair it silently.
    """
    profile=canonical(guard,profile)
    with guard.locked_directory(profile,owner=True) as owner:
        raw=optional(owner,SELECTION)
    if raw is None:return None
    value=document(raw)
    if not known_selection(value):
        raise PreparationError('Unrecognized FSR4 selection; existing file preserved')
    checked=checked_result(pipeline,value['asset_root'],value['component'],guard)
    if any(value.get(key)!=item for key,item in checked.items()):
        raise PreparationError('FSR4 preparation differs from its durable selection')
    return value

def activate(profile,*,assets,component,pipeline,guard,checkpoint=lambda:None,sdk='',tools='',idle=lambda:None,expected=_UNSET):
    """Publish only a freshly revalidated selection; no settings/model copy."""
    profile=canonical(guard,profile)
    with guard.installation_lease(profile),guard.locked_directory(profile,owner=True) as owner:
        previous=optional(owner,SELECTION)
        if expected is not _UNSET and previous!=expected:
            raise PreparationError('FSR4 selection changed during preparation; current model preserved')
        if previous is not None:
            original=document(previous)
            if not known_selection(original):
                raise PreparationError('Unknown FSR4 selection file preserved; choose another profile or resolve it manually')
        checked=checked_result(pipeline,assets,component,guard)
        value=dict(checked,schema=1,kind=KIND,status='verified',sdk=str(sdk),tools=str(tools),
                   selection_id=uuid.uuid4().hex,created_utc=int(time.time()))
        checkpoint();idle()
        # Pin the old selection observed before validation; an unrelated edit
        # during preparation cannot be overwritten as though it were ours.
        current=optional(owner,SELECTION)
        if current!=previous:raise PreparationError('FSR4 selection changed while validating; current selection preserved')
        raw=encoded(value)
        if previous is None:owner.atomic_write_new(SELECTION,raw,owner=True)
        else:owner.atomic_replace(SELECTION,raw,expected_sha256=hashlib.sha256(previous).hexdigest(),owner=True)
        return value

def prepare(*,installation,profile,sdk,tools,component,python,script,cancel_file,pipeline,guard,
            run,progress,checkpoint=lambda:None,idle=lambda:None,protected=(),output=None):
    installation=canonical(guard,installation);profile=canonical(guard,profile)
    sdk=canonical(guard,sdk);tools=canonical(guard,tools);component=canonical(guard,component)
    script=canonical(guard,script);python=canonical(guard,python)
    with ExitStack() as stack:
        stack.enter_context(guard.installation_lease(installation))
        stack.enter_context(guard.installation_lease(profile))
        owner=stack.enter_context(guard.locked_directory(installation,owner=True))
        profile_guard=stack.enter_context(guard.locked_directory(profile,owner=True))
        expected=optional(profile_guard,SELECTION)
        if expected is not None:
            prior=document(expected)
            if not known_selection(prior):
                raise PreparationError('Unknown FSR4 selection file preserved before preparation')
        # Every explicitly selected source must be an ordinary owned object.
        for path in (sdk,component,python,script):
            parent=stack.enter_context(guard.locked_directory(path.parent,owner=False))
            with parent.open_regular(path.name,max_bytes=2*1024*1024*1024,owner=True):pass
        stack.enter_context(guard.locked_directory(tools,owner=True))
        outputs=stack.enter_context(owner.child('fsr4-preparations',create=True,owner=True))
        output=canonical(guard,output) if output is not None else Path(outputs.path)/('prepare-'+str(time.time_ns())+'-'+uuid.uuid4().hex[:8])
        if output.parent!=Path(outputs.path) or not re.fullmatch(r'prepare-[0-9]{1,24}-[0-9a-f]{8}',output.name):
            raise PreparationError('FSR4 output must be a fresh named attempt in this installation preparation directory')
        # Input/model sources may be anywhere outside this fresh work namespace.
        for source in (sdk,tools,component,*map(lambda p:canonical(guard,p),protected)):
            disjoint(output,source)
        if output.exists():raise PreparationError('Fresh FSR4 preparation output already exists')
        checkpoint();idle();progress('fsr4-prepare','Preparing explicitly selected local SDK and tools')
        argv=[python,'-I','-S','-B',script,'--prepare','--sdk',sdk,'--tools',tools,'--component',component,
              '--output',output,'--cancel-file',cancel_file]
        run(argv,cwd=script.parent,phase='fsr4-prepare',output=None)
        checkpoint();progress('fsr4-verify','Checking every prepared model and the local native component')
        # The pipeline's read-only installed verifier checks all52 resources,
        # exact hashes/magic/sidecars/models.id and native component ABI.
        value=activate(profile,assets=output,component=component,pipeline=pipeline,guard=guard,
                       checkpoint=checkpoint,sdk=sdk,tools=tools,idle=idle,expected=expected)
        progress('fsr4-complete','FSR4 preparation verified'+('; native synthetic GPU validation passed' if value['gpu_qualified'] else '; GPU rendering qualification remains pending'))
        return value

def load_module(path,name):
    spec=importlib.util.spec_from_file_location(name,path)
    if spec is None or spec.loader is None:raise PreparationError('Cannot load a checked preparation helper')
    module=importlib.util.module_from_spec(spec);sys.modules[name]=module
    spec.loader.exec_module(module);return module

@contextmanager
def verified_context(path,expected_sha256):
    """Retain native leases for the build-bound context and complete closure.

    The native settings launcher pins this hash in its compiled binary and holds
    the interpreter/context members before Python startup. These first bounded
    reads authenticate the guard module; all reads are then repeated under its
    native owner/ordinary-file leases before any pipeline/package import.
    """
    if os.name!='nt':raise PreparationError('Native Windows preparation is required')
    if not isinstance(expected_sha256,str) or not HASH.fullmatch(expected_sha256):raise PreparationError('Missing build-bound preparation context identity')
    path=Path(path)
    with path.open('rb') as stream:raw=stream.read(16*1024*1024+1)
    if hashlib.sha256(raw).hexdigest()!=expected_sha256:raise PreparationError('Preparation context differs from the checked launcher')
    context=document(raw,limit=16*1024*1024)
    fields={'schema','platform','status','reason','installation','receipt','source_root','python','python_site','worker','pipeline','guard','files'}
    if (set(context)!=fields or type(context['schema']) is not int or context['schema']!=1 or
        context['platform']!='windows-amd64' or context['status'] not in ('available','unavailable') or
        not isinstance(context['reason'],str)):
        raise PreparationError('Unrecognized build preparation context')
    if context['status']!='available':raise PreparationError(context['reason'] or 'This source build has no pinned preparation interpreter; use the portable builder')
    files=context['files']
    if not isinstance(files,dict) or not 4<=len(files)<=20000:raise PreparationError('Invalid preparation context closure')
    for name,item in files.items():
        if (not isinstance(name,str) or not Path(name).is_absolute() or '..' in Path(name).parts or
            not isinstance(item,dict) or set(item)!={'bytes','sha256'} or type(item['bytes']) is not int or
            not 0<=item['bytes']<=512*1024*1024 or not isinstance(item['sha256'],str) or not HASH.fullmatch(item['sha256'])):
            raise PreparationError('Invalid preparation member identity')
    guard_path=Path(context['guard'])
    item=files.get(str(guard_path))
    if item is None:raise PreparationError('Preparation guard is outside its checked closure')
    with guard_path.open('rb') as stream:guard_raw=stream.read(min(item['bytes'],16*1024*1024)+1)
    if len(guard_raw)!=item['bytes'] or hashlib.sha256(guard_raw).hexdigest()!=item['sha256']:raise PreparationError('Preparation guard identity differs')
    guard=load_module(guard_path,'windows_file_guard')
    with ExitStack() as leases:
        path=canonical(guard,path);context_parent=leases.enter_context(guard.locked_directory(path.parent,owner=True))
        if context_parent.read_regular(path.name,max_bytes=16*1024*1024,owner=True)!=raw:raise PreparationError('Preparation context changed before its lease')
        source=canonical(guard,context['source_root']);python=canonical(guard,context['python']);python_home=python.parent
        for key in ('installation','receipt','source_root','python','python_site','worker','pipeline','guard'):
            if not isinstance(context[key],str):raise PreparationError('Invalid preparation context path')
            context[key]=str(canonical(guard,context[key]))
        if (canonical(guard,sys.executable)!=python or canonical(guard,__file__)!=Path(context['worker']) or
            Path(context['worker'])!=source/'Monado_Windows/tools/fsr4_prepare_worker_windows.py' or
            Path(context['pipeline'])!=source/'Monado_Windows/tools/prepare_windows_fsr4.py' or
            Path(context['guard'])!=source/'tools/windows_file_guard.py' or
            Path(context['python_site'])!=python_home/'Lib/site-packages'):
            raise PreparationError('Preparation interpreter or authored module location differs')
        canonical_files={};parents={};total=0
        for name,item in files.items():
            member=canonical(guard,name)
            if member in canonical_files or not (member.is_relative_to(source) or member.is_relative_to(python_home)):
                raise PreparationError('Aliased or out-of-scope preparation member')
            canonical_files[member]=item;total+=item['bytes']
            if total>2*1024*1024*1024:raise PreparationError('Preparation closure exceeds its size bound')
            if member.parent not in parents:parents[member.parent]=leases.enter_context(guard.locked_directory(member.parent,owner=False))
            stream=leases.enter_context(parents[member.parent].open_regular(member.name,max_bytes=item['bytes'],owner=True))
            if os.fstat(stream.fileno()).st_size!=item['bytes'] or hashlib.file_digest(stream,'sha256').hexdigest()!=item['sha256']:
                raise PreparationError('Checked preparation member changed: '+str(member))
        if any(Path(context[key]) not in canonical_files for key in ('worker','pipeline','guard','python')):
            raise PreparationError('Preparation entry is absent from the checked closure')
        # Isolated Python does not run site or .pth. No unlisted importable
        # member may redirect imports once authenticated search roots are used.
        count=0
        for directory,subdirs,names in os.walk(python_home,followlinks=False):
            count+=len(names)+len(subdirs)
            if count>25000:raise PreparationError('Interpreter membership exceeds its bound')
            parent=Path(directory)
            for name in subdirs:leases.enter_context(guard.locked_directory(parent/name,owner=True))
            if any(parent/name not in canonical_files for name in names):raise PreparationError('Unlisted interpreter member; pinned preparation is unavailable')
        for directory in (source/'Monado_Windows/tools',source/'tools'):
            leases.enter_context(guard.locked_directory(directory,owner=True))
            count=0
            for parent,subdirs,names in os.walk(directory,followlinks=False):
                count+=len(subdirs)+len(names)
                if count>20000:raise PreparationError('Authored import membership exceeds its bound')
                parent=Path(parent)
                for name in subdirs:
                    if name=='__pycache__':raise PreparationError('Unlisted authored bytecode cache; use a clean checked source export')
                    leases.enter_context(guard.locked_directory(parent/name,owner=True))
                for name in names:
                    entry=parent/name
                    if entry.suffix.lower() in ('.py','.pyw','.pyc','.pyd','.dll','.pth') and entry not in canonical_files:
                        raise PreparationError('Unlisted authored import member; rebuild the checked preparation context')
        receipt_path=Path(context['receipt']);receipt_parent=leases.enter_context(guard.locked_directory(receipt_path.parent,owner=True))
        receipt=document(receipt_parent.read_regular(receipt_path.name,max_bytes=4*1024*1024,owner=True),limit=4*1024*1024)
        if (receipt.get('schema')!=1 or receipt.get('status')!='success' or
            receipt.get('preparation_context_sha256')!=expected_sha256 or
            not isinstance(receipt.get('preparation_context'),str) or Path(receipt['preparation_context'])!=path):
            raise PreparationError('Completed build receipt does not bind this preparation context')
        # Appending does not evaluate .pth files or user customization.
        if context['python_site'] not in sys.path:sys.path.append(context['python_site'])
        pipeline=load_module(context['pipeline'],'monado_checked_fsr4_pipeline')
        bootstrap_path=source/'Monado_Windows/tools/bootstrap_windows.py'
        if bootstrap_path not in canonical_files:raise PreparationError('Checked preparation lacks the native process inventory helper')
        bootstrap=load_module(bootstrap_path,'monado_checked_fsr4_process_inventory')
        yield context,guard,pipeline,bootstrap

def main(argv=None):
    import argparse
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('context','profile','progress','cancel-file'):parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--context-sha256',required=True)
    modes=parser.add_mutually_exclusive_group()
    for name in ('verify-selection','build-tools','verify-tools'):modes.add_argument('--'+name,action='store_true')
    for name in ('sdk','tools','component','output'):parser.add_argument('--'+name,type=Path)
    for name in ('archives','support','cmake','ninja','compiler','vulkan-prefix'):parser.add_argument('--'+name,type=Path)
    parser.add_argument('--jobs',type=int,choices=range(1,7),default=4)
    args=parser.parse_args(argv);progress=None;pipeline=None
    try:
        with verified_context(args.context,args.context_sha256) as (context,guard,pipeline,bootstrap),ExitStack() as leases:
            profile=canonical(guard,args.profile);owner=leases.enter_context(guard.locked_directory(profile,owner=True))
            logs=leases.enter_context(owner.child('logs',create=True,owner=True));state=leases.enter_context(logs.child('fsr4-preparation',create=True,owner=True))
            progress_path=canonical(guard,args.progress);cancel=canonical(guard,args.cancel_file)
            if (progress_path.parent!=Path(state.path) or not re.fullmatch(r'progress-[0-9a-f]{32}\.jsonl',progress_path.name) or
                cancel.parent!=Path(state.path) or not re.fullmatch(r'cancel-[0-9a-f]{32}',cancel.name)):
                raise PreparationError('Worker progress and cancel paths must be fresh preparation state files')
            progress=bootstrap.Progress(Path(state.path),progress_path,progress_path)
            progress.installation=context['installation']
            def checkpoint():pipeline.checkpoint(cancel)
            def idle():
                binaries={Path(context['receipt']).parent/'build/xcde.exe',profile/'build/xcde.exe'}
                if bootstrap.busy_resources(binaries):raise PreparationError('Close this profile game manually before preparing or selecting models')
            checkpoint();idle()
            tool_fields={}
            if args.verify_selection:
                try:
                    selected_tools=verify_tools_selection(profile,tools_module=pipeline.tools,component_verifier=pipeline.verify_component,guard=guard)
                    if selected_tools:tool_fields={'fsr4_'+key:selected_tools[key] for key in ('tools','component','archives','support','cmake','ninja','compiler','vulkan_prefix')}
                except (OSError,ValueError,RuntimeError,KeyError,TypeError):
                    progress.emit('fsr4-tools-unavailable','Saved preparation tools are unavailable; their metadata and current model are unchanged')
            if args.build_tools:
                if any(getattr(args,name) is None for name in ('archives','support','cmake','ninja','compiler','vulkan_prefix','output')):
                    raise PreparationError('Tools build needs explicit local archives, support, CMake, Ninja, compiler, Vulkan prefix and fresh output')
                value=build_tools(installation=context['installation'],profile=profile,archives=args.archives,support=args.support,
                    cmake=args.cmake,ninja=args.ninja,compiler=args.compiler,vulkan_prefix=args.vulkan_prefix,jobs=args.jobs,
                    tools_module=pipeline.tools,component_module=pipeline.component,component_verifier=pipeline.verify_component,
                    guard=guard,progress=lambda phase,message,**extra:progress.emit(phase,message,**extra),
                    checkpoint=checkpoint,idle=idle,output=args.output,
                    protected=(Path(context['source_root'])/'tools',Path(context['source_root'])/'Monado_Windows/tools',
                               Path(context['source_root'])/'Monado_Windows/third_party',Path(context['python']).parent))
            elif args.verify_tools:
                value=verify_tools_selection(profile,tools_module=pipeline.tools,component_verifier=pipeline.verify_component,guard=guard)
                if value is None:
                    progress.emit('fsr4-tools-unavailable','No verified preparation tools are selected',status='needs-setup');return 0
            elif args.verify_selection:
                value=verify_selection(profile,pipeline=pipeline,guard=guard)
                if value is None:
                    progress.emit('fsr4-unavailable','No verified FSR4 preparation is selected'+('; local tools are verified' if tool_fields else ''),status='needs-setup',**tool_fields);return 0
            else:
                if any(getattr(args,name) is None for name in ('sdk','tools','component','output')):
                    raise PreparationError('Preparation needs explicit local SDK, tools, component and fresh output')
                value=prepare(installation=context['installation'],profile=profile,sdk=args.sdk,tools=args.tools,component=args.component,
                    python=context['python'],script=context['pipeline'],cancel_file=cancel,pipeline=pipeline,guard=guard,
                    run=lambda command,**unused:run_verified(pipeline,command,emit=lambda **row:progress.emit(**row)),
                    progress=lambda phase,message:progress.emit(phase,message),checkpoint=checkpoint,idle=idle,output=args.output,
                    protected=(Path(context['source_root'])/'tools',Path(context['source_root'])/'Monado_Windows/tools',
                               Path(context['source_root'])/'Monado_Windows/third_party',Path(context['python']).parent))
            if args.build_tools or args.verify_tools:
                progress.emit('complete','Native preparation tools verified; current model and upscaler are unchanged',status='success',
                    **{'fsr4_'+key:value[key] for key in ('tools','component','archives','support','cmake','ninja','compiler','vulkan_prefix')})
            else:
                fields=dict(fsr4_assets=value['asset_root'],fsr4_component=value['component'],fsr4_sdk=value['sdk'],fsr4_tools=value['tools'])
                fields.update({key:item for key,item in tool_fields.items() if key!='fsr4_component'})
                progress.emit('complete','FSR4 selection verified'+('; native synthetic GPU validation passed' if value['gpu_qualified'] else '; GPU rendering qualification remains pending'),
                    status='success',**fields)
            return 0
    except (OSError,ValueError,RuntimeError,KeyError,TypeError) as error:
        cancelled=pipeline is not None and isinstance(error,pipeline.Cancelled)
        if progress:progress.emit('cancel' if cancelled else 'error',str(error),status='cancelled' if cancelled else 'error')
        else:print(str(error),file=sys.stderr)
        return 2 if cancelled else 1
    finally:
        if progress:progress.close()

if __name__=='__main__':sys.exit(main())
