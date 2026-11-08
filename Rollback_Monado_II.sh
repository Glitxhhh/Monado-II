#!/usr/bin/env bash
# Select a previous local version using the retained, version-aware updater.
# This entrypoint never opens the launcher or game and never copies saves.
set -euo pipefail
monado_rollback_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
exec /usr/bin/python3.14 -I -B - "$monado_rollback_root" "$@" <<'PY'
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import stat
import sys

HASH = re.compile(r'[0-9a-f]{64}\Z')
BRIDGE_KEYS = {'schema', 'owner', 'workspace', 'manifest_sha256', 'updater_sha256',
               'version_helper_sha256', 'wrapper_sha256'}
WRAPPER = 'Rollback_Monado_II.sh'


def ordinary(path, directory=False):
    if not path.is_absolute() or path.resolve() != path or '..' in path.parts:
        raise ValueError('rollback path must be canonical without links')
    for item in [*reversed(path.parents), path]:
        info = item.lstat()
        if item != path or directory:
            if not stat.S_ISDIR(info.st_mode):
                raise ValueError('rollback path contains a non-directory or link')
        elif not stat.S_ISREG(info.st_mode) or info.st_nlink != 1:
            raise ValueError('rollback file must be ordinary and single-link')
    return path


def read(path, limit):
    ordinary(path)
    descriptor = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC)
    with os.fdopen(descriptor, 'rb') as stream:
        info = os.fstat(stream.fileno())
        if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1 or info.st_size > limit:
            raise ValueError('rollback file exceeds its bound or is not ordinary')
        data = stream.read(limit + 1)
        if len(data) > limit or len(data) != info.st_size:
            raise ValueError('rollback file changed while reading')
        return data


def unique(pairs):
    result = {}
    for name, value in pairs:
        if name in result:
            raise ValueError('duplicate rollback metadata field')
        result[name] = value
    return result


def document(path, limit):
    result = json.loads(read(path, limit), object_pairs_hook=unique)
    if not isinstance(result, dict):
        raise ValueError('rollback metadata must be an object')
    return result


def sha(data):
    return hashlib.sha256(data).hexdigest()


def relative(name):
    if not isinstance(name, str) or not name or '\\' in name or ':' in name:
        raise ValueError('invalid rollback manifest path')
    path = PurePosixPath(name)
    if path.is_absolute() or path.as_posix() != name or any(p in ('.', '..', '.git') for p in path.parts):
        raise ValueError('unsafe rollback manifest path')
    return path


