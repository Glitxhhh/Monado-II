#!/usr/bin/env python3
"""[XCDE] LINUX-018: actual activity lease and pipeline cache fault components.

Only authored subprocess fixtures and bytes are consumed. Vulkan is a mock API;
no loader, GPU, SDK, game input, Windows seed, human state or visible UI is used.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import sys

from boot_smoke_linux import bounded_process

ROOT = Path(__file__).resolve().parents[1]
FIXTURES = ('tests/native/test_runtime_activity_linux.c', 'tests/native/test_pipeline_cache_io_linux.c')


def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def sources():
    pending = [ROOT / name for name in (*FIXTURES, 'runtime/platform/linux_state_guard.c')]
    found = {}
    while pending:
        path = pending.pop().resolve()
        key = str(path.relative_to(ROOT))
        if key in found:
            continue
        found[key] = digest(path)
        for include in re.findall(r'^\s*#\s*include\s+"([^"]+)"', path.read_text(), re.M):
            target = (path.parent / include).resolve()
            if not target.is_file():
                raise RuntimeError('missing fixture header: ' + str(target))
            pending.append(target)
    for name in ('scripts/precompile_platform_tests_linux.py', 'scripts/boot_smoke_linux.py',
                 'scripts/platform_tests_linux.py', 'scripts/run_linux.py', 'runtime/host/main_boot.c'):
        found[name] = digest(ROOT / name)
    return dict(sorted(found.items()))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True, help='fresh ignored ordinary runs/ directory')
    args = parser.parse_args(argv)
    output = args.output.absolute()
    if output != output.resolve() or not output.is_relative_to(ROOT / 'runs') or output.exists():
        parser.error('--output must be a fresh ordinary directory below Monado_Linux/runs')
    os.umask(0o077)
    output.mkdir(parents=True)
    cc = ROOT / '.deps/bin/clang-cc'
    sysroot = Path((ROOT / '.deps/graphics/active-sysroot.txt').read_text().strip())
    marker = sysroot / '.monado-deps.json'
    lock = ROOT / 'graphics-deps-linux.lock.json'
    if not sysroot.is_relative_to(ROOT / '.deps/graphics/sysroots') or json.loads(marker.read_text()).get('lock_sha256') != digest(lock):
        parser.error('graphics header sysroot does not match the pinned local lock')
    dependencies = {str(path): digest(path) for path in (cc, marker, lock,
                    sysroot / 'usr/include/vulkan/vulkan.h', sysroot / 'usr/include/vulkan/vulkan_core.h')}
    environment = {'PATH': '/usr/bin:/bin', 'LANG': 'C.UTF-8', 'LC_ALL': 'C.UTF-8',
                   'TMPDIR': str(output), 'PYTHONDONTWRITEBYTECODE': '1'}
    receipt = {'scope': __doc__.split('\n\n', 1)[1].strip(), 'status': 'running',
               'started_utc': datetime.now(timezone.utc).isoformat(), 'cwd': str(ROOT),
               'argv': sys.argv, 'host': list(platform.uname()), 'sources_before': sources(),
               'dependencies': dependencies, 'commands': [], 'artifacts': {}, 'errors': []}
    main_source = (ROOT / 'runtime/host/main_boot.c').read_text().split('int main(', 1)[1].split('#else', 1)[0]
    order = [main_source.index(name) for name in
             ('rt_paths_init_linux(', 'rt_linux_state_guard(', 'rt_linux_activity_acquire(', 'plat_init(')]
    receipt['startup_order_source_inspection'] = {
        'scope': 'Source inspection only, not a game or boot execution',
        'before_platform_init': order == sorted(order),
        'sequence': ['rt_paths_init_linux', 'rt_linux_state_guard', 'rt_linux_activity_acquire', 'plat_init']}
    for name in receipt['sources_before']:
        target = output / 'source-snapshot' / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes((ROOT / name).read_bytes())

    def save():
        temporary = output / 'receipt.json.tmp'
        temporary.write_text(json.dumps(receipt, indent=2) + '\n')
        temporary.replace(output / 'receipt.json')

    def run(name, command, *, seconds=15, artifact=None):
        command = list(map(str, command))
        item = {'name': name, 'argv': command, 'cwd': str(ROOT), 'environment': environment,
                'seconds_limit': seconds}
        receipt['commands'].append(item); save()
        completion = {}
        failure = None
        try:
            bounded_process(command, cwd=ROOT, env=environment, seconds=seconds, completion=completion)
        except BaseException as error:
            failure = error
        item.update(completion.get('record', {}))
        for stream in ('stdout', 'stderr'):
            path = output / (name + '.' + stream + '.log')
            path.write_bytes(completion.get(stream, b''))
            item[stream + '_sha256'] = digest(path)
        item['passed'] = (failure is None and item.get('exit') == 0 and not item.get('timed_out')
                          and not item.get('killed_owned_pids') and not item.get('remaining_owned_pids')
                          and not item.get('cleanup_errors'))
        if failure:
            item['error'] = str(failure)
        if artifact and item['passed']:
            receipt['artifacts'][str(artifact.relative_to(output))] = digest(artifact)
        save(); print(name, 'PASS' if item['passed'] else 'FAIL', flush=True)
        if isinstance(failure, (KeyboardInterrupt, SystemExit)):
            raise failure
        if not item['passed']:
            raise RuntimeError(name + ': inspect private stdout/stderr logs')

    save()
    try:
        if not receipt['startup_order_source_inspection']['before_platform_init']:
            raise RuntimeError('native startup source no longer acquires guarded activity before platform init')
        run('compiler-version', [cc, '--version'])
        flags = ['-O1', '-UNDEBUG', '-march=x86_64_v2', '-ffp-contract=off', '-fno-strict-aliasing',
                 '-Wall', '-Wextra', '-pthread', '-I' + str(sysroot / 'usr/include')]
        guard = ROOT / 'runtime/platform/linux_state_guard.c'
        for fixture, stem in zip(FIXTURES, ('activity', 'pipeline-cache')):
            binary = output / ('test-' + stem)
            run('compile-' + stem, [cc, *flags, ROOT / fixture, guard, '-o', binary], seconds=45, artifact=binary)
            cases = (('busy-release', 'owner-death', 'close-on-exec', 'symlink', 'hardlink', 'directory', 'long-path')
                     if stem == 'activity' else
                     ('success', 'relative-parent', 'absolute-parent', 'query-error', 'query-zero', 'fetch-error', 'fetch-zero', 'fetch-overflow',
                      'incomplete-once', 'incomplete-limit', 'oom', 'temp-open', 'cloexec-error',
                      'write-eintr', 'write-short', 'write-zero', 'write-error', 'file-fsync', 'file-close',
                      'rename-error', 'directory-open', 'directory-fsync', 'directory-close',
                      'null-device', 'null-cache', 'null-path', 'long-path'))
            for case in cases:
                state = output / ('state-' + stem + '-' + case); state.mkdir()
                run(stem + '-' + case, [binary, case, state])
    except BaseException as error:
        receipt['errors'].append(str(error) or type(error).__name__)
        if isinstance(error, (KeyboardInterrupt, SystemExit)):
            raise
    finally:
        receipt['sources_after'] = sources()
        receipt['dependencies_after'] = {name: digest(name) for name in dependencies}
        stable = receipt['sources_before'] == receipt['sources_after'] and dependencies == receipt['dependencies_after']
        if not stable:
            receipt['errors'].append('source or dependency changed during components')
        receipt.update(status='passed' if not receipt['errors'] else 'failed',
                       finished_utc=datetime.now(timezone.utc).isoformat())
        save()
    return 0 if receipt['status'] == 'passed' else 1


if __name__ == '__main__':
    sys.exit(main())
