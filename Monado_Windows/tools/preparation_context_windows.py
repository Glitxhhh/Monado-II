"""Build-authored, pinned native FSR4 worker context; never starts a process."""
from __future__ import annotations
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import sys


def module(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec)
    sys.modules[name] = result
    spec.loader.exec_module(result)
    return result


def create(port, output, *, installation, manifest=None, manifest_sha256=None):
    port, output = Path(port), Path(output)
    python = Path(sys.executable).absolute()
    home = python.parent
    value = dict(schema=1, platform='windows-amd64', status='unavailable', reason='',
                 installation=str(installation), receipt=str(output/'build-receipt.json'),
                 source_root=str(port.parent), python=str(python), python_site=str(home/'Lib/site-packages'),
                 worker=str(port/'tools/fsr4_prepare_worker_windows.py'),
                 pipeline=str(port/'tools/prepare_windows_fsr4.py'),
                 guard=str(port.parent/'tools/windows_file_guard.py'), files={})
    try:
        publication_path = port.parent/'publication-manifest.json'
        publication = publication_path.read_bytes()
        if len(publication)>16*1024*1024:
            raise ValueError('Reviewed source manifest exceeds its bound')
        reviewed = json.loads(publication)['files']
        guard_path = port.parent/'tools/windows_file_guard.py'
        guard_identity = reviewed.get('tools/windows_file_guard.py')
        if (not isinstance(guard_identity,dict) or guard_path.is_symlink() or guard_path.is_junction() or
                guard_path.stat().st_size!=guard_identity['bytes'] or hashlib.sha256(guard_path.read_bytes()).hexdigest()!=guard_identity['sha256']):
            raise ValueError('Native guard differs from the reviewed source manifest')
        guard = module(port.parent/'tools/windows_file_guard.py', 'monado_preparation_context_guard')
        if manifest is not None:
            with guard.locked_directory(Path(manifest).parent, owner=True) as parent:
                raw = parent.read_regular(Path(manifest).name, max_bytes=16*1024*1024, owner=True)
            if hashlib.sha256(raw).hexdigest() != manifest_sha256:
                raise ValueError('Pinned Python payload manifest hash differs')
            payload = json.loads(raw)
            if payload.get('schema') != 1 or payload.get('platform') != 'windows-amd64':
                raise ValueError('Unrecognized Python payload manifest')
            if Path(manifest).parent/'python' != home:
                raise ValueError('Python interpreter differs from its payload home')
            python_files = {name[7:]: item for name, item in payload['files'].items() if name.startswith('python/')}
        else:
            # Source fallback accepts only the exact official archives used by
            # the portable executable, with installed members matching them.
            if home != port/'.deps/python314':
                raise ValueError('Use the portable setup or the pinned project-local CPython runtime')
            pack = module(port/'tools/build_windows_builder.py', 'monado_preparation_pins')
            members = pack.Members()
            archives = pack.python_archives(port/'.deps/python-archives', members)
            try:
                python_files = {}
                for name, member in members.items():
                    data = member.read()
                    python_files[name[7:]] = {'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
            finally:
                for archive in archives:
                    archive.close()
        if 'python.exe' not in python_files or 'python314.zip' not in python_files or not python_files:
            raise ValueError('Pinned Python executable or standard-library closure is missing')
        files = {}
        with guard.locked_directory(home, owner=True) as root:
            for name, item in python_files.items():
                with root.open_regular(name, max_bytes=item['bytes'], owner=True) as stream:
                    if stream.seek(0, 2) != item['bytes']:
                        raise ValueError('Pinned Python member size differs: '+name)
                    stream.seek(0)
                    if hashlib.file_digest(stream, 'sha256').hexdigest() != item['sha256']:
                        raise ValueError('Pinned Python member hash differs: '+name)
                files[str(home/name)] = item
            # Reject executable/import additions, including old bytecode, rather
            # than bless an ambient Python installation by hashing its contents.
            actual = {p.relative_to(home).as_posix() for p in home.rglob('*') if p.is_file()}
            if actual != set(python_files):
                raise ValueError('Pinned Python contains extra or missing files; use a fresh portable cache')
        sources = {name:item for name,item in reviewed.items() if
            ((name.startswith(('Monado_Windows/tools/','tools/')) and name.endswith('.py')) or
             name.startswith(('Monado_Windows/third_party/fsr4cap/','Monado_Windows/third_party/fsr4_native/')) or
             name.startswith('Monado_Windows/fsr4') and name.endswith('.json'))}
        for name,item in list(sources.items()):
            if not name.startswith('Monado_Windows/fsr4') or not name.endswith('.json'):
                continue
            with guard.locked_directory(port,owner=True) as source:
                raw = source.read_regular(Path(name).name,max_bytes=1024*1024,owner=True)
            if len(raw)!=item['bytes'] or hashlib.sha256(raw).hexdigest()!=item['sha256']:
                raise ValueError('Preparation source lock differs from its reviewed identity')
            for relative,identity in json.loads(raw).get('files',{}).items():
                full = 'Monado_Windows/'+relative
                if reviewed.get(full)!=identity:
                    raise ValueError('Preparation source lock is outside reviewed source: '+relative)
                sources[full]=identity
        for name,item in sorted(sources.items()):
            path = port.parent/name
            with guard.locked_directory(path.parent, owner=True) as parent:
                with parent.open_regular(path.name, max_bytes=item['bytes'], owner=True) as stream:
                    stream.seek(0, 2); size = stream.tell(); stream.seek(0)
                    identity = {'bytes': size, 'sha256': hashlib.file_digest(stream, 'sha256').hexdigest()}
                    if identity != item:
                        raise ValueError('Preparation source differs from its reviewed identity: '+name)
                    files[str(path)] = identity
        files[str(publication_path)] = {'bytes': len(publication), 'sha256': hashlib.sha256(publication).hexdigest()}
        for import_root in (port/'tools',port.parent/'tools'):
            count = 0
            for directory,children,names in os.walk(import_root,followlinks=False):
                count += len(children)+len(names)
                if count>20000:
                    raise ValueError('Authored import membership exceeds its bound')
                if '__pycache__' in children:
                    raise ValueError('Authored bytecode cache is not reviewed; use a clean source export')
                with guard.locked_directory(directory,owner=True):
                    for child in children:
                        with guard.locked_directory(Path(directory)/child,owner=True):
                            pass
                    for leaf in names:
                        path = Path(directory)/leaf
                        if path.suffix.lower() in ('.py','.pyw','.pyc','.pyd','.dll','.pth') and str(path) not in files:
                            raise ValueError('Unreviewed authored import member preserved: '+path.name)
        if any(value[key] not in files for key in ('python', 'worker', 'pipeline', 'guard')):
            raise ValueError('Native preparation worker closure is incomplete')
        value.update(status='available', files=files)
    except (OSError, ValueError, KeyError, TypeError, ImportError) as error:
        value.update(status='unavailable', reason='Optional FSR4 preparation is unavailable: '+str(error)[:1024], files={})
    return value
