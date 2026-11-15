"""Current-only native Windows update cleanup, after checked activation only.

The trusted supervisor prepares a bounded ownership/content snapshot of its NEW
verified update transaction before activation. Cleanup consumes only those exact
snapshots, never arbitrary directories. Current/profile/bootstrap/cache roots,
unknown or modified files and busy sessions are preserved. A durable snapshot
also authorizes a safe retry after interrupted partial deletion. There is no
rollback selection, process termination or game/input traversal in this module.
"""
from __future__ import annotations

import ctypes as C
from ctypes import wintypes as W
import hashlib
import json
import msvcrt
import os
from pathlib import Path, PurePosixPath
import re

import windows_file_guard as guard

NAME = re.compile(r'windows-[0-9]{1,24}-[0-9a-f]{8}\Z')
BUILD = re.compile(r'native-[0-9]{1,24}-[0-9a-f]{8}\Z')
HASH = re.compile(r'[0-9a-f]{64}\Z')
MAX_NODES = 200_000
MAX_RECORD = 64 * 1024 * 1024
MAX_FILE = 512 * 1024 * 1024
MAX_DEPTH = 128
PREFIX = '.retention-'


class RetentionError(ValueError):
    pass


def _json(raw):
    if len(raw) > MAX_RECORD:
        raise RetentionError('Retention inventory exceeds its bound')
    def unique(pairs):
        out = {}
        for key, value in pairs:
            if key in out:
                raise RetentionError('Duplicate retention field')
            out[key] = value
        return out
    result = json.loads(raw, object_pairs_hook=unique)
    if not isinstance(result, dict):
        raise RetentionError('Invalid retention record')
    return result


def _encode(value):
    return (json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(',', ':'))+'\n').encode('utf-8')


def _relative(name):
    if not isinstance(name, str) or not name or '\\' in name or ':' in name:
        raise RetentionError('Invalid retention relative path')
    parts = name.split('/')
    if len(parts) > MAX_DEPTH:
        raise RetentionError('Retention path exceeds its depth bound')
    for part in parts:
        guard._component(part)
    return PurePosixPath(name)


def _canonical(path):
    return Path(guard.canonical_path(path))


def _scope(owner, candidate):
    owner, candidate = _canonical(owner), _canonical(candidate)
    if candidate.parent != owner/'updates' or not NAME.fullmatch(candidate.name):
        raise RetentionError('Cleanup target is outside its exact owned update directory')
    return owner, candidate


def _identity(identity, kind):
    # A directory's size/write time changes as its recognized children disappear.
    return [identity.volume, identity.file_id,
            identity.size if kind != 'd' else 0,
            identity.written if kind != 'd' else 0,
            identity.links if kind != 'd' else 0, identity.attributes]


def _junction(path, expected=None, delete=False):
    """Open only the reparse object itself; never open or delete its target."""
    access = guard._READ_ATTR | guard._READ_CONTROL | (guard._DELETE if delete else 0)
    handle = guard.K.CreateFileW(guard._native(str(path)), access, 3, None, 3,
                                guard._NOFOLLOW | guard._BACKUP, None)
    if handle == guard._INVALID:
        guard._failure('open exact input junction', path)
    try:
        info = guard._FileInfo()
        if not guard.K.GetFileInformationByHandle(handle, C.byref(info)):
            guard._failure('junction identity', path)
        if not info.attrs & guard._REPARSE or not info.attrs & guard._DIRECTORY:
            raise RetentionError('Expected the exact recorded directory junction')
        guard._owned(handle, str(path))
        api = guard.K.DeviceIoControl
        api.restype = W.BOOL
        api.argtypes = [W.HANDLE, W.DWORD, C.c_void_p, W.DWORD, C.c_void_p,
                        W.DWORD, C.POINTER(W.DWORD), C.c_void_p]
        data, got = C.create_string_buffer(16 * 1024), W.DWORD()
        if not api(handle, 0x900A8, None, 0, data, len(data), C.byref(got), None):
            guard._failure('read exact junction data', path)
        raw = data.raw[:got.value]
        if len(raw) < 16 or int.from_bytes(raw[:4], 'little') != 0xA0000003:
            raise RetentionError('Only recorded input mount-point junctions are eligible')
        identity = [info.volume, (info.index_high << 32) | info.index_low,
                    0, 0, 0, info.attrs]
        node = {'kind': 'j', 'identity': identity, 'sha256': hashlib.sha256(raw).hexdigest()}
        if expected is not None and node != expected:
            raise RetentionError('Recorded input junction changed; preserved')
        if delete:
            _dispose(handle, path)
        return node
    finally:
        guard.K.CloseHandle(handle)


