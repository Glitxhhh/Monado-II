#!/usr/bin/env python3
"""Authored native AM/IPC/kernel, parental-client and report-refusal tests.

Never starts a game, touches profiles or interacts with an existing process.
Every negative case is a separate owned fixture process with a bounded timeout.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time
import uuid

PORT = Path(__file__).resolve().parents[1]
ROOT = PORT.parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dependency-root', type=Path, required=True)
    parser.add_argument('--groups', nargs='+', choices=('am', 'pctl', 'prepo'), default=['am', 'pctl', 'prepo'])
    args = parser.parse_args()
    if os.name != 'nt':
        parser.error('Native Windows is required')
    dependencies = args.dependency_root.resolve()
    if not (dependencies / 'lib/libSDL3.dll.a').is_file():
        parser.error('Expected the pinned native dependency closure')
    output = PORT / 'runs' / ('services-' + datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ') + '-' + uuid.uuid4().hex[:8])
    output.mkdir(parents=True)
    env = os.environ.copy()
    env.update(PATH=str(dependencies / 'bin') + os.pathsep + env.get('PATH', ''), XCDE_STRICT='1',
               ZIG_GLOBAL_CACHE_DIR=str(PORT / '.deps/zig-global-cache'), ZIG_LOCAL_CACHE_DIR=str(output / 'zig-cache'))
    sources = [PORT / f'tests/native/test_{name}_windows.c' for name in args.groups]
    support = [PORT / name for name in ('runtime/platform/windows.c', 'runtime/paths.c', 'runtime/hle/fibers.c',
                                      'runtime/diag/diag.c', 'runtime/diag/sampler.c')]

    def snapshot():
        pending, found = sources + support, {}
        while pending:
            path = pending.pop().resolve()
            if path in found:
                continue
            found[path] = hashlib.sha256(path.read_bytes()).hexdigest()
            if path.suffix in ('.c', '.h'):
                for include in re.findall(r'^\s*#\s*include\s+"([^"]+)"', path.read_text(), re.M):
                    target = (path.parent / include).resolve()
                    if target.is_file():
                        pending.append(target)
        return {str(path.relative_to(ROOT)): digest for path, digest in sorted(found.items())}

    receipt = {'status': 'failed', 'cwd': str(ROOT), 'python': sys.version, 'sources_before': snapshot(), 'commands': []}

    def save():
        (output / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')

    def run(name, argv, *, mode='0', diagnostic=None, strict='1'):
        argv = [str(arg) for arg in argv]
        started = time.monotonic()
        result = subprocess.run(argv, cwd=ROOT, env=dict(env, XCDE_DETERMINISTIC=mode, XCDE_STRICT=strict),
                                capture_output=True, text=True, errors='replace', timeout=30)
        text = result.stdout + result.stderr
        (output / (name + '.log')).write_text(text)
        passed = result.returncode == 0 if diagnostic is None else result.returncode != 0 and diagnostic in text
        receipt['commands'].append({'name': name, 'argv': argv, 'mode': mode, 'strict': strict,
                                    'returncode': result.returncode, 'required_diagnostic': diagnostic,
                                    'seconds': time.monotonic() - started, 'passed': passed})
        save()
        if not passed:
            raise RuntimeError(f'{name} failed; see {output / (name + ".log")}')

    flags = ['-std=gnu11', '-O1', '-UNDEBUG', '-march=x86_64_v2', '-ffp-contract=off', '-fno-fast-math',
             '-fno-strict-aliasing', '-D_FILE_OFFSET_BITS=64', '-I', str(PORT / 'runtime'), '-I', str(dependencies / 'include')]
    try:
        run('compiler', [sys.executable, '-m', 'ziglang', 'version'])
        for group, source in zip(args.groups, sources):
            binary = output / ('test_' + group + '.exe')
            run('compile-' + group, [sys.executable, '-m', 'ziglang', 'cc', *flags, source, *support,
                dependencies / 'lib/libSDL3.dll.a', '-ldbghelp', '-lwinmm', '-lshell32', '-lpsapi', '-o', binary])
            for mode in ('0', '1'):
                if group == 'am':
                    positives = ['', '--idle-set', '--domain-size-probe', '--blocked-close', '--shutdown-close',
                                 '--shutdown-renderer-wait', '--early-close'] + (['--timer-signal'] if mode == '1' else [])
                    negative = [(name, 'invalid idle-time extension') for name in ('--idle-range', '--idle-domain-range', '--idle-negative')]
                    malformed = ['--idle-short', '--idle-domain-short', '--idle-extra-data', '--idle-get-extra-data']
                    for extra in range(1, 9):
                        domain = '-domain' if extra == 8 else ''
                        malformed += [f'--idle{domain}-extra-{extra}', f'--idle{domain}-get-extra-{extra}']
                    negative += [(name, 'malformed idle-time cmd') for name in malformed]
                    negative += [('--strict-negative', 'ISelfController cmd 2147483647'),
                                 ('--strict-window-negative', 'IWindowController cmd 2147483647'),
                                 ('--malformed-boolean', 'flag 2 is not boolean'), ('--malformed-short', 'payload bytes, need 3'),
                                 ('--malformed-domain-short', 'payload bytes, need 3'),
                                 ('--malformed-domain-bounds', 'invalid domain payload bounds'),
                                 ('--shutdown-exit-old-order', 'shutdown_stage == 2')]
                    for scenario in positives:
                        run('am-' + mode + '-' + (scenario.lstrip('-') or 'events'), [binary] + ([scenario] if scenario else []), mode=mode)
                elif group == 'pctl':
                    run('pctl-' + mode + '-lifecycle', [binary], mode=mode)
                    negative = [('strict-factory', 'IParentalControlServiceFactory cmd 2147483647'),
                        ('strict-child', 'IParentalControlService cmd 2147483647'), ('preinit-query', 'query before client initialization'),
                        ('stale-init', 'closed or reset client'), ('stale-query', 'closed or reset client'),
                        ('factory-reserved', 'nonzero factory reserved input'), ('factory-domain-bounds', 'invalid domain payload bounds')]
                    negative += [(name, 'expected 8 payload bytes, send-PID=1') for name in ('factory-no-pid', 'factory-short',
                        'factory-domain-short', 'factory-extra', 'factory-handle', 'factory-buffer', 'factory-domain-object')]
                    negative += [(name, 'expected 0 payload bytes, send-PID=0') for name in ('init-data', 'init-domain-data',
                        'init-pid', 'init-handle', 'init-buffer', 'init-domain-object')]
                else:
                    for scenario in ('positive', 'null-buffers', 'oversized-buffers', 'unknown-nonstrict'):
                        run('prepo-' + mode + '-' + scenario, [binary, '0xb481', scenario], mode=mode,
                            strict='0' if scenario == 'unknown-nonstrict' else '1')
                    negative = [(name, '[prepo:u] malformed SaveReportWithUserOld2 envelope') for name in ('short',
                        'domain-short', 'extra-data', 'empty-data', 'missing-pid', 'missing-buffers', 'missing-event',
                        'missing-report', 'copy-handle', 'output-buffer', 'domain-object')]
                    negative += [('unknown', 'STRICT MODE: stopping at ipc'), ('unknown-domain', 'STRICT MODE: stopping at ipc'),
                                 ('domain-bounds', 'invalid domain payload bounds')]
                for scenario, diagnostic in negative:
                    argv = [binary, scenario] if group != 'prepo' else [binary, '0xb481', scenario]
                    run(group + '-' + mode + '-' + scenario.lstrip('-'), argv, mode=mode, diagnostic=diagnostic)
            print(group + ': native positive/negative fixtures PASS', flush=True)
        receipt['sources_after'] = snapshot()
        if receipt['sources_before'] != receipt['sources_after']:
            raise RuntimeError('Fixture source closure changed during testing')
        receipt['status'] = 'passed'
    finally:
        save()
        print(f'Native service evidence: {output}')


if __name__ == '__main__':
    main()
