"""[XCDE] Verified Windows source updates; never execute or activate a candidate.

The Windows channel has its own version and metadata asset. A Linux-only release
or the historical original-Windows archive is not a Windows update. The trusted
outer builder owns installation leases, compilation and atomic activation.
"""
from __future__ import annotations

import contextlib
import hashlib
import io
import json
from pathlib import Path
import re
import stat
import urllib.parse
import zipfile

import update_release as common
from publication_guard import MANIFEST, read_regular, verify_tree, path_problem

UpdateError = common.UpdateError
UpdateCancelled = common.UpdateCancelled
METADATA_NAME = 'monado-ii-windows-update.json'
PLATFORM = 'windows-amd64'
IMPLEMENTATION = 'native-windows'
MAX_ARCHIVE = common.MAX_ARCHIVE
MAX_EXTRACTED = common.MAX_EXTRACTED
MAX_MEMBERS = common.MAX_MEMBERS
MAX_METADATA = common.MAX_METADATA
MAX_FILE = 8 * 1024 * 1024
HASH = re.compile(r'[0-9a-f]{64}\Z')


def _digest(data):
    return hashlib.sha256(data).hexdigest()


def _download_url(tag, name):
    if (not isinstance(tag, str) or not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._-]{0,95}', tag)
            or not isinstance(name, str) or not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._-]{0,127}', name)):
        raise UpdateError('unsafe Windows release tag or asset name')
    return ('https://github.com/' + common.REPOSITORY + '/releases/download/' +
            urllib.parse.quote(tag, safe='') + '/' + urllib.parse.quote(name, safe=''))


def _asset(asset, tag, name, maximum):
    if (not isinstance(asset, dict) or asset.get('name') != name or asset.get('state') != 'uploaded'
            or type(asset.get('size')) is not int or not 0 < asset['size'] <= maximum
            or not isinstance(asset.get('digest'), str) or not asset['digest'].startswith('sha256:')
            or not HASH.fullmatch(asset['digest'][7:])
            or asset.get('browser_download_url') != _download_url(tag, name)):
        raise UpdateError('Windows release asset has an invalid identity or official URL')
    return {'url': asset['browser_download_url'], 'bytes': asset['size'], 'sha256': asset['digest'][7:]}


def _read_verified(identity, *, opener=None, progress=None, cancel=None, phase='download'):
    chunks, count = [], 0
    with common._open(identity['url'], opener) as response:
        while True:
            common._cancel(cancel)
            block = response.read(min(common.CHUNK, identity['bytes'] - count + 1))
            if not block:
                break
            count += len(block)
            if count > identity['bytes']:
                raise UpdateError('Windows release asset exceeds its declared size')
            chunks.append(block)
            common._progress(progress, phase, count, identity['bytes'])
    data = b''.join(chunks)
    if count != identity['bytes'] or _digest(data) != identity['sha256']:
        raise UpdateError('Windows release asset size or SHA256 differs')
    return data


def _offer(value):
    required = {'schema', 'platform', 'implementation', 'version', 'tag', 'archive_name',
                'archive_root', 'archive_url', 'archive_sha256', 'archive_bytes',
                'source_manifest_sha256', 'metadata_sha256'}
    if not isinstance(value, dict) or set(value) != required:
        raise UpdateError('unsupported Windows update declaration')
    version = value.get('version')
    common._version(version)
    if (type(value.get('schema')) is not int or value['schema'] != 1
            or value.get('platform') != PLATFORM or value.get('implementation') != IMPLEMENTATION
            or value.get('archive_name') != 'monado-ii-windows-' + version + '-setup.zip'
            or value.get('archive_root') != 'monado-ii-windows-' + version + '-source'
            or value.get('archive_url') != _download_url(value.get('tag'), value['archive_name'])
            or type(value.get('archive_bytes')) is not int or not 0 < value['archive_bytes'] <= MAX_ARCHIVE
            or any(not isinstance(value.get(key), str) or not HASH.fullmatch(value[key])
                   for key in ('archive_sha256', 'source_manifest_sha256', 'metadata_sha256'))):
        raise UpdateError('Windows update platform, version or archive identity differs')
    return value


