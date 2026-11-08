"""Strict data checks for the mutable installed-version marker, never executable input."""
import hashlib
import json
import re

MAX_METADATA = 4096
BACKUP = 'bootstrap-version.json'
BRIDGE = 'version-bridge.json'
WRAPPER = 'Rollback_Monado_II.sh'
BACKUP_KEYS = {'schema', 'owner', 'manifest_sha256', 'original_metadata'}
BRIDGE_KEYS = {'schema', 'owner', 'workspace', 'manifest_sha256', 'updater_sha256',
               'version_helper_sha256', 'wrapper_sha256'}


def metadata(raw):
    if not isinstance(raw, bytes) or not 0 < len(raw) <= MAX_METADATA:
        raise ValueError('invalid installed-version metadata size')

    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError('duplicate installed-version metadata field')
            result[key] = value
        return result

    try:
        value = json.loads(raw, object_pairs_hook=unique)
    except (UnicodeError, ValueError, RecursionError) as error:
        raise ValueError('invalid installed-version metadata JSON') from error
    if (not isinstance(value, dict) or set(value) != {'schema', 'protocol', 'version'}
            or type(value['schema']) is not int or value['schema'] != 1
            or type(value['protocol']) is not int or value['protocol'] != 1
            or not isinstance(value['version'], str) or len(value['version']) > 64
            or not re.fullmatch(r'(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)', value['version'])):
        raise ValueError('unsupported installed-version metadata')
    return value


def checked_original(raw, identity):
    metadata(raw)
    if (not isinstance(identity, dict) or type(identity.get('bytes')) is not int
            or identity['bytes'] != len(raw)
            or identity.get('sha256') != hashlib.sha256(raw).hexdigest()):
        raise ValueError('bootstrap version differs from the original source manifest')
    return raw


def make_backup(owner, manifest_sha256, identity, raw):
    checked_original(raw, identity)
    return dict(schema=1, owner=str(owner), manifest_sha256=manifest_sha256,
                original_metadata=raw.decode('utf-8'))


def original_from_backup(document, owner, manifest_sha256, identity):
    if (not isinstance(document, dict) or set(document) != BACKUP_KEYS
            or type(document.get('schema')) is not int or document['schema'] != 1
            or document.get('owner') != str(owner) or document.get('manifest_sha256') != manifest_sha256
            or not isinstance(document.get('original_metadata'), str)):
        raise ValueError('invalid manifest-bound bootstrap version backup')
    try:
        raw = document['original_metadata'].encode('utf-8')
    except UnicodeError as error:
        raise ValueError('invalid bootstrap version encoding') from error
    return checked_original(raw, identity)
