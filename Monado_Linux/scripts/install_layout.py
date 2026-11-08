"""Bound ordinary update workspaces to their original input and private profile.

Only the updater writes installation.json, after verifying a source release.
These read-only checks do not create paths, move state or bless relocated builds.
"""
import json
import os
from pathlib import Path
import re
import stat

MAX_MARKER = 16 * 1024
UPDATE_ID = re.compile(r'[A-Za-z0-9][A-Za-z0-9._-]{0,127}\Z')


def canonical(path, *, directory=False, exists=True):
    path = Path(path)
    if not path.is_absolute() or '..' in path.parts or path.resolve() != path:
        raise ValueError('installation paths must be canonical absolute paths without symlinks: ' + str(path))
    current = Path(path.anchor)
    for part in path.parts[1:]:
        current /= part
        try:
            info = current.lstat()
        except FileNotFoundError:
            if not exists:
                continue
            raise ValueError('missing installation path: ' + str(current)) from None
        if stat.S_ISLNK(info.st_mode):
            raise ValueError('installation paths must not contain symlinks: ' + str(current))
        if (current != path or directory) and not stat.S_ISDIR(info.st_mode):
            raise ValueError('installation path must be an ordinary directory: ' + str(current))
    return path


def _unique(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError('duplicate installation marker key')
        result[key] = value
    return result


def _path(value):
    if not isinstance(value, str) or str(Path(value)) != value:
        raise ValueError('installation marker paths must be canonical absolute strings')
    return canonical(value, directory=True)


def _marker(port):
    path = port.parent / 'installation.json'
    try:
        before = path.lstat()
    except FileNotFoundError:
        return None
    if not stat.S_ISREG(before.st_mode) or before.st_nlink != 1 or not 0 < before.st_size <= MAX_MARKER:
        raise ValueError('installation marker must be an ordinary single-link bounded file')
    fd = os.open(path, os.O_RDONLY | getattr(os, 'O_NOFOLLOW', 0) | getattr(os, 'O_CLOEXEC', 0))
    try:
        first = os.fstat(fd)
        with os.fdopen(fd, 'rb', closefd=False) as stream:
            data = stream.read(MAX_MARKER + 1)
        last = os.fstat(fd)
        after = path.lstat()
        snapshot = lambda value: (value.st_dev, value.st_ino, value.st_mode, value.st_nlink,
                                  value.st_size, value.st_mtime_ns, value.st_ctime_ns,
                                  getattr(value, 'st_birthtime_ns', None))
        def same_object(left, right):
            # Windows lstat/fstat can disagree on executable bits and ctime
            # semantics (CPython issue157671). Keep full snapshots within each
            # API; compare their shared file identity across the two APIs.
            def identity(value):
                return (value.st_dev, value.st_ino, stat.S_IFMT(value.st_mode),
                        value.st_size, value.st_mtime_ns)
            left_birth = getattr(left, 'st_birthtime_ns', None)
            right_birth = getattr(right, 'st_birthtime_ns', None)
            return (identity(left) == identity(right) and
                    (left_birth is None or right_birth is None or left_birth == right_birth))
        if (not stat.S_ISREG(first.st_mode) or first.st_nlink != 1 or
                snapshot(before) != snapshot(after) or snapshot(first) != snapshot(last) or
                not same_object(before, first) or not same_object(after, last) or
                len(data) != first.st_size or len(data) > MAX_MARKER or path.resolve() != path):
            raise ValueError('installation marker changed while reading')
    finally:
        os.close(fd)
    try:
        value = json.loads(data, object_pairs_hook=_unique)
    except (UnicodeError, ValueError) as error:
        raise ValueError('invalid installation marker: ' + str(error)) from error
    if (not isinstance(value, dict) or set(value) != {'schema', 'owner', 'workspace', 'game_port'} or
            type(value['schema']) is not int or value['schema'] != 1):
        raise ValueError('invalid installation marker fields or schema')
    return value


def _managed_port(port, owner):
    try:
        relative = port.relative_to(owner / 'Monado_Linux/updates')
    except ValueError:
        raise ValueError('managed workspace must belong to its installation owner') from None
    if (len(relative.parts) != 3 or not UPDATE_ID.fullmatch(relative.parts[0]) or
            relative.parts[1:] != ('workspace', 'Monado_Linux')):
        raise ValueError('managed workspace must be OWNER/Monado_Linux/updates/ID/workspace')


def binding(port):
    """Return owner/workspace/game roots; absent markers retain ordinary layout."""
    port = canonical(port, directory=True)
    if port.name != 'Monado_Linux':
        raise ValueError('installation port must be named Monado_Linux')
    value = _marker(port)
    if value is None:
        return {'owner': port.parent, 'workspace': port.parent, 'game_port': port}
    owner, workspace, selected = (_path(value[key]) for key in ('owner', 'workspace', 'game_port'))
    canonical(owner / 'Monado_Linux', directory=True)
    if workspace != port.parent:
        raise ValueError('installation marker workspace differs from its location')
    _managed_port(port, owner)
    # The original port and individually bound ordinary managed ports are the
    # only possible build owners. Never accept arbitrary external dependency or
    # executable roots, and never follow a chain to silently change selection.
    seen = {port}
    checked = selected
    while checked != owner / 'Monado_Linux':
        _managed_port(checked, owner)
        if checked in seen:
            if checked == selected == port:
                break  # a newly built managed game belongs to its own workspace
            raise ValueError('cyclic installation game binding')
        seen.add(checked)
        other = _marker(checked)
        if other is None or _path(other['owner']) != owner or _path(other['workspace']) != checked.parent:
            raise ValueError('selected managed game port has no matching owner binding')
        following = _path(other['game_port'])
        if following == checked:
            break
        checked = following
        if len(seen) > 32:
            raise ValueError('installation game binding nesting exceeds limit')
    return {'owner': owner, 'workspace': workspace, 'game_port': selected}


def installation_root(port):
    return binding(port)['owner']


def game_port(port):
    return binding(port)['game_port']


def profile_port(port):
    return installation_root(port) / 'Monado_Linux'


def state_roots(port):
    profile = profile_port(port)
    return tuple(dict.fromkeys((profile / 'play', profile / 'runs', Path(port) / 'runs')))


def build_port(port, executable):
    """Select only current-GUI or directly bound game outputs, without links."""
    port = canonical(port, directory=True)
    selected = game_port(port)
    binary = canonical(executable)
    for root in dict.fromkeys((port, selected)):
        for name in ('build', 'runs', 'accepted'):
            parent = root / name
            if binary != parent and binary.is_relative_to(parent):
                return root
    raise ValueError('build must belong to the current or bound game Linux port')


def shared_input(port):
    """An update may link the original input, never substitute another dump."""
    owner = installation_root(port)
    original = (owner / 'game_files').resolve(strict=True)
    candidate = (Path(port).parent / 'game_files').resolve(strict=True)
    if original != candidate or not original.is_dir():
        raise ValueError('managed workspace input must match its installation owner')
    return original
