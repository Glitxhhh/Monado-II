#!/usr/bin/env python3
"""Build/run authored native Windows gameplay components; never launches a game.

Run with the project Python containing Zig 0.14.1. --file-io also checks the
production journal on disposable files; Windows sandbox ancestor ACLs may
require that explicit fixture run outside the tool sandbox.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import uuid

PORT = Path(__file__).resolve().parents[1]
ROOT = PORT.parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--file-io', action='store_true')
    args = parser.parse_args()
    if os.name != 'nt':
        parser.error('This runner qualifies native Windows components only')
    run = PORT / 'runs' / ('gameplay-' + datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ') + '-' + uuid.uuid4().hex[:8])
    run.mkdir(parents=True)
    environment = os.environ.copy()
    environment['ZIG_GLOBAL_CACHE_DIR'] = str(PORT / '.deps' / 'zig-global-cache')
    environment['ZIG_LOCAL_CACHE_DIR'] = str(run / 'zig-cache')
    environment['MONADO_TEST_PORT'] = 'Monado_Windows'
    for key in ('XCDE_INI', 'XCDE_LIBNX', 'LARECOMP_SCALE', 'LARECOMP_WIDE_TEST', 'LARECOMP_HIDDEN'):
        environment.pop(key, None)
    commands = []
    sources = set()

    def command(argv, timeout=120, cwd=ROOT):
        result = subprocess.run([str(a) for a in argv], cwd=cwd, env=environment,
                                capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=timeout)
        commands.append({'argv': [str(a) for a in argv], 'cwd': str(cwd), 'returncode': result.returncode,
                         'stdout': result.stdout, 'stderr': result.stderr})
        print(result.stdout, end='')
        if result.returncode:
            print(result.stderr, file=sys.stderr, end='')
            raise RuntimeError(f'Fixture command failed: {argv[0]} (exit {result.returncode})')
        return result.stdout.strip()

    cc = [sys.executable, '-m', 'ziglang', 'cc']
    flags = ['-std=gnu11', '-O1', '-UNDEBUG', '-march=x86_64_v2', '-ffp-contract=off',
             '-fno-fast-math', '-fno-strict-aliasing', '-D_FILE_OFFSET_BITS=64']

    def build(name, inputs, extra=()):
        binary = run / (name + '.exe')
        paths = [PORT / item for item in inputs]
        sources.update(paths)
        command(cc + flags + list(extra) + paths + ['-o', binary])
        return binary

    status = 'failed'
    try:
        compiler = command([sys.executable, '-m', 'ziglang', 'version'])
        if compiler != '0.14.1':
            raise RuntimeError(f'Expected pinned Zig 0.14.1; found {compiler}')
        source = PORT / 'runtime/hle/cheats_combat.c'
        sources.add(source)
        # Same authored fixture technique as the reference combat test: retain
        # every production function body against synthetic guest frames.
        combat = '\n'.join(line for line in source.read_text().splitlines() if not line.startswith('#include')) + '\n'
        (run / 'combat_production.h').write_text(combat)
        strict = ['-Wall', '-Wextra', '-Werror']
        binary = build('combat', ['tests/native/test_cheats_combat_windows.c'],
                       strict + ['-I', str(run), '-I', str(PORT / 'runtime/hle')])
        command([binary])
        binary = build('rewards', ['tests/native/test_cheats_rewards_windows.c', 'runtime/hle/cheats_rewards.c'], strict)
        command([binary])
        binary = build('threshold', ['tests/native/test_frame_threshold_windows.c'],
                       ['-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections'])
        for hz in (30, 60, 75, 90, 100, 120, 144, 165, 180, 240, 360):
            command([binary, hz])
        binary = build('aspect-camera', ['tests/native/test_xcde_aspect_windows.c', 'runtime/host/xcde_aspect.c'],
                       ['-I', str(PORT / 'runtime')])
        for scenario in ('identity', 'bounds', 'deadband', '16:10', '21:9', '32:9', 'hud', 'invalid', 'concurrent', 'render-context'):
            command([binary, scenario])
        binary = build('aspect-composite', ['tests/native/test_aspect_composite_windows.c'])
        for scenario in ('reduced', 'ui', 'boundaries', 'transitions', 'inherited'):
            command([binary, scenario])
        binary = build('dynamic-output', ['tests/native/test_dynamic_resolution_output_windows.c',
                       'runtime/nvn/output_scale.c', 'runtime/host/xcde_aspect.c'])
        for scenario in ('bounds', 'budget', 'cap', 'resize', 'legacy', 'native-aa'):
            command([binary, scenario])
        contract = ROOT / 'tests/test_function_entries.py'
        sources.add(contract)
        command([sys.executable, '-B', contract,
                 'EntryContract.test_guards_before_returning_entries_and_other_module_ignored',
                 'EntryContract.test_wrong_build_or_any_changed_word_fails_closed',
                 'EntryContract.test_whole_generator_emits_every_checked_dispatch_destination', '-v'])
        if args.file_io:
            for name in ('windows_save_io', 'windows_save_transaction'):
                binary = build(name, [f'tests/native/test_{name}.c'], strict + ['-Wno-unused-function'])
                command([binary, run / (name + '-disposable')])
            binary = build('aspect-guest-dynamic', ['tests/native/test_aspect_guest_dynres_windows.c', 'runtime/host/xcde_aspect.c'])
            for scenario in ('modes', 'original', 'missing', 'mod', 'override', 'malformed'):
                state = run / ('guest-dynamic-' + scenario); state.mkdir()
                command([binary, scenario], cwd=state)
            binary = build('aspect-output', ['tests/native/test_aspect_output_windows.c', 'runtime/host/settings_ini.c',
                           'runtime/host/run_diagnostics_windows.c', 'runtime/host/stubs_gfx.c', 'runtime/host/xcde_aspect.c',
                           'runtime/nvn/output_scale.c'], ['-lbcrypt','-ladvapi32'])
            for scenario in ('defaults', 'roundtrip', 'original', 'native', 'explicit', 'explicit-quality', 'quality', 'bounds', 'canvas'):
                state = run / ('aspect-output-' + scenario); state.mkdir()
                command([binary, scenario], cwd=state)
        status = 'passed'
    finally:
        for pattern in ('runtime/hle/*', 'runtime/platform/windows_save*.h', 'runtime/services/fs.c',
                        'games/xcde/patches.py'):
            sources.update(path for path in PORT.glob(pattern) if path.is_file())
        for name in ('cpu.h', 'ipc.c', 'ipc.h', 'kernel.c', 'rt.h'):
            sources.add(PORT / 'runtime' / name)
        for name in ('gen.py', 'patchfmt.py'):
            sources.add(PORT / 'tools/lifter' / name)
        receipt = {'status': status, 'cwd': str(ROOT), 'python': sys.version,
                   'flags': flags, 'file_io_requested': args.file_io, 'commands': commands,
                   'sources': {str(path.relative_to(ROOT)): hashlib.sha256(path.read_bytes()).hexdigest()
                               for path in sorted(sources) if path.is_file()}}
        (run / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
        print(f'Native component evidence: {run}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