def main():
    arguments = sys.argv[2:]
    if arguments not in ([], ['--check']):
        raise ValueError('usage: ./Rollback_Monado_II.sh [--check]')
    owner = ordinary(Path(sys.argv[1]), directory=True)
    wrapper_data = read(owner / WRAPPER, 128 * 1024)
    bridge_path = owner / 'Monado_Linux/updates/version-bridge.json'
    if os.path.lexists(bridge_path.parent):
        ordinary(bridge_path.parent, directory=True)
    bridge = None
    if os.path.lexists(bridge_path):
        bridge = document(bridge_path, 8192)
        if (set(bridge) != BRIDGE_KEYS or type(bridge['schema']) is not int or bridge['schema'] != 1
                or bridge['owner'] != str(owner)
                or any(not isinstance(bridge[k], str) or not HASH.fullmatch(bridge[k])
                       for k in BRIDGE_KEYS - {'schema', 'owner', 'workspace'})):
            raise ValueError('invalid version bridge descriptor')
        name = bridge['workspace']
        if name != '.' and (not isinstance(name, str) or not re.fullmatch(
                r'Monado_Linux/updates/update-[A-Za-z0-9][A-Za-z0-9_.-]{0,95}/workspace', name)):
            raise ValueError('version bridge must select a retained managed workspace')
        selected = ordinary(owner if name == '.' else owner / name, directory=True)
        if sha(wrapper_data) != bridge['wrapper_sha256']:
            raise ValueError('installed rollback wrapper differs from its bridge binding')
    else:
        # A fresh 0.1.6+ installation can use its own reviewed updater. An older
        # owner needs the bridge established by the capable updated launcher.
        selected = owner
        marker = document(owner / 'update-version.json', 4096)
        value = marker.get('version')
        if (type(marker.get('schema')) is not int or marker['schema'] != 1
                or type(marker.get('protocol')) is not int or marker['protocol'] != 1 or not isinstance(value, str)
                or not re.fullmatch(r'(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)', value)
                or tuple(map(int, value.split('.'))) < (0, 1, 6)):
            raise ValueError('this original installation needs a version-aware updater bridge')
    manifest_data = read(selected / 'publication-manifest.json', 2 * 1024 * 1024)
    manifest = json.loads(manifest_data, object_pairs_hook=unique)
    if (not isinstance(manifest, dict) or type(manifest.get('schema')) is not int
            or manifest['schema'] != 1 or manifest.get('project') != 'Monado II'
            or manifest.get('source_only') is not True or not isinstance(manifest.get('files'), dict)
            or not 1 <= len(manifest['files']) <= 4096):
        raise ValueError('invalid reviewed rollback source manifest')
    if bridge and sha(manifest_data) != bridge['manifest_sha256']:
        raise ValueError('retained rollback source manifest changed')
    files = manifest['files']
    for needed in ('tools/update_linux.py', 'tools/installation_version.py', WRAPPER):
        if needed not in files:
            raise ValueError('reviewed source has no version-aware rollback entrypoint')
    for name, item in files.items():
        path = selected / relative(name)
        if (not isinstance(item, dict) or set(item) != {'bytes', 'sha256'}
                or type(item['bytes']) is not int or not 0 <= item['bytes'] <= 8 * 1024 * 1024
                or not isinstance(item['sha256'], str) or not HASH.fullmatch(item['sha256'])):
            raise ValueError('invalid reviewed rollback file identity')
        if selected == owner and bridge and name == 'update-version.json':
            # Only the verified NEW updater may validate this owner marker
            # against its immutable bootstrap backup and current active record.
            continue
        data = read(path, 8 * 1024 * 1024)
        if len(data) != item['bytes'] or sha(data) != item['sha256']:
            raise ValueError('reviewed rollback source changed: ' + name)
    for folder in (selected / 'tools', selected / 'Monado_Linux/scripts'):
        ordinary(folder, directory=True)
        for path in folder.rglob('*'):
            if path.is_symlink():
                raise ValueError('rollback import source contains a link')
            info = path.lstat()
            if not (stat.S_ISREG(info.st_mode) or stat.S_ISDIR(info.st_mode)):
                raise ValueError('rollback import source contains a special file')
            if stat.S_ISREG(info.st_mode) and path.suffix in ('.py', '.pyc', '.so', '.pyd'):
                if path.relative_to(selected).as_posix() not in files:
                    raise ValueError('rollback import source is not reviewed')
    if bridge:
        for name, key in (('tools/update_linux.py', 'updater_sha256'),
                          ('tools/installation_version.py', 'version_helper_sha256'),
                          (WRAPPER, 'wrapper_sha256')):
            if files[name]['sha256'] != bridge[key]:
                raise ValueError('version bridge differs from reviewed rollback source')
    if selected != owner:
        binding = document(selected / 'installation.json', 8192)
        if (set(binding) != {'schema', 'owner', 'workspace', 'game_port'}
                or type(binding['schema']) is not int or binding['schema'] != 1
                or binding['owner'] != str(owner) or binding['workspace'] != str(selected)):
            raise ValueError('retained rollback workspace has a different installation binding')
    elif os.path.lexists(owner / 'installation.json'):
        raise ValueError('run rollback from the original installation folder')
    command = ['/usr/bin/python3.14', '-I', '-B', str(selected / 'tools/update_linux.py'),
               '--rollback', '--owner', str(owner)]
    if arguments == ['--check']:
        print(json.dumps({'owner': str(owner), 'workspace': str(selected), 'argv': command,
                          'source_verified': True, 'rollback_started': False}))
        return
    os.execv(command[0], command)


try:
    main()
except (OSError, ValueError, TypeError, KeyError, json.JSONDecodeError) as error:
    print('Monado II rollback was not started: ' + str(error), file=sys.stderr)
    sys.exit(1)
PY