def discover(current_version='0.0.0', *, opener=None):
    """Return a newer independent Windows offer, or None for other channels.

    GitHub HTTPS metadata and API asset digests bind the payload. This is not
    claimed to be an independent publisher signature. Missing/malformed Windows
    metadata is never replaced with a Linux archive or a legacy original build.
    """
    current = common._version(current_version)
    with common._open(common.LATEST_URL, opener) as response:
        raw = response.read(MAX_METADATA + 1)
    if len(raw) > MAX_METADATA:
        raise UpdateError('oversized release metadata')
    release = common._json(raw)
    if release.get('draft') is not False or release.get('prerelease') is not False:
        return None
    assets, tag = release.get('assets'), release.get('tag_name')
    if (not isinstance(assets, list) or len(assets) > 64 or
            any(not isinstance(item, dict) for item in assets)):
        raise UpdateError('invalid official release asset list')
    selected = [item for item in assets if item.get('name') == METADATA_NAME]
    if not selected:
        return None
    if len(selected) != 1:
        raise UpdateError('ambiguous Windows update metadata asset')
    identity = _asset(selected[0], tag, METADATA_NAME, MAX_METADATA)
    metadata = common._json(_read_verified(identity, opener=opener, phase='metadata'))
    expected = {'schema', 'platform', 'implementation', 'version', 'archive_name',
                'archive_root', 'archive_sha256', 'archive_bytes', 'source_manifest_sha256'}
    if set(metadata) != expected:
        raise UpdateError('unsupported Windows update metadata fields')
    metadata.update(tag=tag, metadata_sha256=identity['sha256'])
    metadata['archive_url'] = _download_url(tag, metadata['archive_name'])
    offer = _offer(metadata)
    archive = [item for item in assets if item.get('name') == offer['archive_name']]
    if len(archive) != 1:
        raise UpdateError('Windows update needs one exact source archive')
    archive_identity = _asset(archive[0], tag, offer['archive_name'], MAX_ARCHIVE)
    if archive_identity != {'url': offer['archive_url'], 'sha256': offer['archive_sha256'],
                            'bytes': offer['archive_bytes']}:
        raise UpdateError('Windows update metadata and official archive asset disagree')
    return offer if common._version(offer['version']) > current else None


def verified_source(root, offer=None):
    """Run installed trusted source guards without importing candidate modules."""
    root = Path(root).absolute()
    problems = verify_tree(root)
    if problems:
        raise UpdateError('Windows source guard rejected candidate: ' + '; '.join(problems))
    raw = read_regular(root / MANIFEST)
    manifest = common._json(raw)
    if (manifest.get('source_only') is not True or manifest.get('platform') != 'windows'
            or manifest.get('windows_implementation') not in (IMPLEMENTATION, 'windows-parity-development')
            or any(name.startswith('Monado_Linux/') for name in manifest['files'])):
        raise UpdateError('candidate is not a native Windows source-only tree')
    if offer is not None:
        _offer(offer)
        status_path = root / 'source-only-status.json'
        if (manifest.get('windows_implementation') != IMPLEMENTATION
                or manifest.get('release_version') != offer['version']
                or _digest(raw) != offer['source_manifest_sha256']):
            raise UpdateError('Windows source manifest differs from the release identity')
        status = common._json(read_regular(status_path))
        if (status.get('schema') != 1 or type(status.get('schema')) is not int
                or status.get('project') != 'Monado II' or status.get('platform') != 'windows'
                or status.get('version') != offer['version'] or status.get('kind') != 'source-only'
                or status.get('local_generation_required') is not True
                or any(status.get(key) is not False for key in
                       ('contains_game_files', 'contains_game_derived_code', 'contains_compiled_binaries'))):
            raise UpdateError('Windows distribution metadata is missing or incompatible')
    return manifest


