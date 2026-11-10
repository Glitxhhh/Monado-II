"""[XCDE] LINUX-042: inspect a declared native runtime without executing it.

Owned SONAMEs must come from their locked private paths. Host libraries are an
explicit interface boundary supplied by the caller, never an ambient fallback.
The loader/driver/plugin implementation behind that boundary is not pinned by
this graph; callers also perform a trusted loader-only check in the same search
environment. This is not a relocation, GPU, codec or gameplay test.
"""
from __future__ import annotations

from collections import deque
import hashlib
from pathlib import Path, PurePosixPath
import re
import stat
import struct
import subprocess


SONAME = re.compile(r'[A-Za-z0-9_+.-]+\.so(?:\.[A-Za-z0-9_+.-]+)*\Z')
READELF = '/usr/bin/readelf'
MAX_OBJECTS = 4096


def _native_name(name):
    if not isinstance(name, str) or not SONAME.fullmatch(name):
        raise ValueError('invalid runtime SONAME: ' + repr(name))
    return name


def _relative(name):
    if (not isinstance(name, str) or not name or '\\' in name or ':' in name
            or any(ord(char) < 32 for char in name)
            or name != '/'.join(PurePosixPath(name).parts)
            or PurePosixPath(name).is_absolute()
            or any(part in ('.', '..') for part in PurePosixPath(name).parts)):
        raise ValueError('invalid private runtime path: ' + repr(name))
    return name


def _readelf(path, flag):
    result = subprocess.run([READELF, flag, str(path)], capture_output=True,
                            text=True, timeout=30,
                            env={'PATH': '/usr/bin:/bin', 'LANG': 'C', 'LC_ALL': 'C'})
    if result.returncode or len(result.stdout) > 8 * 1024 * 1024:
        raise ValueError('cannot inspect native ELF metadata: ' + str(path))
    return result.stdout


def inspect_elf(path):
    """Return native ELF dependencies and exact version needs/definitions.

    Symlinks are resolved by the caller's declared provider path. No loader or
    constructors are invoked. A changed file is refused rather than cached.
    """
    path = Path(path).resolve(strict=True)
    before = path.stat()
    if not stat.S_ISREG(before.st_mode):
        raise ValueError('runtime provider must be an ordinary file')
    with path.open('rb') as stream:
        header = stream.read(64)
        stream.seek(0)
        identity = hashlib.file_digest(stream, 'sha256').hexdigest()
    if (len(header) != 64 or header[:6] != b'\x7fELF\x02\x01'
            or struct.unpack_from('<H', header, 16)[0] not in (2, 3)
            or struct.unpack_from('<H', header, 18)[0] != 62):
        raise ValueError('runtime provider is not native Linux x86-64 ELF: ' + str(path))
    dynamic = _readelf(path, '-dW')
    program = _readelf(path, '-lW')
    versions = _readelf(path, '-VW')
    needed = re.findall(r'\(NEEDED\).*\[([^\]]+)\]', dynamic)
    for name in needed:
        _native_name(name)
    sonames = re.findall(r'\(SONAME\).*\[([^\]]+)\]', dynamic)
    if len(sonames) > 1:
        raise ValueError('runtime provider has multiple SONAMEs')
    soname = _native_name(sonames[0]) if sonames else None
    required, weak, defined = {}, {}, []
    section, library = None, None
    for line in versions.splitlines():
        if line.startswith('Version needs section'):
            section = 'need'
        elif line.startswith('Version definition section'):
            section = 'define'
        elif line.startswith('Version symbols section'):
            section = None
        if section == 'need':
            match = re.search(r'\bFile: (\S+)\s+Cnt:', line)
            if match:
                library = _native_name(match[1])
            match = re.search(r'\bName: (\S+)\s+Flags: (.*?)\s+Version:', line)
            if match:
                if library not in needed:
                    raise ValueError('ELF version requirement has no dependency')
                target = weak if 'WEAK' in match[2].split() else required
                target.setdefault(library, []).append(match[1])
        elif section == 'define':
            match = re.search(r'\bName: (\S+)\s*$', line)
            if match:
                defined.append(match[1])
    interpreters = re.findall(r'\[Requesting program interpreter: ([^\]]+)\]', program)
    if len(interpreters) > 1:
        raise ValueError('runtime provider has multiple interpreters')
    after = path.stat()
    snapshot = lambda s: (s.st_dev, s.st_ino, s.st_size, s.st_mtime_ns, s.st_ctime_ns)
    if snapshot(before) != snapshot(after):
        raise ValueError('runtime provider changed while inspecting: ' + str(path))
    return {'path': str(path), 'bytes': before.st_size, 'sha256': identity,
            'elf_class': 64, 'machine': 62, 'endian': 'little',
            'needed': needed, 'soname': soname,
            'required_versions': {k: sorted(set(v)) for k, v in required.items()},
            'weak_required_versions': {k: sorted(set(v)) for k, v in weak.items()},
            'defined_versions': sorted(set(defined)),
            'interpreter': interpreters[0] if interpreters else None,
            'runpath': re.findall(r'\(RUNPATH\).*\[([^\]]*)\]', dynamic),
            'rpath': re.findall(r'\(RPATH\).*\[([^\]]*)\]', dynamic)}


