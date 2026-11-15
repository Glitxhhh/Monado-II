#!/usr/bin/env python3
"""LINUX-099: receipt-bound, state-private explicit observer layer manifest.

No layer/driver code is loaded here. Only our fixed name and verified local
resource can enter Vulkan's explicit-layer search path. A failed optional
counter setup does not disable the user's separately selected Smooth Motion.
"""
import hashlib
import json
import os
from pathlib import Path
import stat

RESOURCE = 'libmonado-present-observer.so'
LAYER = 'VK_LAYER_MONADO_present_observer'
DIRECTORY = 'present-observer-layer'
OWNER = b'{"owner":"Monado II private presentation observer","schema":1}\n'


def checked_resource(binary, receipt):
    """The caller has checked all resource hashes; independently bind this ABI."""
    row = receipt.get('runtime_resources', {}).get(RESOURCE)
    abi = receipt.get('present_observer', {})
    if (not isinstance(row, dict) or abi.get('abi') != 1 or
            abi.get('generated_classification') is not False):
        return None
    path = Path(binary).parent / RESOURCE
    try:
        before = path.lstat()
        if (not stat.S_ISREG(before.st_mode) or before.st_nlink != 1 or
                before.st_uid != os.getuid() or path.resolve() != path or
                row.get('path') != str(path) or row.get('bytes') != before.st_size or
                not 64 <= before.st_size <= 16777216):
            return None
        fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC)
        with os.fdopen(fd, 'rb') as stream:
            opened = os.fstat(stream.fileno())
            header = stream.read(20); stream.seek(0)
            digest = hashlib.file_digest(stream, 'sha256').hexdigest()
            after = os.fstat(stream.fileno())
        final = path.lstat()
        identity = lambda s: (s.st_dev, s.st_ino, s.st_size, s.st_mtime_ns, s.st_ctime_ns)
        if (identity(before) != identity(opened) or identity(opened) != identity(after) or
                identity(after) != identity(final) or path.resolve() != path or
                not stat.S_ISREG(final.st_mode) or final.st_nlink != 1 or final.st_uid != os.getuid() or
                header[:6] != b'\x7fELF\x02\x01' or int.from_bytes(header[18:20], 'little') != 62 or
                row.get('sha256') != digest):
            return None
        return str(path)
    except (OSError, ValueError, TypeError):
        return None


def _directory(parent, name):
    try:
        os.mkdir(name, 0o700, dir_fd=parent)
    except FileExistsError:
        pass
    fd = os.open(name, os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC | os.O_NOFOLLOW, dir_fd=parent)
    s = os.fstat(fd)
    if s.st_uid != os.getuid() or stat.S_IMODE(s.st_mode) != 0o700:
        os.close(fd)
        raise ValueError('observer directory ownership or mode differs')
    return fd


def _read(fd, name, limit):
    child = os.open(name, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=fd)
    try:
        before = os.fstat(child)
        if (not stat.S_ISREG(before.st_mode) or before.st_uid != os.getuid() or before.st_nlink != 1 or
                stat.S_IMODE(before.st_mode) != 0o600 or before.st_size > limit):
            raise ValueError('observer manifest is not an ordinary owned private file')
        data = os.read(child, limit + 1); after = os.fstat(child)
        if len(data) > limit or (before.st_size, before.st_mtime_ns, before.st_ctime_ns) != (after.st_size, after.st_mtime_ns, after.st_ctime_ns):
            raise ValueError('observer manifest changed while reading')
        return data
    finally:
        os.close(child)


def prepare(state, library):
    """Create after dry-run only. Every parent is pinned without following links."""
    state = Path(state)
    if not isinstance(library, str) or not Path(library).is_absolute() or '\x00' in library:
        raise ValueError('observer resource path must be absolute')
    if not state.is_absolute() or state.resolve() != state:
        raise ValueError('observer state path must be canonical')
    root = os.open('/', os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC)
    fd = root
    try:
        for part in state.parts[1:]:
            child = os.open(part, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC, dir_fd=fd)
            os.close(fd); fd = child
        child = _directory(fd, 'runtime'); os.close(fd); fd = child
        child = _directory(fd, DIRECTORY); os.close(fd); fd = child
        entries = set(os.listdir(fd))
        if entries and (entries != {'owner', 'observer.json'} or _read(fd, 'owner', 256) != OWNER):
            raise ValueError('observer directory is not exclusively managed by Monado II')
        document = {'file_format_version': '1.2.0', 'layer': {
            'name': LAYER, 'type': 'GLOBAL', 'library_path': library,
            'api_version': '1.3.0', 'implementation_version': '1',
            'description': 'Monado II accepted downstream present requests, not scanout',
            'functions': {'vkGetInstanceProcAddr': 'monado_present_GetInstanceProcAddr',
                          'vkGetDeviceProcAddr': 'monado_present_GetDeviceProcAddr'}}}
        data = (json.dumps(document, indent=2, ensure_ascii=True) + '\n').encode('ascii')
        if entries:
            old = _read(fd, 'observer.json', 16384)
            # Validate managed contents, including its exact schema. Its old
            # absolute library path is never used or loaded before replacement.
            previous = json.loads(old)
            expected = json.loads(data)
            oldpath = previous.get('layer', {}).get('library_path')
            if not isinstance(oldpath, str) or not Path(oldpath).is_absolute() or '\x00' in oldpath:
                raise ValueError('previous observer manifest is malformed')
            previous['layer']['library_path'] = library
            if previous != expected:
                raise ValueError('previous observer manifest has unmanaged fields')
            if old == data:
                return str(state / 'runtime' / DIRECTORY)
        for name, content in ((('owner', OWNER),) if not entries else ()) + (('observer.json', data),):
            temporary = '.' + name + '.new-' + os.urandom(8).hex()
            child = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW | os.O_CLOEXEC, 0o600, dir_fd=fd)
            try:
                view = memoryview(content)
                while view:
                    size = os.write(child, view)
                    if size <= 0: raise OSError('short observer manifest write')
                    view = view[size:]
                os.fsync(child)
            finally:
                os.close(child)
            try:
                os.rename(temporary, name, src_dir_fd=fd, dst_dir_fd=fd)
            except BaseException:
                os.unlink(temporary, dir_fd=fd)
                raise
        return str(state / 'runtime' / DIRECTORY)
    finally:
        os.close(fd)