def _zip_members(raw, offer, cancel):
    contents, folded, prefixes, components, total = {}, set(), set(), {}, 0
    prefix = offer['archive_root'] + '/'
    try:
        with zipfile.ZipFile(io.BytesIO(raw)) as archive:
            members = archive.infolist()
            if not 1 <= len(members) <= MAX_MEMBERS:
                raise UpdateError('Windows source archive member count exceeds its bound')
            for item in members:
                common._cancel(cancel)
                original = item.orig_filename
                if (original != item.filename or '\\' in original or '\x00' in original
                        or item.is_dir() or item.flag_bits & 1 or not original.startswith(prefix)
                        or stat.S_IFMT(item.external_attr >> 16) not in (0, stat.S_IFREG)
                        or item.external_attr & 0x10):
                    raise UpdateError('Windows source archive contains a linked, encrypted or nonordinary member')
                name = original[len(prefix):]
                if path_problem(name) or name in contents or name.casefold() in folded:
                    raise UpdateError('Windows source archive path is unsafe, duplicated or colliding')
                parts = name.split('/')
                parents = {'/'.join(parts[:n]).casefold() for n in range(1, len(parts))}
                if name.casefold() in prefixes or parents & folded:
                    raise UpdateError('Windows source archive has a file/directory collision')
                for depth in range(1, len(parts) + 1):
                    component = '/'.join(parts[:depth])
                    previous = components.setdefault(component.casefold(), component)
                    if previous != component:
                        raise UpdateError('Windows source archive directory spellings collide')
                if not 0 <= item.file_size <= MAX_FILE:
                    raise UpdateError('Windows source archive file exceeds its bound')
                total += item.file_size
                if total > MAX_EXTRACTED:
                    raise UpdateError('Windows source archive expands beyond its bound')
                with archive.open(item) as stream:
                    data = stream.read(item.file_size + 1)
                if len(data) != item.file_size:
                    raise UpdateError('Windows source archive member size differs')
                contents[name] = data
                folded.add(name.casefold())
                prefixes.update(parents)
    except (zipfile.BadZipFile, RuntimeError, OSError) as error:
        raise UpdateError('Windows source archive cannot be read: ' + str(error)) from error
    if MANIFEST not in contents or _digest(contents[MANIFEST]) != offer['source_manifest_sha256']:
        raise UpdateError('Windows source archive has no matching manifest')
    return contents


def fetch_and_extract(offer, destination, *, opener=None, progress=None, cancel=None):
    """Stage a new, fully checked Windows source tree; never alter an install."""
    _offer(offer)
    destination = Path(destination).absolute()
    if destination.exists() or destination.resolve() != destination:
        raise UpdateError('Windows update staging must be a new canonical directory')
    raw = _read_verified({'url': offer['archive_url'], 'sha256': offer['archive_sha256'],
                          'bytes': offer['archive_bytes']}, opener=opener, progress=progress, cancel=cancel)
    contents = _zip_members(raw, offer, cancel)
    from windows_file_guard import locked_directory
    with contextlib.ExitStack() as stack:
        parent = stack.enter_context(locked_directory(destination.parent))
        target = stack.enter_context(parent.child(destination.name, create=True, owner=True))
        target.write_new('source.zip', raw)
        declaration = {'schema': 1, 'kind': 'verified-windows-source-extraction', 'offer': offer,
                       'files': {name: {'bytes': len(data), 'sha256': _digest(data)} for name, data in contents.items()}}
        target.write_new('extraction-intent.json', (json.dumps(declaration, sort_keys=True) + '\n').encode('utf-8'))
        source = stack.enter_context(target.child('source', create=True, owner=True))
        directories = {'': source}
        for index, (name, data) in enumerate(sorted(contents.items()), 1):
            common._cancel(cancel)
            parts = name.split('/')
            for depth in range(1, len(parts)):
                key = '/'.join(parts[:depth])
                if key not in directories:
                    previous = directories['/'.join(parts[:depth-1])]
                    directories[key] = stack.enter_context(previous.child(parts[depth-1], create=True, owner=True))
            directories['/'.join(parts[:-1])].write_new(parts[-1], data)
            common._progress(progress, 'extract', index, len(contents))
        verified_source(destination / 'source', offer)
        target.write_new('extraction.json', (json.dumps(declaration, sort_keys=True) + '\n').encode('utf-8'))
    return destination / 'source'
