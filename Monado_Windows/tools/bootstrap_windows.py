#!/usr/bin/env python3
"""Trusted portable Windows builder backend. Never starts a game or a launcher.

The native frontend authenticates its embedded payload before starting this
persistent Python. This backend independently binds and verifies the manifests,
then stages reviewed source and runs only checked native build tools. Human
profiles, original build receipts and failed build outputs remain in place.
"""
from __future__ import annotations

import argparse
from contextlib import ExitStack, contextmanager
import hashlib
import importlib.util
import json
import os
from pathlib import Path, PurePosixPath
import queue
import re
import stat
import subprocess
import sys
import threading
import time
import uuid

SCHEMA = 1
PLATFORM = 'windows-amd64'
MAX_JSON = 4 * 1024 * 1024
MAX_PAYLOAD_MANIFEST = 16 * 1024 * 1024
MAX_SOURCE_FILE = 16 * 1024 * 1024
MAX_LOG = 64 * 1024 * 1024
HASH = re.compile(r'[0-9a-f]{64}\Z')
EXPECTED_NSO = {
    'main': '76ffa21b99c0385cfb14d1f90bbeddcdc98c17350b8edd79dfdbbb7341e70c48',
    'sdk': '31ce67e8701584ba812dceeae9b590d7a6ad642bcb5aa74d2fe3ea5c5474250d',
}
DEVICE = {'con','prn','aux','nul'} | {prefix+str(n) for prefix in ('com','lpt') for n in range(1,10)}


class BootstrapError(ValueError):
    pass


class Cancelled(BootstrapError):
    pass


class ChildFailure(BootstrapError):
    def __init__(self,message,exit_code):
        super().__init__(message)
        self.exit_code=exit_code


def encoded(value):
    return (json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True)+'\n').encode('utf-8')


def decode(raw,*,limit=MAX_JSON):
    if len(raw) > limit:
        raise BootstrapError('JSON record exceeds its bound')
    def unique(pairs):
        out = {}
        for key, value in pairs:
            if key in out:
                raise BootstrapError('Duplicate JSON key')
            out[key] = value
        return out
    try:
        out = json.loads(raw, object_pairs_hook=unique,
                         parse_constant=lambda value: (_ for _ in ()).throw(BootstrapError('Nonfinite JSON')))
    except (UnicodeError, ValueError, RecursionError) as error:
        raise BootstrapError('Malformed bounded JSON record') from error
    if not isinstance(out, dict):
        raise BootstrapError('JSON record must be an object')
    return out


def relative(name):
    if not isinstance(name, str) or not name or len(name) > 2048 or '\\' in name or ':' in name:
        raise BootstrapError('Invalid manifest path')
    pieces = name.split('/')
    if any(p in ('', '.', '..') or p != p.rstrip(' .') or
           p.split('.')[0].casefold() in DEVICE or any(ord(c) < 32 for c in p) for p in pieces):
        raise BootstrapError('Unsafe manifest path: '+name)
    return PurePosixPath(name)


def file_table(document, *, maximum=20000):
    files = document.get('files')
    if document.get('schema') != SCHEMA or document.get('platform') != PLATFORM or not isinstance(files, dict) or not 1 <= len(files) <= maximum:
        raise BootstrapError('Unsupported or empty Windows content manifest')
    aliases = set()
    for name, item in files.items():
        relative(name)
        folded = name.casefold()
        if folded in aliases:
            raise BootstrapError('Case-aliased manifest paths')
        aliases.add(folded)
        if not isinstance(item, dict) or set(item) != {'bytes','sha256'} or type(item['bytes']) is not int or not 0 <= item['bytes'] <= 512*1024*1024 or not isinstance(item['sha256'],str) or not HASH.fullmatch(item['sha256']):
            raise BootstrapError('Invalid content identity: '+name)
    if any('/'.join(PurePosixPath(n).parts[:i]).casefold() in aliases for n in files for i in range(1,len(PurePosixPath(n).parts))):
        raise BootstrapError('Manifest file/directory collision')
    return files


def plain(path, *, directory=False, owner=False):
    """Early bootstrap check; held native guards protect subsequent operations."""
    path = Path(path).absolute()
    if '..' in path.parts or path.resolve() != path:
        raise BootstrapError('Path must have ordinary canonical ancestors: '+str(path))
    current = Path(path.anchor)
    for component in path.parts[1:]:
        current /= component
        info = current.lstat()
        if stat.S_ISLNK(info.st_mode) or getattr(info, 'st_file_attributes', 0) & 0x400:
            raise BootstrapError('Filesystem links are not permitted here: '+str(current))
    info = path.stat()
    if directory != stat.S_ISDIR(info.st_mode) or (not directory and (not stat.S_ISREG(info.st_mode) or info.st_nlink != 1)):
        raise BootstrapError('Expected an ordinary '+('directory: ' if directory else 'file: ')+str(path))
    return path


def disjoint(*paths):
    canonical = [Path(p).resolve() for p in paths]
    for i, a in enumerate(canonical):
        for b in canonical[i+1:]:
            if a == b or a.is_relative_to(b) or b.is_relative_to(a):
                raise BootstrapError('Input, cache, source and installation must not overlap: '+str(a)+' / '+str(b))


def digest_stream(stream):
    return hashlib.file_digest(stream, 'sha256').hexdigest()


def read_bounded(path, limit=MAX_JSON):
    plain(path)
    with Path(path).open('rb') as stream:
        data = stream.read(limit+1)
    if len(data) > limit:
        raise BootstrapError('File exceeds its read bound: '+str(path))
    return data


