#!/usr/bin/env python3
"""Run the bundled native frontend through the workspace's existing checks."""
import argparse
import hashlib
import importlib
import json
import os
from pathlib import Path, PurePosixPath
import stat
import sys

EXPECTED = {'AppRun', 'monado-ii.desktop', 'monado-ii.xpm', '.DirIcon',
    'usr/bin/monado-launcher', 'usr/lib/monado-ii/driver.py',
    'usr/share/monado-ii/selection.json'} | {
    'usr/share/doc/monado-ii/' + name for name in (
        'AppImage-runtime-LICENSE.txt', 'libfuse-LGPL-2.1.txt', 'squashfuse-LICENSE.txt',
        'musl-COPYRIGHT.txt', 'zstd-LICENSE.txt', 'zlib-LICENSE.txt',
        'mimalloc-LICENSE.txt', 'THIRD-PARTY-NOTICES.txt',
        'LICENSE', 'CREDITS.md', 'NOTICE.md', 'ImGui-LICENSE.txt', 'DejaVu-LICENSE.txt')}


def regular(path, limit=64 * 1024 * 1024):
    if path.resolve() != path:
        raise ValueError('AppImage file must be canonical: ' + str(path))
    before = path.lstat()
    if not stat.S_ISREG(before.st_mode) or before.st_nlink != 1 or before.st_size > limit:
        raise ValueError('AppImage file must be an ordinary single-link file: ' + str(path))
    data = path.read_bytes()
    after = path.lstat()
    snapshot = lambda value: (value.st_dev, value.st_ino, value.st_mode, value.st_nlink,
                              value.st_size, value.st_mtime_ns, value.st_ctime_ns)
    if snapshot(before) != snapshot(after) or len(data) != before.st_size:
        raise ValueError('AppImage file changed while reading: ' + str(path))
    return data


def relative(name):
    if (not isinstance(name, str) or not name or '\\' in name or ':' in name or
            name.startswith('/') or name != str(PurePosixPath(name)) or
            any(part in ('', '.', '..') for part in name.split('/'))):
        raise ValueError('Invalid AppImage selection path')
    return PurePosixPath(name)


def document(path):
    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError('Duplicate AppImage metadata key')
            result[key] = value
        return result
    return json.loads(regular(path, 1024 * 1024), object_pairs_hook=unique)


def validate_image(appdir):
    manifest_name = 'usr/share/monado-ii/image-manifest.json'
    manifest = document(appdir / manifest_name)
    if type(manifest.get('schema')) is not int or manifest.get('schema') != 1 or manifest.get('project') != 'Monado II':
        raise ValueError('Invalid Monado II AppImage manifest')
    identities = manifest.get('files')
    if not isinstance(identities, dict) or set(identities) != EXPECTED:
        raise ValueError('Invalid AppImage file identities')
    expected = {manifest_name} | set(identities)
    actual = set()
    directories = set()
    for path in appdir.rglob('*'):
        info = path.lstat()
        if stat.S_ISDIR(info.st_mode):
            directories.add(str(path.relative_to(appdir)))
            continue
        name = path.relative_to(appdir).as_posix()
        actual.add(name)
        data = regular(path)
        if name == manifest_name:
            continue
        identity = identities.get(name)
        if (not isinstance(identity, dict) or identity.get('bytes') != len(data) or
                identity.get('sha256') != hashlib.sha256(data).hexdigest()):
            raise ValueError('Bundled launcher file differs from manifest: ' + name)
    if actual != expected:
        raise ValueError('Unexpected or missing file in launcher AppImage')
    expected_directories = {str(parent) for name in expected for parent in PurePosixPath(name).parents if str(parent) != '.'}
    if directories != expected_directories:
        raise ValueError('Unexpected or missing directory in launcher AppImage')
    for name in expected:
        relative(name)


def selection_paths(appdir, workspace):
    selection = document(appdir / 'usr/share/monado-ii/selection.json')
    if selection.get('schema') != 1 or selection.get('project') != 'Monado II':
        raise ValueError('Invalid AppImage workspace selection')
    result = {}
    for field in ('launcher', 'game'):
        name = relative(selection.get(field))
        if len(name.parts) < 4 or name.parts[0] != 'Monado_Linux' or name.parts[1] not in ('build', 'runs', 'accepted'):
            raise ValueError('AppImage may select only local Monado II builds')
        path = workspace / name
        if path.resolve() != path:
            raise ValueError('Build paths moved or contain links; rebuild the AppImage')
        result[field] = path
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description='Open Monado II using this folder’s verified local builds. Keep the AppImage beside Monado_Linux; rebuild after moving the folder.')
    parser.add_argument('--dry-run', action='store_true', help='verify the workspace and print the launch plan without opening a window')
    args = parser.parse_args(argv)
    try:
        appdir = Path(__file__).resolve().parents[3]
        validate_image(appdir)
        image = Path(os.environ.get('APPIMAGE', '')).absolute()
        regular(image)
        workspace = image.parent
        port = workspace / 'Monado_Linux'
        if port.resolve() != port or not port.is_dir():
            raise ValueError('Keep this AppImage beside Monado_Linux and run Build_Monado_II.sh')
        selected = selection_paths(appdir, workspace)
        # Isolated Python ignores inherited paths; only the explicit, ordinary
        # workspace scripts directory is added for the checked launch wrappers.
        scripts = port / 'scripts'
        if scripts.resolve() != scripts or not scripts.is_dir():
            raise ValueError('Missing canonical Monado II launch scripts; rebuild')
        sys.path.insert(0, str(scripts))
        launcher = importlib.import_module('launcher_linux')
        command = ['--launcher-executable', str(selected['launcher']),
                   '--game-executable', str(selected['game']),
                   '--packaged-launcher', str(appdir / 'usr/bin/monado-launcher'),
                   '--state-root', str(port / 'play/launcher')]
        if args.dry_run:
            command.append('--dry-run')
        return launcher.main(command)
    except (OSError, ValueError, ImportError) as error:
        parser.exit(1, 'Monado II AppImage: ' + str(error) + '\nKeep the workspace together; rerun Build_Monado_II.sh after moving it.\n')


if __name__ == '__main__':
    sys.exit(main())