def audit_closure(roots, sysroot, policy, host_paths=None):
    """Audit declared roots and private providers; report every found gap.

    Explicit root DSOs can provide their own exact SONAME to another root.
    Host paths must be selected independently by the caller's trusted discovery;
    the process environment, ldconfig cache and arbitrary directories are never
    searched here. Native host interfaces end the owned traversal deliberately.
    """
    if not isinstance(policy, dict) or policy.get('schema') != 1:
        raise ValueError('unsupported runtime policy schema')
    sysroot = Path(sysroot).resolve(strict=True)
    owned, hosts = policy.get('owned_sonames'), policy.get('host_sonames')
    if not isinstance(owned, dict) or not isinstance(hosts, dict) or set(owned) & set(hosts):
        raise ValueError('runtime providers must have disjoint explicit policies')
    for name, meta in owned.items():
        _native_name(name)
        if not isinstance(meta, dict):
            raise ValueError('invalid owned runtime provider record')
        _relative(meta.get('path'))
    for name, meta in hosts.items():
        _native_name(name)
        if not isinstance(meta, dict) or not meta.get('reason') or not meta.get('category'):
            raise ValueError('host runtime boundary must state its reason/category')
    for directory in policy.get('library_dirs', []):
        _relative(directory)
    host_paths = host_paths or {}
    if set(host_paths) - set(hosts):
        raise ValueError('undeclared host runtime provider')
    errors, edges, objects, host_requirements = [], [], {}, {}
    extra, metadata = {}, {}

    def inspect(path):
        path = Path(path).resolve(strict=True)
        if path not in metadata:
            metadata[path] = inspect_elf(path)
        return metadata[path]

    queue = deque()
    for root in roots:
        try:
            root = Path(root).resolve(strict=True)
            item = inspect(root)
        except (OSError, ValueError, subprocess.SubprocessError) as error:
            errors.append({'kind': 'root_elf', 'path': str(root), 'detail': str(error)})
            continue
        name = item['soname'] or (root.name if SONAME.fullmatch(root.name) else None)
        if name:
            if name in hosts or name in extra and extra[name] != root:
                raise ValueError('conflicting explicit root SONAME: ' + name)
            if name in owned:
                pinned = (sysroot / owned[name]['path']).resolve(strict=True)
                if not pinned.is_relative_to(sysroot) or not root.is_relative_to(sysroot):
                    raise ValueError('owned root provider escapes its private sysroot: ' + name)
                if pinned != root:
                    raise ValueError('root shadows locked private SONAME: ' + name)
            extra[name] = root
        queue.append((root, 'root', [str(root)]))
    while queue:
        path, scope, chain = queue.popleft()
        if path in objects:
            continue
        if len(objects) >= MAX_OBJECTS:
            errors.append({'kind': 'graph_limit', 'chain': chain})
            break
        try:
            item = inspect(path)
        except (OSError, ValueError, subprocess.SubprocessError) as error:
            errors.append({'kind': 'provider_elf', 'path': str(path), 'chain': chain,
                           'detail': str(error)})
            continue
        objects[path] = dict(item, scope=scope)
        for name in item['needed']:
            provider, provider_scope = None, None
            next_chain = chain + [name]
            if name in extra:
                provider, provider_scope = extra[name], 'root'
            elif name in owned:
                candidate = sysroot / owned[name]['path']
                try:
                    provider = candidate.resolve(strict=True)
                    if not provider.is_relative_to(sysroot):
                        raise ValueError('private runtime provider escapes its sysroot')
                    provider_scope = 'owned'
                except (OSError, ValueError) as error:
                    provider = None
                    errors.append({'kind': 'owned_missing', 'needed': name,
                                   'chain': next_chain, 'detail': str(error)})
            elif name in hosts:
                try:
                    if name not in host_paths:
                        raise ValueError('required native host interface is unavailable')
                    provider = Path(host_paths[name]).resolve(strict=True)
                    if provider.is_relative_to(sysroot):
                        raise ValueError('native host interface must not be replaced inside the private sysroot')
                    provider_scope = 'host'
                except (OSError, ValueError) as error:
                    provider = None
                    errors.append({'kind': 'host_missing', 'needed': name,
                                   'chain': next_chain, 'detail': str(error)})
            else:
                errors.append({'kind': 'undeclared_dependency', 'needed': name,
                               'chain': next_chain})
            edge = {'requester': str(path), 'needed': name,
                    'provider': str(provider) if provider else None,
                    'scope': provider_scope, 'chain': next_chain}
            edges.append(edge)
            if provider is None:
                continue
            try:
                target = inspect(provider)
                if target['soname'] != name and not (provider_scope == 'root' and target['soname'] is None and provider.name == name):
                    raise ValueError('provider SONAME differs from requested ABI')
                missing = sorted(set(item['required_versions'].get(name, []))
                                 - set(target['defined_versions']))
                if missing:
                    errors.append({'kind': 'version_missing', 'needed': name,
                                   'missing_versions': missing, 'chain': next_chain,
                                   'provider': str(provider)})
                if provider_scope == 'host':
                    host_requirements.setdefault(name, {'path': str(provider),
                        'sha256': target['sha256'], 'reason': hosts[name]['reason'],
                        'category': hosts[name]['category'], 'required_versions': []})
                    current = host_requirements[name]['required_versions']
                    current[:] = sorted(set(current) | set(item['required_versions'].get(name, [])))
                else:
                    queue.append((provider, provider_scope, next_chain))
            except (OSError, ValueError, subprocess.SubprocessError) as error:
                errors.append({'kind': 'provider_abi', 'needed': name,
                               'chain': next_chain, 'provider': str(provider), 'detail': str(error)})
    return {'schema': 1, 'passed': not errors, 'errors': errors,
            'objects': [objects[p] for p in sorted(objects)], 'edges': edges,
            'host_requirements': host_requirements,
            'scope': 'Static declared private ELF closure and exact native host ABI needs; '
                     'host implementation, dlopen plugins, relocations, GPU and gameplay excluded.'}