def import_checked(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    if not spec or not spec.loader:
        raise BootstrapError('Cannot load checked authored tool')
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


@contextmanager
def parents(guard, parts, *, create=False):
    with ExitStack() as stack:
        for part in parts:
            guard = stack.enter_context(guard.child(part, create=create, owner=True))
        yield guard


def put_json(guard, name, document):
    data = encoded(document)
    before = optional_read(guard,name)
    if before is None:
        guard.atomic_write_new(name, data, owner=True)
    else:
        guard.atomic_replace(name, data, expected_sha256=hashlib.sha256(before).hexdigest(), owner=True)


def optional_read(guard,name,*,max_bytes=MAX_JSON):
    try:
        return guard.read_regular(name,max_bytes=max_bytes,owner=True)
    except OSError as error:
        # Native guard deliberately preserves Win32 error numbers in its own
        # OSError subclass; Windows PATH_NOT_FOUND is3, FILE_NOT_FOUND is2.
        if getattr(error,'winerror',None) in (2,3) or error.errno in (2,3): return None
        raise

def shader_status(raw,session,identity):
    if not raw or len(raw)>4096 or not raw.endswith(b'\n') or b'\x00' in raw:
        raise BootstrapError('Shader preparation did not publish a bounded current-session result')
    try:lines=raw.decode('ascii').splitlines()
    except UnicodeError as error:raise BootstrapError('Invalid shader preparation result encoding') from error
    if lines.pop(0)!='monado_precompile_work 1':raise BootstrapError('Unknown shader preparation result')
    fields={}
    for line in lines:
        parts=line.split(' ')
        if len(parts)!=2 or not parts[0] or not parts[1] or parts[0] in fields:raise BootstrapError('Invalid shader preparation result field')
        fields[parts[0]]=parts[1]
    counters=('files_total','files_done','shaders','programs','programs_done','seed_total','seed_done','variants_total','variants_done',
              'attempted','built','failed','skipped','unsupported','errors','gpu_vendor','gpu_device','gpu_driver')
    if (set(fields)!=set(counters)|{'session','identity','phase','state','gpu_cache_uuid'} or fields['session']!=session or
        fields['identity']!=identity or not re.fullmatch('[0-9a-f]{32}',session) or not HASH.fullmatch(identity) or
        not re.fullmatch('[0-9a-f]{32}',fields['gpu_cache_uuid'])):
        raise BootstrapError('Shader preparation result belongs to another session or runtime')
    for key in counters:
        if not re.fullmatch('[0-9]{1,20}',fields[key]) or int(fields[key])>0xffffffffffffffff:raise BootstrapError('Invalid shader preparation counter')
        fields[key]=int(fields[key])
    if (fields['phase']!='done' or fields['state']!='complete' or any(fields[key] for key in ('failed','skipped','errors')) or
        any(fields[done]!=fields[total] for done,total in (('files_done','files_total'),('programs_done','programs'),('seed_done','seed_total'),('variants_done','variants_total')))):
        raise BootstrapError('Shader preparation is incomplete; prior selected runtime and valid caches are preserved')
    return fields


class Progress:
    def __init__(self, root, path, log):
        self.path, self.log = Path(path), str(log)
        if self.path.parent != root or not self.path.name.endswith('.jsonl'):
            raise BootstrapError('Progress must be a fresh JSONL file directly inside cache/state')
        self.fd = os.open(self.path, os.O_CREAT | os.O_EXCL | os.O_WRONLY | getattr(os,'O_BINARY',0), 0o600)
        self.installation = None
    def emit(self, phase, message, *, done=0, total=0, status='running', **extra):
        record = {'phase':str(phase)[:64], 'done':max(0,int(done)), 'total':max(0,int(total)),
                  'message':str(message)[-2048:], 'status':status,
                  'installation':self.installation, 'log':self.log, **extra}
        line = (json.dumps(record,ensure_ascii=False,separators=(',',':'))+'\n').encode('utf-8')
        if len(line) > 65536:
            raise BootstrapError('Progress record exceeds its bound')
        for start in range(0,len(line),4096):
            data = memoryview(line)[start:start+4096]
            while data:
                written = os.write(self.fd,data)
                if written <= 0:
                    raise OSError('Short progress write')
                data = data[written:]
        os.fsync(self.fd)
    def close(self):
        os.close(self.fd)


class Backend:
    def __init__(self, args):
        self.args = args
        self.cache = plain(args.cache, directory=True)
        if not HASH.fullmatch(args.payload_sha256 or '') or self.cache.name != args.payload_sha256:
            raise BootstrapError('Persistent cache identity must match the authenticated payload SHA256')
        payload_raw = read_bounded(self.cache/'payload-manifest.json',MAX_PAYLOAD_MANIFEST)
        if not HASH.fullmatch(args.manifest_sha256 or '') or hashlib.sha256(payload_raw).hexdigest() != args.manifest_sha256:
            raise BootstrapError('Payload manifest does not match the native frontend trust anchor')
        self.payload = file_table(decode(payload_raw,limit=MAX_PAYLOAD_MANIFEST))
        required = ('setup/bootstrap_windows.py','setup/windows_file_guard.py','source-manifest.json',
                    'source/Monado_Windows/scripts/build_windows.py','source/Monado_Windows/games/xcde/profile.json',
                    'python/python.exe')
        if any(name not in self.payload for name in required):
            raise BootstrapError('Portable payload is missing a trusted tool/source component')
        self._bootstrap_check('setup/windows_file_guard.py')
        if hashlib.sha256(read_bounded(Path(__file__),MAX_SOURCE_FILE)).hexdigest()!=self.payload['setup/bootstrap_windows.py']['sha256']:
            raise BootstrapError('Executing backend differs from the authenticated payload')
        self.guard_module = import_checked(self.cache/'setup/windows_file_guard.py','windows_file_guard')
        self.stack = ExitStack()
        self.guard = self.stack.enter_context(self.guard_module.locked_directory(self.cache,owner=True))
        self.state_guard = self.stack.enter_context(self.guard.child('state',create=True,owner=True))
        self.stack.enter_context(self.guard_module.installation_lease(self.cache))
        state = self.cache/'state'
        self.cancel = Path(args.cancel_file).absolute()
        if self.cancel.parent != state or self.cancel.name in ('','.', '..'):
            raise BootstrapError('Cancellation file must be directly inside cache/state')
        self.log_path = state/('backend-'+uuid.uuid4().hex+'.log')
        self.log = self.log_path.open('xb')
        self.progress = Progress(state,Path(args.progress).absolute(),self.log_path)
        self.registry = {'schema':SCHEMA,'last':None,'installations':{}}
        self.driver = None
        self.verified = False

    def _bootstrap_check(self, name):
        item = self.payload[name]
        path = self.cache/name
        plain(path)
        with path.open('rb') as stream:
            if path.stat().st_size != item['bytes'] or digest_stream(stream) != item['sha256']:
                raise BootstrapError('Trusted bootstrap tool changed: '+name)

    def close(self):
        if hasattr(self,'log'):
            self.log.flush(); os.fsync(self.log.fileno())
            self.log.close()
        if hasattr(self,'progress'):
            self.progress.close()
        if hasattr(self,'stack'):
            self.stack.close()

    def cancelled(self):
        return optional_read(self.state_guard,self.cancel.name,max_bytes=1024) is not None

    def checkpoint(self):
        if self.cancelled():
            raise Cancelled('Build cancelled; checked installation and failed output remain available')

    def verify(self):
        self.progress.emit('verify','Checking persistent tools and reviewed source',total=len(self.payload))
        for index,(name,item) in enumerate(sorted(self.payload.items()),1):
            self.checkpoint()
            if name.split('/')[0] == 'state' or name == 'payload-manifest.json':
                raise BootstrapError('Manifest may not claim mutable state or itself')
            with self.guard.open_regular(name,max_bytes=item['bytes'],owner=True) as stream:
                if os.fstat(stream.fileno()).st_size != item['bytes'] or digest_stream(stream) != item['sha256']:
                    raise BootstrapError('Payload content changed: '+name)
            if index % 64 == 0 or index == len(self.payload):
                self.progress.emit('verify','Checking persistent tools and reviewed source',done=index,total=len(self.payload))
        self.source_raw = self.guard.read_regular('source-manifest.json',max_bytes=MAX_JSON,owner=True)
        self.source_manifest = decode(self.source_raw)
        self.source_files = file_table(self.source_manifest)
        for name,item in self.source_files.items():
            if self.payload.get('source/'+name) != item:
                raise BootstrapError('Reviewed source manifest is not bound to the payload: '+name)
        if {name[7:] for name in self.payload if name.startswith('source/')} != set(self.source_files):
            raise BootstrapError('Payload/source manifest membership differs')
        self.source_id = hashlib.sha256(self.source_raw).hexdigest()
        expected_dirs=set()
        for name in self.payload:
            parts=PurePosixPath(name).parts
            expected_dirs.update('/'.join(parts[:i]) for i in range(1,len(parts)))
        for directory,subdirs,leaves in os.walk(self.cache,followlinks=False):
            for name in list(subdirs):
                path=Path(directory)/name; rel=path.relative_to(self.cache).as_posix()
                plain(path,directory=True)
                if rel=='state': subdirs.remove(name); continue
                if rel not in expected_dirs: raise BootstrapError('Unmanifested immutable cache directory: '+rel)
            for name in leaves:
                rel=(Path(directory)/name).relative_to(self.cache).as_posix()
                if rel in ('.lock','.complete'):
                    # Native extraction creates these exact root markers and
                    # closes its .lock handle before starting this backend.
                    # They authorize no code and never bypass payload hashes.
                    marker=self.guard.read_regular(rel,max_bytes=64,owner=True)
                    expected=b'' if rel=='.lock' else self.args.payload_sha256.encode('ascii')
                    if marker!=expected:
                        raise BootstrapError('Native cache marker differs from its authenticated identity: '+rel)
                elif rel not in self.payload and rel!='payload-manifest.json':
                    raise BootstrapError('Unmanifested immutable cache file: '+rel)
        self.verify_source(self.cache/'source',self.source_files,allow_state=False)
        registered=optional_read(self.state_guard,'checked-installations.json')
        if registered is not None: self.registry=decode(registered)
        if self.registry.get('schema') != SCHEMA or not isinstance(self.registry.get('installations'),dict) or len(self.registry['installations']) > 256:
            raise BootstrapError('Checked-installation registry is invalid')
        # Frontends start -I -S: make only the fully authenticated ordinary
        # bundled packages available, without processing executable .pth files.
        packages=str(self.cache/'python/Lib/site-packages')
        if packages not in sys.path:sys.path.append(packages)
        self.driver = import_checked(self.cache/'source/Monado_Windows/scripts/build_windows.py','monado_trusted_build_driver')
        self.verified = True

    def verify_source(self, root, files, *, allow_state):
        plain(root,directory=True)
        with self.guard_module.locked_directory(root,owner=True) as guard:
            for name,item in files.items():
                with guard.open_regular(name,max_bytes=item['bytes'],owner=True) as stream:
                    if os.fstat(stream.fileno()).st_size != item['bytes'] or digest_stream(stream) != item['sha256']:
                        raise BootstrapError('Reviewed source changed: '+name)
        expected_dirs = {str(PurePosixPath(name).parent) for name in files}
        for name in files:
            parts = PurePosixPath(name).parts
            expected_dirs.update('/'.join(parts[:i]) for i in range(1,len(parts)))
        for directory,subdirs,leaves in os.walk(root,followlinks=False):
            for name in list(subdirs):
                path = Path(directory)/name; rel = path.relative_to(root).as_posix()
                if allow_state and rel in ('Monado_Windows/build','Monado_Windows/.deps'):
                    plain(path,directory=True); subdirs.remove(name); continue
                plain(path,directory=True)
                if rel not in expected_dirs:
                    raise BootstrapError('Unexpected source directory: '+rel)
            for name in leaves:
                rel = (Path(directory)/name).relative_to(root).as_posix()
                if rel not in files:
                    raise BootstrapError('Unexpected executable/source payload file: '+rel)

    def input_identity(self, selected, baseline=None):
        selected = Path(selected).absolute()
        if not selected.is_dir():
            raise BootstrapError('Selected extracted game folder is unavailable')
        # Only these two explicit read-only roots may be selected junctions.
        exefs,romfs = [(selected/name).resolve(strict=True) for name in ('exefs','romfs')]
        plain(exefs,directory=True); plain(romfs,directory=True)
        disjoint(self.cache,exefs,romfs)
        if self.args.destination:
            destination = Path(self.args.destination).resolve()
            disjoint(destination,exefs,romfs)
        profile = decode(self.guard.read_regular('source/Monado_Windows/games/xcde/profile.json',max_bytes=MAX_JSON,owner=True))
        identity = validate_input(exefs,romfs,profile,EXPECTED_NSO,checkpoint=self.checkpoint,
                                  progress=lambda n:self.progress.emit('input','Checking your extracted DE 2.0.0 files',done=n,total=profile['romfs_manifest']['files']))
        if baseline is not None and identity['exefs'] != baseline['exefs']:
            raise BootstrapError('The full exefs inventory differs from this installation; original selection unchanged')
        identity.update(path=str(selected),exefs_path=str(exefs),romfs_path=str(romfs))
        return identity

    def run_child(self, argv, *, cwd, phase, output=None,cancel_request=None):
        self.checkpoint()
        self.progress.emit(phase,'Starting checked native '+phase)
        guard = self.driver.OwnedProcesses()
        process = None
        reader = None
        stop_reader = threading.Event()
        messages = queue.Queue(maxsize=128)
        try:
            # The bundled interpreter and site-packages have already been
            # authenticated. User Python overrides must not redirect this
            # process or its nested generator/compiler Python children.
            environment = {k:v for k,v in os.environ.items() if not k.upper().startswith('PYTHON')}
            environment.update(PYTHONNOUSERSITE='1',PYTHONDONTWRITEBYTECODE='1',PYTHONUNBUFFERED='1',
                               PATH=str(self.cache/'native/ucrt64/bin')+os.pathsep+environment.get('PATH',''))
            process = subprocess.Popen([str(a) for a in argv],cwd=cwd,env=environment,
                                       stdin=subprocess.DEVNULL,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,
                                       creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
            guard.assign(process)
            def send(value):
                while not stop_reader.is_set():
                    try: messages.put(value,timeout=.1); return
                    except queue.Full: pass
            def consume():
                try:
                    while not stop_reader.is_set():
                        line = process.stdout.readline(16384)
                        if not line: break
                        send(line)
                finally:
                    send(None)
            reader = threading.Thread(target=consume,daemon=True); reader.start()
            ended=False; counts={}; logged=0; build_phase='runtime';cancel_started=None
            while not ended or process.poll() is None:
                if cancel_started is None:
                    try:self.checkpoint()
                    except Cancelled:
                        if cancel_request is None:raise
                        try:cancel_request()
                        except OSError as error:raise Cancelled('Could not publish graceful shader cancellation; stopping only the owned worker') from error
                        cancel_started=time.monotonic();self.progress.emit('shaders','Cancelling the owned shader preparation session')
                elif time.monotonic()-cancel_started>=10:
                    raise Cancelled('Shader preparation cancellation deadline reached; stopping only the owned worker')
                try: line=messages.get(timeout=.1)
                except queue.Empty: continue
                if line is None: ended=True; continue
                if logged < MAX_LOG:
                    chunk=line[:MAX_LOG-logged]; self.log.write(chunk); self.log.flush(); logged+=len(chunk)
                text=line.decode('utf-8',errors='replace').strip()
                if text.startswith('MONADO_FSR4_PROGRESS '):
                    worker=self.fsr4_modules()[0]
                    row=worker.progress_record(text)
                    self.progress.emit(row['phase'],row['message'],done=row['done'],total=row['total'])
                    continue
                if text.startswith('Split authored host'):
                    build_phase='launcher'
                if text.startswith('Compiled '):
                    current='game-code' if '/generated/' in text.replace('\\','/') else build_phase
                    counts[current]=counts.get(current,0)+1
                    total = len(list((output/'generated').rglob('*.c'))) if output and current=='game-code' else 0
                    self.progress.emit(current,text,done=counts[current],total=total)
                elif text:
                    self.progress.emit(phase,text)
            code=process.wait(); reader.join(timeout=2)
            if cancel_started is not None:raise Cancelled('Shader preparation cancelled; previous selected runtime and valid caches are preserved')
            if code:
                raise ChildFailure('Checked '+phase+' failed (exit '+str(code)+'); see '+str(self.log_path),code)
        finally:
            stop_reader.set()
            guard.close()
            if process is not None and process.poll() is None:
                try: process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill(); process.wait(timeout=10)
            if reader: reader.join(timeout=2)
            if process is not None and process.stdout is not None:
                process.stdout.close()

    def prepare_shaders(self,runtime,profile):
        runtime=Path(runtime);profile=Path(profile);session=uuid.uuid4().hex
        with self.guard_module.locked_directory(runtime,owner=True) as owned_runtime:
            with owned_runtime.open_regular('build/xcde.exe',max_bytes=512*1024*1024,owner=True) as executable:identity=digest_stream(executable)
        with self.guard_module.locked_directory(profile,owner=True) as owner,owner.child('cache',create=True,owner=True) as cache:
            def request():cache.atomic_write_new('precompile_cancel-'+session+'.txt',('monado_precompile_cancel 1\nsession '+session+'\n').encode('ascii'),owner=True)
            self.run_child([runtime/'build/xcde.exe','--precompile',profile/'targets','--session',session,'--identity',identity],
                           cwd=profile,phase='shaders',cancel_request=request)
            status=shader_status(cache.read_regular('precompile_work.txt',max_bytes=4096,owner=True),session,identity)
            self.progress.emit('shaders','Current-session shader caches committed; remaining unsupported variants compile during play',
                               done=status['built'],total=status['attempted'])

    def selected(self):
        destination = str(Path(self.args.destination).absolute()) if self.args.destination else None
        if destination:
            matches = [x for x in self.registry['installations'].values() if Path(x.get('destination','')) == Path(destination) or Path(x.get('root','')) == Path(destination)]
            if len(matches) != 1:
                raise BootstrapError('Installation is not in the checked registry; use Adopt first')
            return matches[0]
        key=self.registry.get('last')
        return self.registry['installations'].get(key) if key else None

    def require_idle(self,record,*,allow_settings=False):
        roots={Path(record['root']),Path(record.get('profile',record['root']))}
        roots.update(Path(p) for p in record.get('runtime_history',[]))
        profile=record.get('profile',record['root'])
        for item in self.registry['installations'].values():
            if Path(item.get('profile',item['root']))==Path(profile):
                roots.add(Path(item['root']));roots.update(Path(p) for p in item.get('runtime_history',[]))
        names=('xcde.exe',) if allow_settings else ('xcde.exe','xcde_launcher.exe','lasgame.dll')
        files=[root/'build'/name for root in roots for name in names if (root/'build'/name).is_file()]
        if busy_resources(files):
            raise BootstrapError('This installation has an open game or settings session. Close it manually before changing or reopening this profile; nothing was closed.')

    def save_selection(self, record):
        # A failed durable publish must leave both the on-disk and in-memory
        # selection unchanged, so Locate can restore its original junctions.
        updated=dict(self.registry,installations=dict(self.registry['installations']),last=record['id'])
        updated['installations'][record['id']]=record
        put_json(self.state_guard,'checked-installations.json',updated)
        self.registry=updated

    def fsr4_modules(self):
        result=[]
        for leaf,name in (('fsr4_prepare_worker_windows.py','monado_trusted_fsr4_worker'),
                          ('setup_windows_fsr4.py','setup_windows_fsr4'),
                          ('fsr4_process_windows.py','fsr4_process_windows'),
                          ('setup_windows_fsr4_tools.py','setup_windows_fsr4_tools'),
                          ('prepare_windows_fsr4.py','monado_trusted_fsr4_pipeline')):
            relative='source/Monado_Windows/tools/'+leaf
            if relative not in self.payload:raise BootstrapError('This checked builder lacks the native FSR4 preparation pipeline')
            module=sys.modules.get(name)
            if module is None or Path(module.__file__)!=self.cache/relative:
                module=import_checked(self.cache/relative,name)
            if name in ('monado_trusted_fsr4_worker','monado_trusted_fsr4_pipeline'):result.append(module)
        return result

    def launcher_arguments(self,profile):
        arguments=['--profile-root',str(profile)]
        with self.guard_module.locked_directory(profile,owner=True) as owner:
            raw=optional_read(owner,'fsr4-preparation.json',max_bytes=32768)
        if raw is None:return arguments
        try:
            worker,pipeline=self.fsr4_modules()
            selection=worker.verify_selection(profile,pipeline=pipeline,guard=self.guard_module)
            if selection:arguments+=['--fsr4-assets',selection['asset_root'],'--fsr4-component',selection['component']]
        except (OSError,ValueError,RuntimeError,KeyError,TypeError) as error:
            self.progress.emit('fsr4-unavailable','Prepared FSR4 selection is unavailable; ordinary settings and fallback remain available')
            self.log.write(('FSR4 selection unavailable: '+str(error)+'\n').encode('utf-8',errors='replace'));self.log.flush()
        return arguments

    def fsr4_fields(self,profile):
        with self.guard_module.locked_directory(profile,owner=True) as owner:
            raw=optional_read(owner,'fsr4-preparation.json',max_bytes=32768)
            tools_raw=optional_read(owner,'fsr4-tools.json',max_bytes=32768)
        if raw is None and tools_raw is None:return {}
        try:
            worker,_=self.fsr4_modules();fields={}
            if raw is not None:
                value=worker.document(raw)
                if worker.known_selection(value):fields.update(fsr4_sdk=value['sdk'],fsr4_tools=value['tools'],fsr4_component=value['component'])
            if tools_raw is not None:
                value=worker.document(tools_raw)
                if worker.known_tools_selection(value):fields.update({'fsr4_'+key:value[key] for key in ('tools','component','archives','support','cmake','ninja','compiler','vulkan_prefix')})
            return fields
        except (OSError,ValueError,RuntimeError,KeyError,TypeError):return {}

    def build_fsr4_tools(self):
        record=self.selected()
        if not record:raise BootstrapError('Choose a checked installation before building optional FSR4 tools')
        for name in ('archives','support','cmake','ninja','compiler'):
            if not getattr(self.args,name,None):raise BootstrapError('Choose explicit local archives, D3D support, CMake, Ninja and compiler')
        self.require_idle(record,allow_settings=True);self.checked_build(record['root'],record['receipt_sha256'])
        profile=Path(record.get('profile',record['root']));self.checked_build(profile,record['profile_receipt_sha256'])
        worker,pipeline=self.fsr4_modules();self.progress.installation=record['destination']
        try:
            worker.build_tools(installation=record['destination'],profile=profile,archives=self.args.archives,support=self.args.support,
                cmake=self.args.cmake,ninja=self.args.ninja,compiler=self.args.compiler,
                vulkan_prefix=getattr(self.args,'vulkan_prefix',None) or self.cache/'native/ucrt64',jobs=self.args.jobs,
                tools_module=pipeline.tools,component_module=pipeline.component,component_verifier=pipeline.verify_component,guard=self.guard_module,
                progress=lambda phase,message,**extra:self.progress.emit(phase,message,**extra),checkpoint=self.checkpoint,
                idle=lambda:self.require_idle(record,allow_settings=True),protected=(self.cache,record['input']['exefs_path'],record['input']['romfs_path']),
                log=lambda text:(self.log.write(text.encode('utf-8',errors='replace')),self.log.flush()))
        except pipeline.Cancelled as error:raise Cancelled(str(error)) from error
        self.progress.emit('complete','Native preparation tools verified; current model and upscaler are unchanged',status='success',profile=str(profile),**self.fsr4_fields(profile))

    def prepare_fsr4(self,select=False):
        record=self.selected()
        if not record:raise BootstrapError('Choose a checked installation before preparing FSR4')
        component=getattr(self.args,'component',None)
        if not component:raise BootstrapError('Choose your explicit local native FSR4 component')
        self.require_idle(record,allow_settings=True)
        self.checked_build(record['root'],record['receipt_sha256'])
        profile=Path(record.get('profile',record['root']))
        self.checked_build(profile,record['profile_receipt_sha256'])
        worker,pipeline=self.fsr4_modules();self.progress.installation=record['destination']
        idle=lambda:self.require_idle(record,allow_settings=True)
        if select:
            assets=getattr(self.args,'assets',None)
            if not assets:raise BootstrapError('Choose an existing prepared FSR4 model folder')
            selection=worker.activate(profile,assets=assets,component=component,pipeline=pipeline,guard=self.guard_module,
                            checkpoint=self.checkpoint,idle=idle)
        else:
            sdk=getattr(self.args,'sdk',None);tools=getattr(self.args,'tools',None)
            if not sdk or not tools:raise BootstrapError('Choose your local SDK file and checked native preparation tools')
            def run_pipeline(argv,**unused):
                return worker.run_verified(pipeline,argv,emit=lambda **row:self.progress.emit(**row),
                    log=lambda text:(self.log.write(text.encode('utf-8',errors='replace')),self.log.flush()))
            try:
                selection=worker.prepare(installation=record['destination'],profile=profile,sdk=sdk,tools=tools,component=component,
                    python=self.cache/'python/python.exe',script=self.cache/'source/Monado_Windows/tools/prepare_windows_fsr4.py',
                    cancel_file=self.cancel,pipeline=pipeline,guard=self.guard_module,run=run_pipeline,
                    progress=lambda phase,message:self.progress.emit(phase,message),checkpoint=self.checkpoint,idle=idle,
                    protected=(self.cache,record['input']['exefs_path'],record['input']['romfs_path']))
            except pipeline.Cancelled as error:raise Cancelled(str(error)) from error
        message='FSR4 assets verified and selected'+('; native synthetic GPU validation passed' if selection['gpu_qualified'] else '; GPU rendering qualification remains pending')
        self.progress.emit('complete',message,status='success',profile=str(profile),**self.fsr4_fields(profile))

    def checked_build(self, root, receipt_hash=None):
        root=plain(root,directory=True)
        with self.guard_module.locked_directory(root,owner=True) as guard:
            raw=guard.read_regular('build-receipt.json',max_bytes=MAX_JSON,owner=True)
            if receipt_hash and hashlib.sha256(raw).hexdigest()!=receipt_hash:
                raise BootstrapError('Original checked build receipt changed')
            receipt=decode(raw)
            if receipt.get('schema')!=1 or receipt.get('status')!='success' or not receipt.get('installation'):
                raise BootstrapError('This folder has no completed native build receipt; choose a fresh installation folder')
            outputs=receipt.get('outputs')
            if not isinstance(outputs,dict) or not 2 <= len(outputs) <= 256:
                raise BootstrapError('Invalid completed build output inventory')
            for name,sha in outputs.items():
                relative(name)
                if not isinstance(sha,str) or not HASH.fullmatch(sha):
                    raise BootstrapError('Invalid completed binary identity')
                with guard.open_regular(name,max_bytes=512*1024*1024,owner=True) as stream:
                    if digest_stream(stream)!=sha:
                        raise BootstrapError('Checked installation binary changed: '+name)
            if any(n not in outputs for n in ('build/xcde.exe','build/xcde_launcher.exe')):
                raise BootstrapError('Completed receipt lacks the native game/settings pair')
            images=[root/'build/xcde.exe',root/'build/xcde_launcher.exe']
            if receipt.get('split_game'):
                if 'build/lasgame.dll' not in outputs:
                    raise BootstrapError('Completed split installation lacks its local game library')
                images.append(root/'build/lasgame.dll')
            self.driver.dll_closure(images,root/'build',flat=True,own_images=[images[0]])
        return receipt,hashlib.sha256(raw).hexdigest()

    def incompatible_reuse(self,output,identity):
        """Only the trusted driver's exact typed failure permits regeneration.

        Missing/corrupt receipts, DLLs, ownership errors, compiler failures and
        all other failed builds propagate. The helper validates the complete
        previous library BEFORE classifying an actual compatibility mismatch.
        """
        with self.guard_module.locked_directory(output,owner=True) as guard:
            record=decode(guard.read_regular('build-receipt.json',max_bytes=MAX_JSON,owner=True))
        return (type(record.get('schema')) is int and record['schema']==SCHEMA and
                record.get('implementation')=='windows-parity-development' and
                record.get('status')=='failed' and
                record.get('failure_kind')=='incompatible-game-library' and
                record.get('input',{}).get('exefs')=={n:identity['exefs'][n] for n in EXPECTED_NSO})

    def preserve_failed_build(self,output,destination):
        # Keep failed evidence outside the successful update transaction so
        # current-only retention never acquires authority over a failed build.
        output=plain(output,directory=True)
        with self.guard_module.locked_directory(output.parent,owner=True), self.guard_module.locked_directory(destination,owner=True) as owner:
            with owner.child('failed-builds',create=True,owner=True) as failures:
                saved=Path(failures.path)/('reuse-'+output.name)
                if saved.exists():raise BootstrapError('Failed-build evidence destination already exists')
                output.rename(saved)
        self.progress.emit('reuse','Preserved incompatible reuse attempt: '+str(saved))

    def stage_source(self,destination):
        destination=Path(destination).absolute()
        if destination.exists(): plain(destination,directory=True)
        else:
            plain(destination.parent,directory=True)
            with self.guard_module.locked_directory(destination.parent,owner=True) as parent:
                with parent.child(destination.name,create=True,owner=True): pass
        disjoint(destination,self.cache)
        self.stack.enter_context(self.guard_module.installation_lease(destination))
        if self.args.input:
            disjoint(destination,Path(self.args.input).resolve())
        for entry in self.registry['installations'].values():
            old=Path(entry['destination']).resolve()
            if destination != old:
                disjoint(destination,old)
        with self.guard_module.locked_directory(destination,owner=True) as guard:
            raw=optional_read(guard,'monado-installation.json')
            if raw is None:
                if any(destination.iterdir()):
                    raise BootstrapError('Chosen installation folder is not empty; existing human files are preserved')
                marker={'schema':SCHEMA,'id':uuid.uuid4().hex,'source_id':self.source_id}
                guard.atomic_write_new('monado-installation.json',encoded(marker),owner=True)
            else: marker=decode(raw)
            if marker.get('schema')!=SCHEMA or marker.get('source_id')!=self.source_id or not re.fullmatch('[0-9a-f]{32}',marker.get('id','')):
                raise BootstrapError('Installation source identity differs; use the checked update flow or a new empty folder')
            old=optional_read(guard,'source-manifest.json')
            if old is None: guard.write_new('source-manifest.json',self.source_raw,owner=True)
            else:
                if old!=self.source_raw: raise BootstrapError('Installation source manifest changed')
            with guard.child('source',create=True,owner=True) as source:
                for index,(name,item) in enumerate(sorted(self.source_files.items()),1):
                    self.checkpoint()
                    with parents(source,PurePosixPath(name).parts[:-1],create=True) as parent:
                        leaf=PurePosixPath(name).name
                        existing=optional_read(parent,leaf,max_bytes=MAX_SOURCE_FILE)
                        if existing is None:
                            data=self.guard.read_regular('source/'+name,max_bytes=MAX_SOURCE_FILE,owner=True)
                            parent.write_new(leaf,data,owner=True)
                        else:
                            if len(existing)!=item['bytes'] or hashlib.sha256(existing).hexdigest()!=item['sha256']:
                                raise BootstrapError('Existing source changed; preserved without overwrite: '+name)
                    if index%64==0 or index==len(self.source_files):
                        self.progress.emit('source','Preparing reviewed local source',done=index,total=len(self.source_files))
        self.verify_source(destination/'source',self.source_files,allow_state=True)
        return destination,marker

    def build(self):
        if not self.args.input or not self.args.destination:
            raise BootstrapError('Build needs your extracted input and a chosen installation folder')
        identity=self.input_identity(self.args.input)
        destination,marker=self.stage_source(self.args.destination)
        port=destination/'source/Monado_Windows'
        output=port/'build'/('native-'+str(time.time_ns())+'-'+uuid.uuid4().hex[:8])
        self.stack.enter_context(self.guard_module.locked_directory(destination,owner=True))
        self.progress.installation=str(destination)
        self.run_child([self.cache/'python/python.exe','-B',port/'scripts/build_windows.py','--stage','all',
                        '--split-game','--input',identity['path'],'--output',output,'--prefix',self.cache/'native/ucrt64',
                        '--jobs',str(self.args.jobs),'--preparation-python-manifest',self.cache/'payload-manifest.json',
                        '--preparation-python-manifest-sha256',self.args.manifest_sha256,
                        '--preparation-installation',destination],cwd=port,phase='build',output=output)
        receipt,receipt_hash=self.checked_build(output)
        self.known_targets(output,receipt,identity)
        if receipt.get('input',{}).get('exefs')!={n:{'sha256':identity['exefs'][n]['sha256'],'bytes':identity['exefs'][n]['bytes']} for n in EXPECTED_NSO}:
            raise BootstrapError('Completed build input identity changed')
        previous=self.registry['installations'].get(marker['id'])
        profile=previous.get('profile',previous['root']) if previous else str(output)
        if previous and identity['exefs']!=previous['input']['exefs']:
            raise BootstrapError('Existing profile requires its original full exefs inventory; use a new empty folder')
        if previous and any(Path(identity[k])!=Path(previous['input'][k]) for k in ('exefs_path','romfs_path')):
            raise BootstrapError('Locate the moved original input before rebuilding this existing profile')
        record={'schema':SCHEMA,'id':marker['id'],'destination':str(destination),'root':str(output),'profile':profile,
                'source_root':str(port),'source_id':self.source_id,'receipt_sha256':receipt_hash,
                'profile_receipt_sha256':previous['profile_receipt_sha256'] if previous else receipt_hash,
                'runtime_history':list(dict.fromkeys(previous.get('runtime_history',[])+[previous['root']]))[-128:] if previous else [],
                'input':identity,'managed':True,'version':self.source_manifest.get('version','0.0.0')}
        if self.args.precompile:
            if previous:self.require_idle(previous)
            self.prepare_shaders(output,Path(profile))
        self.checkpoint(); self.checked_build(output,receipt_hash)
        if previous: self.require_idle(previous)
        self.save_selection(record)
        self.progress.emit('complete','Native game and settings launcher built and checked',status='success',
                           launcher=str(output/'build/xcde_launcher.exe'),launcher_args=['--profile-root',profile],profile=profile)

    def adopt(self):
        if not self.args.destination:
            raise BootstrapError('Adopt needs the existing completed native build folder')
        root=plain(self.args.destination,directory=True)
        disjoint(root,self.cache)
        receipt,receipt_hash=self.checked_build(root)
        original=receipt.get('input',{}).get('path')
        if not isinstance(original,str) or not Path(original).is_dir():
            raise BootstrapError('Original input is unavailable and this legacy receipt has no full exefs inventory. Restore the ORIGINAL input first, or build into a new empty folder. Existing profile and receipt are unchanged.')
        identity=self.input_identity(original)
        if receipt.get('input',{}).get('exefs')!={n:identity['exefs'][n] for n in EXPECTED_NSO}:
            raise BootstrapError('Original input differs from the completed native build receipt')
        self.known_targets(root,receipt,identity)
        record={'schema':SCHEMA,'id':uuid.uuid4().hex,'destination':str(root),'root':str(root),'profile':str(root),
                'source_root':None,'source_id':None,'receipt_sha256':receipt_hash,'input':identity,
                'profile_receipt_sha256':receipt_hash,
                'runtime_history':[],
                'managed':False,'version':'0.0.0'}
        self.require_idle(record)
        self.save_selection(record); self.progress.installation=str(root)
        self.progress.emit('complete','Existing native installation checked; settings are ready',status='success',
                           launcher=str(root/'build/xcde_launcher.exe'),launcher_args=self.launcher_arguments(root),profile=str(root),**self.fsr4_fields(root))

    def known_targets(self,root,receipt,identity,*,missing=False):
        target=root/'targets'; plain(target,directory=True)
        self.stack.enter_context(self.guard_module.locked_directory(target,owner=True))
        declared=receipt['installation'].get('input_links',{})
        if set(declared)!= {'exefs','romfs'}:
            raise BootstrapError('Original receipt does not declare the exact owned input junctions')
        for name in ('exefs','romfs'):
            original=receipt.get('input',{}).get('path')
            if not isinstance(original,str) or not isinstance(declared[name],str) or Path(declared[name]).absolute()!=Path(original).absolute()/name:
                raise BootstrapError('Original input junction declaration is not scoped to its recorded input')
            link=target/name
            if not link.is_junction():
                raise BootstrapError('Expected an original input junction; human directories are preserved: '+str(link))
            expected=Path(identity[name+'_path'])
            if link.resolve()!=expected:
                raise BootstrapError('Input junction target differs from checked selection: '+str(link))
            if not missing and not expected.is_dir():
                raise BootstrapError('Checked extracted input is unavailable')

    def resume(self):
        record=self.selected()
        if not record:
            self.progress.emit('setup','Choose your extracted game folder and installation location',status='needs-setup'); return
        self.progress.installation=record['destination']
        self.require_idle(record)
        receipt,_=self.checked_build(record['root'],record['receipt_sha256'])
        identity=record['input']
        profile=Path(record.get('profile',record['root']))
        profile_receipt,_=self.checked_build(profile,record['profile_receipt_sha256'])
        self.known_targets(profile,profile_receipt,identity,missing=True)
        if not Path(identity['path']).is_dir() or not Path(identity['exefs_path']).is_dir() or not Path(identity['romfs_path']).is_dir():
            self.progress.emit('input','Extracted input moved. Locate the same original files to continue.',status='needs-input'); return
        current=self.input_identity(identity['path'],identity)
        if current['romfs']!=identity['romfs']:
            raise BootstrapError('Original extracted RomFS inventory changed')
        self.save_selection(record)
        self.progress.emit('complete','Checked native settings launcher is ready',status='success',
                           launcher=str(Path(record['root'])/'build/xcde_launcher.exe'),launcher_args=self.launcher_arguments(profile),profile=str(profile),**self.fsr4_fields(profile))

    def locate(self):
        record=self.selected()
        if not record or not self.args.input:
            raise BootstrapError('Locate needs a checked installation and the moved original extracted folder')
        self.require_idle(record)
        receipt,_=self.checked_build(record['root'],record['receipt_sha256'])
        root=Path(record.get('profile',record['root']))
        self.stack.enter_context(self.guard_module.installation_lease(root))
        profile_receipt,_=self.checked_build(root,record['profile_receipt_sha256'])
        self.known_targets(root,profile_receipt,record['input'],missing=True)
        identity=self.input_identity(self.args.input,record['input'])
        if identity['romfs']!=record['input']['romfs']:
            raise BootstrapError('Relocated RomFS inventory differs; original selection unchanged')
        # Only the two explicitly receipt-owned junction names are replaced.
        # Both replacements are prepared first, then each old junction is moved
        # aside. Any failure restores the original names; no input is removed.
        import _winapi
        token=uuid.uuid4().hex; changes=[]
        try:
            for name in ('exefs','romfs'):
                pending=root/'targets'/('.'+name+'-'+token)
                _winapi.CreateJunction(identity[name+'_path'],str(pending))
                changes.append((root/'targets'/name,pending,root/'targets'/('.old-'+name+'-'+token)))
            self.checkpoint()
            for original,pending,backup in changes:
                original.rename(backup)
                try: pending.rename(original)
                except BaseException: backup.rename(original); raise
            updated=dict(record,input=identity)
            self.save_selection(updated)
        except BaseException:
            for original,pending,backup in reversed(changes):
                if backup.is_junction():
                    if original.is_junction(): original.rmdir()
                    backup.rename(original)
                if pending.is_junction(): pending.rmdir()
            raise
        else:
            for _,_,backup in changes:
                if backup.is_junction(): backup.rmdir()
        self.progress.installation=record['destination']
        self.progress.emit('complete','Moved original input checked and located; settings are ready',status='success',
                           launcher=str(Path(record['root'])/'build/xcde_launcher.exe'),launcher_args=['--profile-root',str(root)],profile=str(root))

    def update(self,execute=False):
        record=self.selected()
        if not record:
            raise BootstrapError('Select a checked native installation before checking updates')
        if 'setup/update_windows_release.py' not in self.payload:
            raise BootstrapError('This checked builder has no Windows update verifier; no update was attempted')
        for name in ('publication_guard','update_release'):
            if 'setup/'+name+'.py' not in self.payload:
                raise BootstrapError('Checked update dependency is missing: '+name)
            import_checked(self.cache/'setup'/(name+'.py'),name)
        updater=import_checked(self.cache/'setup/update_windows_release.py','monado_trusted_windows_updater')
        offer=updater.discover(record.get('version','0.0.0'))
        self.progress.installation=record['destination']
        if not offer:
            self.progress.emit('update','No newer supported Windows release is available',status='up-to-date'); return
        if not execute:
            self.progress.emit('update','A checked Windows update is available: '+offer['version'],status='update-available'); return
        self.require_idle(record)
        previous_receipt,_=self.checked_build(record['root'],record['receipt_sha256'])
        profile=Path(record.get('profile',record['root'])); profile_receipt,_=self.checked_build(profile,record['profile_receipt_sha256'])
        self.known_targets(profile,profile_receipt,record['input'])
        identity=self.input_identity(record['input']['path'],record['input'])
        # A fresh verified source/build lives outside the stable profile. Only
        # the small owned selection registry changes at activation; existing
        # binaries, receipts, settings, saves and unknown profile files stay.
        destination=plain(record['destination'],directory=True)
        self.stack.enter_context(self.guard_module.installation_lease(destination))
        self.stack.enter_context(self.guard_module.locked_directory(destination,owner=True))
        with self.guard_module.locked_directory(destination,owner=True) as installation:
            with installation.child('updates',create=True,owner=True): pass
        candidate=destination/'updates'/('windows-'+str(time.time_ns())+'-'+uuid.uuid4().hex[:8])
        try:
            source=updater.fetch_and_extract(offer,candidate,
                        progress=lambda phase,done,total:self.progress.emit(phase,'Checking Windows source update',done=done,total=total),
                        cancel=self.cancelled)
        except updater.UpdateCancelled as error:
            raise Cancelled(str(error)) from error
        manifest=updater.verified_source(source,offer)  # Installed guards; candidate code is still not imported.
        # Retention authority is bound to the verified source BEFORE any
        # candidate script executes. Its later native snapshot checks these
        # same identities again, including the publication manifest itself.
        if 'setup/update_windows_retention.py' not in self.payload:
            raise BootstrapError('This checked builder lacks the native update retention guard; current selection is unchanged')
        retention=import_checked(self.cache/'setup/update_windows_retention.py','monado_trusted_windows_retention')
        source_files=dict(manifest['files'])
        with self.guard_module.locked_directory(source,owner=True) as source_guard:
            publication=source_guard.read_regular('publication-manifest.json',max_bytes=MAX_JSON,owner=True)
        if hashlib.sha256(publication).hexdigest()!=offer['source_manifest_sha256']:
            raise BootstrapError('Verified update publication identity changed before building')
        source_files['publication-manifest.json']={'bytes':len(publication),'sha256':hashlib.sha256(publication).hexdigest()}
        port=source/'Monado_Windows'
        if not (port/'scripts/build_windows.py').is_file():
            raise BootstrapError('Verified Windows update lacks the native builder entry')
        output=port/'build'/('native-'+str(time.time_ns())+'-'+uuid.uuid4().hex[:8])
        def compile_update(reuse):
            argv=[self.cache/'python/python.exe','-B',port/'scripts/build_windows.py','--stage','all','--split-game',
                  '--input',identity['path'],'--output',output,'--prefix',self.cache/'native/ucrt64','--jobs',str(self.args.jobs),
                  '--preparation-python-manifest',self.cache/'payload-manifest.json',
                  '--preparation-python-manifest-sha256',self.args.manifest_sha256,'--preparation-installation',destination]
            if reuse:argv+=['--reuse-game-library',record['root']]
            self.run_child(argv,cwd=port,phase='update-build',output=output)
        reuse=bool(previous_receipt.get('split_game'))
        try:
            compile_update(reuse)
        except ChildFailure as error:
            if not reuse or error.exit_code!=3 or not self.incompatible_reuse(output,identity):raise
            self.preserve_failed_build(output,destination)
            self.progress.emit('reuse','Game library compatibility changed; regenerating your local game library')
            self.checkpoint();self.checked_build(record['root'],record['receipt_sha256'])
            output=port/'build'/('native-'+str(time.time_ns())+'-'+uuid.uuid4().hex[:8])
            compile_update(False)
        receipt,receipt_hash=self.checked_build(output)
        self.known_targets(output,receipt,identity)
        if receipt.get('input',{}).get('exefs')!={n:identity['exefs'][n] for n in EXPECTED_NSO}:
            raise BootstrapError('Completed update input differs from the checked original inventory')
        if self.args.precompile:
            self.require_idle(record)
            self.prepare_shaders(output,profile)
        result=dict(record,root=str(output),source_root=str(port),source_id=offer['source_manifest_sha256'],
                    receipt_sha256=receipt_hash,version=offer['version'],
                    runtime_history=list(dict.fromkeys(record.get('runtime_history',[])+[record['root']]))[-128:])
        self.checkpoint(); self.checked_build(output,receipt_hash); self.require_idle(record)
        self.progress.emit('retention','Checking the completed update inventory before activation')
        build_files=dict(receipt.get('retention_files',{}))
        with self.guard_module.locked_directory(output,owner=True) as completed:
            receipt_raw=completed.read_regular('build-receipt.json',max_bytes=MAX_JSON,owner=True)
        if hashlib.sha256(receipt_raw).hexdigest()!=receipt_hash:
            raise BootstrapError('Completed update receipt changed before activation')
        build_files['build-receipt.json']={'bytes':len(receipt_raw),'sha256':receipt_hash}
        token=retention.prepare(destination,candidate,source_files=source_files,build_root=output,build_files=build_files,
                    input_links={str(output/'targets'/n):identity[n+'_path'] for n in ('exefs','romfs')},
                    derived_roots=(port/'.deps/zig-cache',))
        result['retention']=token
        authorities=dict(record.get('retention_authorities',{}))
        previous_token=record.get('retention')
        if previous_token:
            authorities[previous_token['candidate']]=previous_token['manifest_sha256']
        authorities[token['candidate']]=token['manifest_sha256']
        if len(authorities)>256:
            raise BootstrapError('Too many pending update cleanup records; current selection is unchanged')
        result['retention_authorities']=authorities
        self.save_selection(result)
        # Activation is now complete. Cleanup is best effort: locks, changed
        # data, a new session or an interrupted removal retain old files and
        # cannot turn the successful new selection into a failed update.
        selected=encoded(self.registry)
        def selection_unchanged():
            if self.state_guard.read_regular('checked-installations.json',max_bytes=MAX_JSON,owner=True)!=selected:
                raise BootstrapError('Selected installation changed during obsolete-update cleanup')
        message='Windows update built and checked; existing profile preserved'
        try:
            self.progress.emit('cleanup','Checking obsolete verified update files')
            report=retention.cleanup(destination,current=candidate,
                    protected=(profile,self.cache),idle=lambda obsolete:self.require_idle(result),
                    selection_unchanged=selection_unchanged,authorities=authorities)
            if report.get('status')=='pending':
                message+='; some obsolete files remain pending cleanup'
            removed=set(report.get('removed',[]))
            if removed:
                selection_unchanged()
                compact=dict(result,retention_authorities={k:v for k,v in authorities.items() if k not in removed})
                self.save_selection(compact)
        except (OSError,ValueError,RuntimeError) as error:
            self.log.write(('Deferred obsolete-update cleanup: '+str(error)+'\n').encode('utf-8',errors='replace'));self.log.flush()
            message+='; obsolete-file cleanup is deferred'
        self.progress.emit('complete',message,status='success',
                           launcher=str(output/'build/xcde_launcher.exe'),launcher_args=['--profile-root',str(profile)],profile=str(profile))


def busy_resources(paths):
    """Read-only exact executable identities, including long Unicode paths.

Restart Manager can fail with ERROR_WRITE_FAULT on otherwise valid long build
paths. Toolhelp + QueryFullProcessImageName avoids that resource-IPC limitation.
Only an exact checked executable path means busy; a matching basename whose
identity cannot be queried is refused explicitly instead of guessed idle.
"""
    if not paths: return False
    import ctypes as C
    from ctypes import wintypes as W
    class Entry(C.Structure):
        _fields_=[('size',W.DWORD),('usage',W.DWORD),('pid',W.DWORD),('heap',C.c_size_t),
                  ('module',W.DWORD),('threads',W.DWORD),('parent',W.DWORD),('priority',W.LONG),
                  ('flags',W.DWORD),('exe',W.WCHAR*260)]
    api=C.WinDLL('kernel32',use_last_error=True)
    api.CreateToolhelp32Snapshot.argtypes=[W.DWORD,W.DWORD];api.CreateToolhelp32Snapshot.restype=W.HANDLE
    api.Process32FirstW.argtypes=[W.HANDLE,C.POINTER(Entry)]
    api.Process32NextW.argtypes=[W.HANDLE,C.POINTER(Entry)]
    api.OpenProcess.argtypes=[W.DWORD,W.BOOL,W.DWORD];api.OpenProcess.restype=W.HANDLE
    api.QueryFullProcessImageNameW.argtypes=[W.HANDLE,W.DWORD,W.LPWSTR,C.POINTER(W.DWORD)]
    api.CloseHandle.argtypes=[W.HANDLE]
    def normalized(value):
        if value.startswith('\\\\?\\UNC\\'):value='\\\\'+value[8:]
        elif value.startswith('\\\\?\\'):value=value[4:]
        return str(Path(value).absolute()).casefold()
    wanted={normalized(str(p)) for p in paths if Path(p).suffix.lower()=='.exe'}
    names={Path(p).name.casefold() for p in wanted}
    snapshot=api.CreateToolhelp32Snapshot(2,0)
    if snapshot==C.c_void_p(-1).value:
        raise BootstrapError('Cannot inventory checked native process identities')
    try:
        entry=Entry();entry.size=C.sizeof(entry)
        present=api.Process32FirstW(snapshot,C.byref(entry))
        while present:
            if entry.exe.casefold() in names:
                handle=api.OpenProcess(0x1000,False,entry.pid)
                if not handle:
                    # The process can exit between snapshot and open.
                    if C.get_last_error()!=87:raise BootstrapError('Cannot identify an existing game/settings process; close it manually before continuing')
                else:
                    try:
                        image=C.create_unicode_buffer(32768);length=W.DWORD(len(image))
                        if not api.QueryFullProcessImageNameW(handle,0,image,C.byref(length)):
                            raise BootstrapError('Cannot query an existing game/settings executable identity')
                        if normalized(image.value) in wanted:return True
                    finally:api.CloseHandle(handle)
            present=api.Process32NextW(snapshot,C.byref(entry))
        if C.get_last_error()!=18:raise BootstrapError('Native process identity inventory was interrupted')
        return False
    finally:
        api.CloseHandle(snapshot)


def validate_input(exefs,romfs,profile,expected_nso,*,checkpoint=lambda:None,progress=lambda n:None):
    """Read-only exact exefs hashes and expected RomFS names/size aggregates."""
    inventory={}
    for directory,subdirs,files in os.walk(exefs,followlinks=False):
        if len(Path(directory).relative_to(exefs).parts)>8: raise BootstrapError('Extracted exefs depth exceeds its bound')
        for name in subdirs: plain(Path(directory)/name,directory=True)
        for name in sorted(files):
            checkpoint(); path=plain(Path(directory)/name)
            if len(inventory)>=256: raise BootstrapError('Extracted exefs inventory exceeds its bound')
            with path.open('rb') as stream:
                inventory[path.relative_to(exefs).as_posix()]={'bytes':path.stat().st_size,'sha256':digest_stream(stream)}
    if any(name not in inventory for name in profile['exefs_required']):
        raise BootstrapError('Required extracted exefs files are missing')
    for name,sha in expected_nso.items():
        if inventory[name]['sha256']!=sha: raise BootstrapError('Unsupported extracted DE 2.0.0 '+name+' SHA256')
    for module in profile['modules']:
        with (exefs/module['name']).open('rb') as stream: header=stream.read(0x48)
        if len(header)<0x48 or header[:4]!=b'NSO0' or header[0x40:0x48].hex()!=module['build_id'][:16]:
            raise BootstrapError('Unsupported extracted NSO build identity: '+module['name'])
    lines=[]; dirs={}; count=0
    for directory,subdirs,files in os.walk(romfs,followlinks=False):
        if len(Path(directory).relative_to(romfs).parts)>64: raise BootstrapError('RomFS directory depth exceeds its bound')
        for name in subdirs: plain(Path(directory)/name,directory=True)
        for name in files:
            checkpoint(); path=plain(Path(directory)/name); rel=path.relative_to(romfs).as_posix(); size=path.stat().st_size
            top=rel.split('/',1)[0] if '/' in rel else '.'; bucket=dirs.setdefault(top,[0,0]); bucket[0]+=1; bucket[1]+=size
            lines.append(f'{rel}\t{size}'); count+=1
            if count>100000: raise BootstrapError('RomFS file inventory exceeds its bound')
            if count%256==0: progress(count)
    got={'dirs':dict(sorted(dirs.items())),'files':count,'bytes':sum(v[1] for v in dirs.values()),
         'sha256':hashlib.sha256('\n'.join(sorted(lines)).encode('utf-8')).hexdigest()}
    if got!=profile['romfs_manifest']:
        raise BootstrapError('Extracted RomFS does not match the accepted DE 2.0.0 inventory')
    progress(count)
    return {'exefs':inventory,'romfs':got}


def main(argv=None):
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--action',required=True,choices=('build','resume','adopt','locate-input','check-update','update','prepare-fsr4','select-fsr4','build-fsr4-tools'))
    parser.add_argument('--cache',required=True,type=Path)
    parser.add_argument('--payload-sha256',required=True)
    parser.add_argument('--manifest-sha256',required=True)
    parser.add_argument('--input',type=Path)
    parser.add_argument('--destination',type=Path)
    parser.add_argument('--jobs',type=int,choices=range(1,7),default=4)
    parser.add_argument('--progress',required=True,type=Path)
    parser.add_argument('--cancel-file',required=True,type=Path)
    parser.add_argument('--precompile',action='store_true')
    parser.add_argument('--sdk',type=Path)
    parser.add_argument('--tools',type=Path)
    parser.add_argument('--component',type=Path)
    parser.add_argument('--assets',type=Path)
    for name in ('archives','support','cmake','ninja','compiler','vulkan-prefix'):parser.add_argument('--'+name,type=Path)
    args=parser.parse_args(argv)
    backend=None
    try:
        if os.name!='nt': raise BootstrapError('This builder requires native Windows')
        backend=Backend(args); backend.verify(); backend.checkpoint()
        {'build':backend.build,'resume':backend.resume,'adopt':backend.adopt,'locate-input':backend.locate,
         'check-update':backend.update,'update':lambda:backend.update(True),
         'prepare-fsr4':backend.prepare_fsr4,'select-fsr4':lambda:backend.prepare_fsr4(True),'build-fsr4-tools':backend.build_fsr4_tools}[args.action]()
        return 0
    except Cancelled as error:
        if backend: backend.progress.emit('cancel',str(error),status='cancelled')
        else: print(str(error),file=sys.stderr)
        return 2
    except (OSError,ValueError,RuntimeError,KeyError,TypeError) as error:
        if backend: backend.progress.emit('error',str(error),status='error')
        else: print(str(error),file=sys.stderr)
        return 1
    finally:
        if backend: backend.close()


if __name__=='__main__':
    sys.exit(main())