def _file(directory, name):
    with directory.open_regular(name, max_bytes=MAX_FILE, owner=True) as stream:
        handle = msvcrt.get_osfhandle(stream.fileno())
        identity = guard._info(handle, str(Path(directory.path)/name), False)
        return {'kind': 'f', 'identity': _identity(identity, 'f'),
                'sha256': hashlib.file_digest(stream, 'sha256').hexdigest()}


def _walk(root, *, allow=None, junctions=(), planned=None):
    nodes = {}
    def visit(directory, prefix):
        if len(prefix.parts) > MAX_DEPTH:
            raise RetentionError('Retention tree exceeds its depth bound')
        key = prefix.as_posix()
        identity = guard._info(directory._handles[-1], directory.path, True)
        nodes[key] = {'kind': 'd', 'identity': _identity(identity, 'd')}
        with os.scandir(directory.path) as entries:
            children = []
            for entry in entries:
                if len(children) + len(nodes) >= MAX_NODES:
                    raise RetentionError('Retention directory exceeds its entry bound')
                children.append(entry)
            children.sort(key=lambda item: item.name.casefold())
        for entry in children:
            guard._component(entry.name)
            name = (prefix/entry.name).as_posix()
            if allow is not None and not allow(name):
                raise RetentionError('Unknown file in update transaction: '+name)
            if len(nodes) >= MAX_NODES:
                raise RetentionError('Retention tree exceeds its node bound')
            info = entry.stat(follow_symlinks=False)
            if getattr(info, 'st_file_attributes', 0) & guard._REPARSE:
                if name not in junctions:
                    raise RetentionError('Unrecognized reparse point in update transaction')
                nodes[name] = _junction(Path(directory.path)/entry.name)
            elif entry.is_dir(follow_symlinks=False):
                with directory.child(entry.name, owner=True) as child:
                    visit(child, prefix/entry.name)
            else:
                nodes[name] = _file(directory, entry.name)
            if planned is not None and nodes.get(name) != planned.get(name):
                raise RetentionError('Obsolete update acquired new or changed content: '+name)
    with guard.locked_directory(root, owner=True) as directory:
        visit(directory, PurePosixPath('.'))
    if planned is not None:
        for name, node in nodes.items():
            if node != planned.get(name):
                raise RetentionError('Obsolete update identity changed: '+name)
    return nodes


