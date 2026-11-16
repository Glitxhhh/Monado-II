#!/usr/bin/env python3
"""Package a reviewed native Windows source release and its updater metadata.

The portable setup EXE is a separate artifact. This command consumes only a
clean, explicitly hash-selected Windows source export; it never reads game
input, builds a game, publishes a release, or modifies an existing output.
"""
from __future__ import annotations

import argparse
import contextlib
import hashlib
import io
import json
import os
from pathlib import Path
import stat
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from publication_guard import MANIFEST, content_problem, verify_tree
import update_windows_release as update
import windows_file_guard as guard

IDENTITY = 'WindowsExperimental'
ZIP_DATE = (1980, 1, 1, 0, 0, 0)
GENERATED = frozenset(('RELEASE_README.md', 'source-only-status.json'))


class ReleaseError(ValueError):
    """The source identity, native file boundary or release contract failed."""


def digest(data):
    return hashlib.sha256(data).hexdigest()


def encoded(value):
    return (json.dumps(value, sort_keys=True, indent=2) + '\n').encode('utf-8')


def _check_source(root):
    problems = verify_tree(root)
    if problems:
        raise ReleaseError('Windows source publication guard rejected: ' + '; '.join(problems))


def _readme(version):
    return (
        '# Monado II — Windows Experimental ' + version + '\n\n'
        'This native Windows source/setup kit is experimental. Its independent\n'
        'Windows version does not identify a Linux release or certify complete\n'
        'gameplay, visual quality, performance, or optional-provider qualification.\n'
        'Preserve existing settings and saves when testing it.\n\n'
        'For graphical setup, use the separate portable Windows setup EXE from\n'
        'the same release. The source workflow is documented in\n'
        'docs/BUILD_WINDOWS.md and Monado_Windows/docs/WINDOWS_BUILDER_BACKEND.md.\n'
        'The updater uses this source archive to build locally before activation.\n\n'
        'This archive contains no game files, game-derived generated code,\n'
        'compiled binaries, translated game shaders, caches, saves, keys,\n'
        'proprietary SDK binaries or prepared model assets. Local generation\n'
        'requires your own already-extracted supported game input. The project\n'
        'will not provide game files or tell you where to obtain them.\n'
        'Optional vendor components remain subject to their own terms and local\n'
        'selection; packaging does not download them or accept an agreement.\n\n'
        'Keep the accompanying LICENSE, NOTICE.md, CREDITS.md and component\n'
        'licenses with the source. No release is published by this command.\n'
    ).encode('utf-8')


