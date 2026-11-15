#!/usr/bin/env python3
"""Native authored UI/save-selection and rumble contracts; no game or device.

Tests operate only on new fixed ranges in owned fixture processes, with an
authored synthetic resource. They never open a profile, controller or game.
"""
from __future__ import annotations
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import uuid
import zlib

PORT = Path(__file__).resolve().parents[1]
ROOT = PORT.parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--groups', nargs='+', choices=('title', 'expanded', 'quickstep', 'rumble'),
                        default=['title', 'expanded', 'quickstep', 'rumble'])
    args = parser.parse_args()
    if os.name != 'nt':
        parser.error('Native Windows is required')
    output = PORT / 'runs' / ('ui-contracts-' + datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ') + '-' + uuid.uuid4().hex[:8])
    output.mkdir(parents=True)
    env = os.environ.copy()
    env.update(ZIG_GLOBAL_CACHE_DIR=str(PORT / '.deps/zig-global-cache'), ZIG_LOCAL_CACHE_DIR=str(output / 'zig-cache'))
    resource = bytearray(49152)
    struct.pack_into('<II', resource, 0, 0x4448414c, 0x2713)
    struct.pack_into('<I', resource, 0x14, 0x80)
    struct.pack_into('<I', resource, 0x80, 0x20)
    resource[0xa3] = 9
    asset = output / 'synthetic/menu/image/common_window_white_c.wilay'
    asset.parent.mkdir(parents=True)
    asset.write_bytes(resource)
    entries = []
    if 'title' in args.groups:
        entries += [(name, []) for name in ('title_exit_model', 'title_exit_bridge', 'desktop_exit_caption', 'system_exit_bridge')]
    if 'expanded' in args.groups:
        entries += [('expanded_save_scan', ['expanded_save_scan'])]
    if 'quickstep' in args.groups:
        entries += [(name, [name]) for name in ('quickstep_slot', 'quickstep_picker', 'quickstep_stats', 'quickstep_ui_memory', 'quickstep_stats_ui', 'quickstep_native')]
    if 'rumble' in args.groups:
        entries += [(name, []) for name in ('rumble_policy', 'rumble_ipc')]
    sources = [PORT / f'tests/native/test_{name}_windows.c' for name, _ in entries]
    sources += [PORT / f'runtime/hle/{name}.c' for _, names in entries for name in names]
    sources.append(Path(__file__))

    def snapshot():
        pending, found = sources.copy(), {}
        while pending:
            path = pending.pop().resolve()
            if path in found:
                continue
            found[path] = hashlib.sha256(path.read_bytes()).hexdigest()
            for include in re.findall(r'^\s*#\s*include\s+"([^"]+)"', path.read_text(), re.M):
                target = (path.parent / include).resolve()
                if not target.is_file():
                    target = PORT / 'runtime' / include
                if target.is_file():
                    pending.append(target)
        return {str(path.relative_to(ROOT)): digest for path, digest in sorted(found.items())}

    references = {}
    for name, _ in entries:
        path = ROOT / (f'tests/native/test_{name}.c' if name.startswith('rumble_') else f'Monado_Linux/tests/native/test_{name}.c')
        references[str(path.relative_to(ROOT))] = hashlib.sha256(path.read_bytes()).hexdigest()
    receipt = {'status': 'failed', 'cwd': str(ROOT), 'sources_before': snapshot(), 'commands': [],
               'reference_fixtures': references, 'python': sys.version,
               'resource_sha256': hashlib.sha256(resource).hexdigest()}

    def run(name, argv, timeout=120):
        argv = [str(arg) for arg in argv]
        result = subprocess.run(argv, cwd=output, env=env, capture_output=True, text=True, errors='replace', timeout=timeout)
        (output / (name + '.log')).write_text(result.stdout + result.stderr)
        receipt['commands'].append({'name': name, 'argv': argv, 'returncode': result.returncode})
        if result.returncode:
            raise RuntimeError(f'{name} failed ({result.returncode}):\n{result.stdout}{result.stderr}')
        print(result.stdout, end='')
        return result.stdout

    flags = ['-std=gnu11', '-O1', '-UNDEBUG', '-march=x86_64_v2', '-ffp-contract=off', '-fno-fast-math',
             '-fno-strict-aliasing', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', '-I', str(PORT / 'runtime')]
    try:
        if run('compiler', [sys.executable, '-m', 'ziglang', 'version']).strip() != '0.14.1':
            raise RuntimeError('Expected pinned Zig 0.14.1')
        for name, extra in entries:
            binary = output / (name + '.exe')
            specific = ['-DQUICKSTEP_NATIVE_RESOURCE_CRC=' + hex(zlib.crc32(resource))] if name == 'quickstep_native' else []
            command = [sys.executable, '-m', 'ziglang', 'cc', *flags, *specific,
                       PORT / f'tests/native/test_{name}_windows.c',
                       *(PORT / f'runtime/hle/{part}.c' for part in extra), '-o', binary]
            run('compile-' + name, command)
            run('run-' + name, [binary, output / 'synthetic'] if name == 'quickstep_native' else [binary], timeout=45)
        receipt['sources_after'] = snapshot()
        if receipt['sources_before'] != receipt['sources_after']:
            raise RuntimeError('Tested source closure changed during execution')
        receipt['status'] = 'passed'
    finally:
        (output / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
        print('Native authored UI evidence: ' + str(output))


if __name__ == '__main__':
    main()