def prepare(owner, candidate, *, source_files, build_root, build_files, input_links, derived_roots=()):
    """Bind a newly verified source/build transaction before selecting it.

    Caller holds installation maintenance leases, has verified source BEFORE
    building, and verified the completed native pair/receipt/DLL closure AFTER.
    source_files includes the exact publication-manifest.json identity. build_files
    is the completed receipt's explicit recipe artifact inventory, augmented with
    the immutable build-receipt.json identity. Unknown profile files are ineligible.
    """
    owner, candidate = _scope(owner, candidate)
    build_root = _canonical(build_root)
    try:
        build = build_root.relative_to(candidate)
    except ValueError as error:
        raise RetentionError('Checked native output escaped its update transaction') from error
    if (build.parent.as_posix() != 'source/Monado_Windows/build' or
            not BUILD.fullmatch(build.name)):
        raise RetentionError('Unrecognized fresh native output directory')
    if not isinstance(source_files, dict) or not source_files or 'publication-manifest.json' not in source_files:
        raise RetentionError('Exact verified source inventory is required')
    sources = {}
    directories = {'.', 'source'}
    for name, identity in source_files.items():
        path = PurePosixPath('source')/_relative(name)
        if (not isinstance(identity, dict) or set(identity) != {'bytes', 'sha256'} or
                type(identity['bytes']) is not int or not 0 <= identity['bytes'] <= MAX_FILE or
                not isinstance(identity['sha256'], str) or not HASH.fullmatch(identity['sha256'])):
            raise RetentionError('Invalid verified source identity')
        sources[path.as_posix()] = identity
        directories.update(p.as_posix() for p in path.parents)
    directories.update(p.as_posix() for p in build.parents)
    if not isinstance(build_files, dict) or not {'build-receipt.json', 'build/xcde.exe', 'build/xcde_launcher.exe'} <= build_files.keys():
        raise RetentionError('Exact completed native recipe artifact inventory is required')
    for name, identity in build_files.items():
        path = PurePosixPath(build.as_posix())/_relative(name)
        if (not isinstance(identity, dict) or set(identity) != {'bytes', 'sha256'} or
                type(identity['bytes']) is not int or not 0 <= identity['bytes'] <= MAX_FILE or
                not isinstance(identity['sha256'], str) or not HASH.fullmatch(identity['sha256'])):
            raise RetentionError('Invalid native recipe artifact identity')
        sources[path.as_posix()] = identity
        directories.update(p.as_posix() for p in path.parents)
    marker = candidate/'source/Monado_Windows/build/active-windows-build.json'
    if marker.exists():
        raw = guard.read_regular(marker, max_bytes=8192, owner=True)
        if _json(raw) != {'root': str(build_root), 'receipt_sha256': build_files['build-receipt.json']['sha256']}:
            raise RetentionError('Candidate active build marker differs from its completed pair')
        sources[marker.relative_to(candidate).as_posix()] = {'bytes': len(raw), 'sha256': hashlib.sha256(raw).hexdigest()}
    derived = []
    for value in derived_roots:
        path = _canonical(value)
        if path != candidate/'source/Monado_Windows/.deps/zig-cache':
            raise RetentionError('Unrecognized compiler cache scope')
        relative = PurePosixPath(path.relative_to(candidate).as_posix())
        derived.append(relative)
        directories.update(p.as_posix() for p in relative.parents)
    links = {}
    if set(input_links) != {str(build_root/'targets/exefs'), str(build_root/'targets/romfs')}:
        raise RetentionError('Only the checked build input junction pair may be declared')
    for name, target in input_links.items():
        path = Path(name)
        if path.resolve() != Path(target).resolve():
            raise RetentionError('Checked input junction target differs')
        links[path.relative_to(candidate).as_posix()] = str(_canonical(target))
        directories.update(parent.as_posix() for parent in PurePosixPath(path.relative_to(candidate).as_posix()).parents)
    def allowed(name):
        path = PurePosixPath(name)
        return (name in sources or name in directories or name in links or
                name in {'source.zip', 'extraction.json', 'extraction-intent.json'} or
                any(path == cache or path.is_relative_to(cache) for cache in derived))
    with guard.locked_directory(owner, owner=True), guard.locked_directory(owner/'updates', owner=True) as updates:
        nodes = _walk(candidate, allow=allowed, junctions=links)
        for name, identity in sources.items():
            node = nodes.get(name)
            if (not node or node['kind'] != 'f' or node['sha256'] != identity['sha256'] or
                    node['identity'][2] != identity['bytes']):
                raise RetentionError('Verified source changed before activation: '+name)
        document = {'schema': 1, 'kind': 'native-windows-update-retention',
                    'owner': str(owner), 'candidate': candidate.name, 'nodes': nodes}
        raw = _encode(document)
        if len(raw) > MAX_RECORD:
            raise RetentionError('Retention inventory exceeds its bound')
        updates.atomic_write_new(PREFIX+candidate.name+'.json', raw, owner=True)
    return {'candidate': candidate.name, 'manifest_sha256': hashlib.sha256(raw).hexdigest()}


def _dispose(handle, path):
    disposition = W.BOOL(True)
    if not guard.K.SetFileInformationByHandle(handle, 4, C.byref(disposition), C.sizeof(disposition)):
        guard._failure('remove exact obsolete handle', path)


def _remove_node(path, expected):
    with guard.locked_directory(path.parent, owner=True):
        if expected['kind'] == 'j':
            _junction(path, expected=expected, delete=True)
            return
        directory = expected['kind'] == 'd'
        handle, identity = guard._open(str(path), directory=directory, owner=True,
            access=guard._READ_ATTR | guard._DELETE | (0 if directory else 0x80000000), share=guard._SHARE_READ)
        try:
            if _identity(identity, expected['kind']) != expected['identity']:
                raise RetentionError('Obsolete node changed before removal')
            if directory:
                _dispose(handle, path)
            else:
                fd = msvcrt.open_osfhandle(handle, os.O_RDONLY | os.O_BINARY)
                handle = None
                with os.fdopen(fd, 'rb') as stream:
                    if hashlib.file_digest(stream, 'sha256').hexdigest() != expected['sha256']:
                        raise RetentionError('Obsolete file changed before removal')
                    _dispose(msvcrt.get_osfhandle(stream.fileno()), path)
        finally:
            if handle is not None:
                guard.K.CloseHandle(handle)


def _remove_authority(updates, owner, name, expected_sha256):
    node = _file(updates, name)
    if node['sha256'] != expected_sha256:
        raise RetentionError('Cleanup authority changed before its removal; preserved')
    _remove_node(owner/'updates'/name, node)


