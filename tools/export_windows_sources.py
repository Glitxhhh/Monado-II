"""[XCDE] Create a clean Windows development source export from reviewed hashes.

Only manifest-declared authored files are read. Private workspace input/builds,
profiles and unrelated files stay in place. This does not package a public release
or approve redistribution of any native dependency or vendor component.
"""
from __future__ import annotations

import argparse
import contextlib
import hashlib
import json
from pathlib import Path
import re

from publication_guard import MANIFEST, POLICY, MAX_FILE, path_problem, content_problem, read_regular, verify_tree

HASH = re.compile(r'[0-9a-f]{64}\Z')


def digest(data):
    return hashlib.sha256(data).hexdigest()


def encoded(value):
    return (json.dumps(value, indent=2, sort_keys=True) + '\n').encode('utf-8')


def document(raw):
    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError('duplicate source metadata key')
            result[key] = value
        return result
    def finite(value):
        raise ValueError('nonfinite source metadata')
    value = json.loads(raw, object_pairs_hook=unique, parse_constant=finite)
    if not isinstance(value, dict):
        raise ValueError('source metadata must be an object')
    return value


def export(source, output, manifest_sha256):
    source, output = Path(source).absolute(), Path(output).absolute()
    if (not HASH.fullmatch(manifest_sha256) or source.resolve() != source or
            output.resolve() != output or output.exists() or source.is_relative_to(output) or
            (output.is_relative_to(source) and not output.is_relative_to(source / 'Monado_Windows/runs'))):
        # The explicit allowlist can safely stage into ignored private runs;
        # no directory recursion can pick up the export or other private data.
        raise ValueError('export needs canonical separate source and fresh output roots plus a manifest hash')
    raw = read_regular(source / MANIFEST)
    if digest(raw) != manifest_sha256:
        raise ValueError('reviewed source manifest SHA256 differs')
    manifest = document(raw)
    files = manifest.get('files')
    if (type(manifest.get('schema')) is not int or manifest.get('schema') != 1 or
            manifest.get('project') != 'Monado II' or manifest.get('source_only') is not True or
            not isinstance(files, dict) or not 1 <= len(files) <= 4096 or POLICY not in files):
        raise ValueError('unsupported reviewed source manifest')
    for name, identity in files.items():
        if (path_problem(name) or name == MANIFEST or not isinstance(identity, dict) or
                not isinstance(identity.get('sha256'), str) or not HASH.fullmatch(identity['sha256']) or
                type(identity.get('bytes')) is not int or not 0 <= identity['bytes'] <= MAX_FILE):
            raise ValueError('unsafe reviewed source entry: ' + str(name))
    names = [name for name in files if not name.startswith('Monado_Linux/') and name != 'update-version.json']
    if not any(name.startswith('Monado_Windows/') for name in names):
        raise ValueError('reviewed source has no Windows implementation')
    policy_bytes = read_regular(source / POLICY)
    if {'bytes': len(policy_bytes), 'sha256': digest(policy_bytes)} != files[POLICY]:
        raise ValueError('source publication policy identity changed')
    policy = document(policy_bytes)
    licensed = policy.get('licensed_media', {})
    if type(policy.get('schema')) is not int or policy.get('schema') != 1 or not isinstance(licensed, dict):
        raise ValueError('invalid source publication policy')
    data, identities = {}, {}
    for name in sorted(names):
        value = read_regular(source / name)
        identity = {'bytes': len(value), 'sha256': digest(value)}
        problem = content_problem(name, value, licensed)
        if identity != files[name] or problem:
            raise ValueError('reviewed source changed or rejected: ' + name + (': ' + problem if problem else ''))
        data[name], identities[name] = value, identity
    trimmed = {'schema': 1, 'project': 'Monado II', 'source_only': True, 'platform': 'windows',
               'windows_implementation': 'windows-parity-development', 'files': identities}
    data[MANIFEST] = encoded(trimmed)
    if read_regular(source / MANIFEST) != raw:
        raise ValueError('reviewed source manifest changed during export')
    from windows_file_guard import locked_directory
    with contextlib.ExitStack() as stack:
        parent = stack.enter_context(locked_directory(output.parent))
        target = stack.enter_context(parent.child(output.name, create=True, owner=True))
        exported = stack.enter_context(target.child('source', create=True, owner=True))
        directories = {'': exported}
        for name, value in sorted(data.items()):
            parts = name.split('/')
            for depth in range(1, len(parts)):
                key = '/'.join(parts[:depth])
                if key not in directories:
                    previous = directories['/'.join(parts[:depth-1])]
                    directories[key] = stack.enter_context(previous.child(parts[depth-1], create=True, owner=True))
            directories['/'.join(parts[:-1])].write_new(parts[-1], value)
        problems = verify_tree(output / 'source')
        if problems:
            raise ValueError('clean Windows source export rejected: ' + '; '.join(problems))
        declaration = {'schema': 1, 'platform': 'windows-amd64',
                       'files': {name: {'bytes': len(value), 'sha256': digest(value)} for name, value in data.items()}}
        target.write_new('source-manifest.json', encoded(declaration))
        target.write_new('source-export.json', encoded({'schema': 1, 'kind': 'private-windows-development-source',
            'reviewed_manifest_sha256': manifest_sha256, 'source_manifest_sha256': digest(data[MANIFEST]),
            'files': len(data), 'public_release': False}))
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--manifest-sha256', required=True)
    args = parser.parse_args()
    try:
        print(export(args.source, args.output, args.manifest_sha256))
    except (OSError, ValueError) as error:
        parser.exit(1, str(error) + '\n')


if __name__ == '__main__':
    main()
