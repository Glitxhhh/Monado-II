#!/usr/bin/env python3
"""Import a hash-bound authored runtime after preparing local compiler tools.

The local dump is not read here. Guest code remains separately generated and
compiled by build_linux.py; this worker never starts the imported executable.
"""
import argparse
import hashlib
import importlib.util
import json
import tarfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module); return module


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--archive', type=Path, required=True)
    ap.add_argument('--sha256', required=True)
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--allow-source-fallback', action='store_true')
    ap.add_argument('--features', help=argparse.SUPPRESS)
    args = ap.parse_args()
    port = ROOT / 'Monado_Linux'
    identity = load('_monado_import_game_abi', port / 'scripts/gamecode_identity.py')
    compiler = {name: digest(port / name) for name in ('.deps/bin/clang-cc', '.deps/bin/clang-cxx')}
    abi = identity.identity(port, compiler)['id']
    portable = load('_monado_import_portable_runtime', ROOT / 'tools/portable_runtime.py')
    source_sha = digest(ROOT / 'publication-manifest.json')
    if not args.allow_source_fallback:
        portable.import_bundle(args.archive, args.output, expected_sha256=args.sha256,
            expected_abi=abi, expected_source_manifest=source_sha)
        return 0
    # Validate the entire externally hash-bound envelope independently before
    # treating any compatibility mismatch as an optional source fallback.
    archive = portable.ordinary(args.archive)
    if portable.digest(archive) != args.sha256:
        raise ValueError('runtime archive SHA256 differs; corruption is not a compatibility fallback')
    with tarfile.open(archive, 'r:gz') as tar:
        members = [m for m in tar.getmembers() if m.name == 'manifest.json']
        if len(members) != 1 or not members[0].isfile() or members[0].size > portable.MAX_JSON:
            raise ValueError('runtime archive requires one bounded manifest')
        with tar.extractfile(members[0]) as stream:
            declared = json.loads(stream.read(portable.MAX_JSON + 1))
    imported = portable.import_bundle(archive, args.output, expected_sha256=args.sha256,
        expected_abi=declared.get('abi_id'), expected_source_manifest=declared.get('source_manifest_sha256'))
    native = load('_monado_import_current_native_recipe', port / 'scripts/build_linux.py')
    prior = json.loads((args.output / 'receipt.json').read_text())
    reasons = []
    if imported['abi_id'] != abi: reasons.append('guest ABI/toolchain')
    if imported['source_manifest_sha256'] != source_sha: reasons.append('source release')
    if prior.get('sources_before') != native.source_snapshot(): reasons.append('native source closure')
    if args.features:
        expected = json.loads(args.features)
        keys = {'dlss_enabled', 'fsr3_enabled', 'fsr4_enabled', 'framegen_enabled'}
        if not isinstance(expected, dict) or set(expected) != keys or any(type(v) is not bool for v in expected.values()):
            raise ValueError('invalid requested runtime capabilities')
        if imported['features'] != expected: reasons.append('requested capabilities')
    if reasons:
        print(json.dumps({'status': 'source-fallback', 'reasons': reasons,
            'scope': 'entire archive verified; unused imported data retained; full source build selected'}), flush=True)
        return 3
    print(json.dumps({'status': 'passed', 'abi_id': abi}), flush=True)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