def _record(raw, owner, name):
    record = _json(raw)
    if (set(record) != {'schema', 'kind', 'owner', 'candidate', 'nodes'} or
            type(record['schema']) is not int or record['schema'] != 1 or
            record['kind'] != 'native-windows-update-retention' or
            record['owner'] != str(owner) or not NAME.fullmatch(record.get('candidate', '')) or
            name != PREFIX+record['candidate']+'.json'):
        raise RetentionError('Unrecognized cleanup authority')
    nodes = record['nodes']
    if not isinstance(nodes, dict) or not 1 <= len(nodes) <= MAX_NODES or '.' not in nodes:
        raise RetentionError('Invalid cleanup node inventory')
    for path, node in nodes.items():
        if path != '.':
            _relative(path)
        if (not isinstance(node, dict) or node.get('kind') not in ('d', 'f', 'j') or
                set(node) != ({'kind', 'identity'} if node['kind'] == 'd' else {'kind', 'identity', 'sha256'}) or
                not isinstance(node['identity'], list) or len(node['identity']) != 6 or
                any(type(n) is not int or n < 0 for n in node['identity']) or
                (node['kind'] != 'd' and not HASH.fullmatch(node.get('sha256', '')))):
            raise RetentionError('Invalid cleanup node identity')
    if nodes['.']['kind'] != 'd':
        raise RetentionError('Cleanup root must be an ordinary directory')
    return record


def cleanup(owner, *, current, authorities, protected=(), idle, selection_unchanged):
    """Prune ONLY snapshotted obsolete candidates after successful activation.

    idle(candidate) raises for any checked live runtime/session. The caller's
    selection_unchanged callback must raise if its exact durable selection moved.
    Unsafe/changed/locked targets are reported pending, without closing processes.
    """
    owner, current = _scope(owner, current)
    if (not isinstance(authorities, dict) or len(authorities) > 256 or
            any(not isinstance(name, str) or not NAME.fullmatch(name) or
                not isinstance(sha, str) or not HASH.fullmatch(sha)
                for name, sha in authorities.items())):
        raise RetentionError('Cleanup requires durable registry-bound snapshot authorities')
    protected = tuple(_canonical(path) for path in protected) + (current,)
    report = {'schema': 1, 'status': 'complete', 'removed': [], 'pending': []}
    def pending(name, error):
        report['status'] = 'pending'
        report['pending'].append({'id': name, 'reason': str(error)[:512]})
    with guard.locked_directory(owner, owner=True), guard.locked_directory(owner/'updates', owner=True) as updates:
        for candidate_name, expected_sha256 in sorted(authorities.items()):
            name = PREFIX+candidate_name+'.json'
            try:
                try:
                    raw = updates.read_regular(name, max_bytes=MAX_RECORD, owner=True)
                except OSError as error:
                    if (error.errno in (2, 3) or getattr(error, 'winerror', None) in (2, 3)) and not (owner/'updates'/candidate_name).exists():
                        continue
                    raise
                if hashlib.sha256(raw).hexdigest() != expected_sha256:
                    raise RetentionError('Cleanup authority changed from its durable registry identity')
                record = _record(raw, owner, name)
                _, candidate = _scope(owner, owner/'updates'/record['candidate'])
                if any(candidate == p or candidate.is_relative_to(p) or p.is_relative_to(candidate) for p in protected):
                    continue
                selection_unchanged()
                if not candidate.exists():
                    _remove_authority(updates, owner, name, expected_sha256)
                    continue
                idle(candidate)
                planned = record['nodes']
                actual = _walk(candidate, junctions={n for n, v in planned.items() if v['kind'] == 'j'}, planned=planned)
                for relative in sorted(actual, key=lambda item: len(PurePosixPath(item).parts), reverse=True):
                    selection_unchanged()
                    path = candidate if relative == '.' else candidate/relative
                    _remove_node(path, actual[relative])
                report['removed'].append(candidate.name)
                _remove_authority(updates, owner, name, expected_sha256)
            except (OSError, ValueError) as error:
                pending(name, error)
        report['removed'] = report['removed'][-32:]
        report['pending'] = report['pending'][-32:]
        data = _encode(report)
        try:
            old = updates.read_regular('retention-status.json', owner=True)
        except OSError as error:
            if error.errno not in (2, 3) and getattr(error, 'winerror', None) not in (2, 3):
                raise
            updates.atomic_write_new('retention-status.json', data)
        else:
            updates.atomic_replace('retention-status.json', data, expected_sha256=hashlib.sha256(old).hexdigest())
    return report