def _archive(contents, prefix):
    memory = io.BytesIO()
    with zipfile.ZipFile(memory, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for name, data in sorted(contents.items()):
            item = zipfile.ZipInfo(prefix + '/' + name, date_time=ZIP_DATE)
            item.create_system = 3
            item.external_attr = (stat.S_IFREG | 0o644) << 16
            item.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(item, data, compress_type=zipfile.ZIP_DEFLATED, compresslevel=9)
    raw = memory.getvalue()
    if not 0 < len(raw) <= update.MAX_ARCHIVE:
        raise ReleaseError('Windows source archive exceeds the updater bound')
    return raw


def package(source, output, version, manifest_sha256):
    """Create fresh artifacts, retaining native source/ancestor leases throughout.

    Hashes have no cycle: the publication manifest lists source/status/README,
    excluding itself; metadata binds the ZIP and that manifest; the external
    receipt/checksums bind metadata. Neither receipt nor metadata is in the ZIP.
    On failure, known fresh output is preserved for inspection, never repaired.
    """
    if os.name != 'nt':
        raise ReleaseError('Windows release packaging requires native Win32 file guards')
    update.common._version(version)
    if not isinstance(manifest_sha256, str) or not update.HASH.fullmatch(manifest_sha256):
        raise ReleaseError('An explicit reviewed publication-manifest SHA256 is required')
    source, output = map(lambda path: Path(guard.canonical_path(path)), (source, output))
    if (source == output or source.is_relative_to(output) or output.is_relative_to(source)):
        raise ReleaseError('Release output must be separate from the clean source export')
    archive_name = 'monado-ii-windows-' + version + '-setup.zip'
    archive_root = 'monado-ii-windows-' + version + '-source'
    with contextlib.ExitStack() as leases:
        original = leases.enter_context(guard.locked_directory(source, owner=True))
        stream = leases.enter_context(original.open_regular(MANIFEST, max_bytes=update.MAX_FILE, owner=True))
        raw = stream.read()
        if digest(raw) != manifest_sha256:
            raise ReleaseError('Reviewed source manifest identity differs')
        document = update.common._json(raw)
        _check_source(source)
        if (document.get('source_only') is not True or document.get('platform') != 'windows'
                or document.get('windows_implementation') not in
                    ('windows-parity-development', update.IMPLEMENTATION)
                or not isinstance(document.get('files'), dict)
                or not 1 <= len(document['files']) <= update.MAX_MEMBERS - 3
                or not any(name.startswith('Monado_Windows/') for name in document['files'])
                or any(name.startswith('Monado_Linux/') or name == 'update-version.json'
                       for name in document['files'])
                or GENERATED.intersection(document['files'])):
            raise ReleaseError('Use a clean Windows-only source export without release-generated files')
        files = {}
        total = 0
        for name, identity in sorted(document['files'].items()):
            leaf = leases.enter_context(original.open_regular(name, max_bytes=update.MAX_FILE, owner=True))
            data = leaf.read()
            if identity != {'bytes': len(data), 'sha256': digest(data)}:
                raise ReleaseError('Reviewed source member identity differs: ' + name)
            total += len(data)
            if total > update.MAX_EXTRACTED:
                raise ReleaseError('Windows source exceeds the updater expanded-size bound')
            files[name] = data
        status = dict(schema=1, project='Monado II', platform='windows', version=version,
            windows_implementation=update.IMPLEMENTATION, windows_identity=IDENTITY,
            kind='source-only', local_generation_required=True, contains_game_files=False,
            contains_game_derived_code=False, contains_compiled_binaries=False,
            runtime_validation='not established by packaging', release_channel='experimental')
        for name, data in {'RELEASE_README.md': _readme(version),
                           'source-only-status.json': encoded(status)}.items():
            if problem := content_problem(name, data):
                raise ReleaseError('Generated release declaration rejected: ' + problem)
            files[name] = data
        release = dict(document, platform='windows', source_only=True,
            windows_implementation=update.IMPLEMENTATION, windows_identity=IDENTITY,
            release_version=version, release_channel='experimental', release_kind='source-only',
            files={name: {'bytes': len(data), 'sha256': digest(data)} for name, data in files.items()})
        files[MANIFEST] = encoded(release)
        if len(files) > update.MAX_MEMBERS or sum(map(len, files.values())) > update.MAX_EXTRACTED:
            raise ReleaseError('Generated Windows source exceeds updater member/size bounds')
        if content_problem(MANIFEST, files[MANIFEST]):
            raise ReleaseError('Generated Windows manifest exceeds source policy')
        archive = _archive(files, archive_root)
        metadata = dict(schema=1, platform=update.PLATFORM, implementation=update.IMPLEMENTATION,
            version=version, archive_name=archive_name, archive_root=archive_root,
            archive_sha256=digest(archive), archive_bytes=len(archive),
            source_manifest_sha256=digest(files[MANIFEST]))
        metadata_raw = encoded(metadata)
        # A synthetic official URL/tag is used only to exercise the installed
        # parser. Nothing is downloaded and no tag/release is created.
        offer = dict(metadata, tag='windows-' + version,
            archive_url=update._download_url('windows-' + version, archive_name),
            metadata_sha256=digest(metadata_raw))
        update._offer(offer)
        if update._zip_members(archive, offer, None) != files:
            raise ReleaseError('Windows updater archive readback differs from reviewed source')
        _check_source(source)
        if original.read_regular(MANIFEST, max_bytes=update.MAX_FILE, owner=True) != raw:
            raise ReleaseError('Reviewed source changed during packaging')
        receipt = dict(schema=1, project='Monado II', platform=update.PLATFORM,
            implementation=update.IMPLEMENTATION, windows_identity=IDENTITY,
            version=version, release_channel='experimental', kind='source-only',
            archive=archive_name, archive_root=archive_root, sha256=digest(archive), bytes=len(archive),
            metadata=update.METADATA_NAME, metadata_sha256=digest(metadata_raw),
            reviewed_manifest_sha256=manifest_sha256,
            release_manifest_sha256=metadata['source_manifest_sha256'],
            archived_files=len(files), archive_readback_verified=True,
            contains_game_files=False, contains_game_derived_code=False,
            contains_compiled_binaries=False, local_generation_required=True,
            portable_exe_included=False, release_published=False)
        checksums = (digest(archive) + '  ' + archive_name + '\n' +
                     digest(metadata_raw) + '  ' + update.METADATA_NAME + '\n').encode('ascii')
        leases.enter_context(guard.locked_directory(output.parent))
        # mkdir is exclusive. Guard.child(create=True) also adopts an existing
        # directory, which would be wrong for a release output transaction.
        output.mkdir()
        target = leases.enter_context(guard.locked_directory(output, owner=True))
        target.write_new(archive_name, archive)
        target.write_new(update.METADATA_NAME, metadata_raw)
        target.write_new('SHA256SUMS', checksums)
        # Written last: an interrupted directory is never a passed package.
        target.write_new('release-receipt.json', encoded(receipt))
    return receipt


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True, help='clean Windows source export root')
    parser.add_argument('--manifest-sha256', required=True, help='reviewed publication-manifest.json SHA256')
    parser.add_argument('--version', required=True, help='independent numeric Windows version, e.g. 0.1.0')
    parser.add_argument('--output', type=Path, required=True, help='fresh separate artifact directory')
    args = parser.parse_args(argv)
    try:
        result = package(args.source, args.output, args.version, args.manifest_sha256)
    except (OSError, ValueError) as error:
        parser.exit(1, 'Windows release rejected: ' + str(error) + '\n')
    print(json.dumps(result, sort_keys=True))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
