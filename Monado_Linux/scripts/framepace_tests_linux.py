#!/usr/bin/env python3
"""Test actual framepace.c with synthetic memory/time; no game, GPU, saves or replay."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import resource
import signal
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
SOURCE = 'tests/native/test_framepace_linux.c'


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def snapshot():
    pending = [ROOT / SOURCE, Path(__file__).resolve()]
    found = {}
    while pending:
        path = pending.pop().resolve()
        name = str(path.relative_to(ROOT))
        if name in found:
            continue
        found[name] = digest(path)
        if path.suffix in ('.c', '.h'):
            for include in re.findall(r'^\s*#\s*include\s+"([^"]+)"', path.read_text(), re.M):
                target = (path.parent / include).resolve()
                if not target.is_file():
                    raise RuntimeError(f'local include missing: {target}')
                pending.append(target)
    return dict(sorted(found.items()))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, help='fresh directory under Monado_Linux/runs')
    args = parser.parse_args()
    if sys.platform != 'linux' or platform.machine() != 'x86_64':
        parser.error('native Linux x86-64 required')
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    os.umask(0o077)
    runs = ROOT / 'runs'
    if runs.is_symlink():
        parser.error('runs must not be a symlink')
    runs.mkdir(exist_ok=True)
    if args.output:
        output = args.output.absolute()
        if output != output.resolve() or not output.is_relative_to(runs) or output.exists():
            parser.error('--output must be fresh and non-symlink under Monado_Linux/runs')
        output.mkdir(parents=True)
    else:
        output = Path(tempfile.mkdtemp(prefix='linux-framepace.', dir=runs))
    compiler = ROOT / '.deps/bin/clang-cc'
    env = {'PATH': '/usr/bin:/bin', 'LANG': 'C.UTF-8', 'LC_ALL': 'C.UTF-8', 'TMPDIR': str(output)}
    receipt = {'scope': __doc__, 'status': 'running', 'cwd': str(ROOT), 'argv': sys.argv,
               'host': list(platform.uname()), 'sources_before': snapshot(), 'commands': [],
               'compiler': str(compiler), 'compiler_sha256': digest(compiler), 'artifacts': {},
               'limitations': ['Target interval/period, debounce, GUI/repeat delta helpers only; not the guest wait loop or observed frame rate',
                               'Synthetic host time, memory and page protection; no runtime thread races exercised',
                               'Follower thresholds, renderer, generated GUI/repeat hooks, guest consumers and real cutscene classification not validated']}

    def save():
        (output / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')

    def run(name, argv, overrides=None, expected=0, marker=None, artifact=None):
        child_env = dict(env, **(overrides or {}))
        row = {'name': name, 'argv': list(map(str, argv)), 'cwd': str(ROOT), 'env': child_env,
               'timeout_seconds': 60, 'expected_exit': expected, 'required_output': marker}
        receipt['commands'].append(row); save()
        begin = time.monotonic()
        child = subprocess.Popen(row['argv'], cwd=ROOT, env=child_env, stdin=subprocess.DEVNULL,
                                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT, start_new_session=True)
        timed_out = False
        try:
            stdout, _ = child.communicate(timeout=60)
        except (subprocess.TimeoutExpired, KeyboardInterrupt):
            timed_out = True
            os.killpg(child.pid, signal.SIGKILL)
            stdout, _ = child.communicate(timeout=5)
        (output / (name + '.log')).write_bytes(stdout)
        row.update(exit=child.returncode, seconds=time.monotonic() - begin, timed_out=timed_out,
                   passed=not timed_out and child.returncode == expected and
                   (marker is None or marker.encode() in stdout))
        if artifact and row['passed']:
            receipt['artifacts'][artifact.name] = digest(artifact)
        save()
        print(name, 'PASS' if row['passed'] else 'FAIL', flush=True)
        if not row['passed']:
            raise RuntimeError(f'{name} failed; see {output / (name + ".log")}')

    save()
    try:
        run('compiler-version', [compiler, '--version'])
        binary = output / 'test_framepace_linux'
        flags = ['-O1', '-UNDEBUG', '-march=x86_64_v2', '-ffp-contract=off', '-fno-strict-aliasing',
                 '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections']
        run('compile', [compiler, *flags, SOURCE, '-lm', '-o', binary], artifact=binary)
        for fps in (30, 60, 120, 144, 165, 0):
            for scenario in ('basic', 'first-cutscene'):
                run(f'{scenario}-{fps}', [binary, scenario, fps], marker='PASS framepace')
        run('original-period-preserved', [binary, 'basic', 165, '.9125'], marker='PASS framepace')
        for scenario, overrides in (
                ('interval-override', {'XCDE_FPS_INTERVAL': '3', 'XCDE_FPS_PERIOD_SCALE': '.5'}),
                ('scale-override', {'XCDE_FPS_PERIOD_SCALE': '.25'}),
                ('delayed-cutscene', {'XCDE_FPS_AFTER_S': '.1', 'XCDE_FPS_AFTER_FRAMES': '8'}),
                ('disabled-override', {'XCDE_FPS_INTERVAL': '-1'})):
            run(scenario, [binary, scenario, 165], overrides, marker='PASS framepace')
        for call in (1, 2):
            run(f'protect-failure-{call}', [binary, 'protect-failure', 165, call],
                expected=-signal.SIGABRT, marker='cannot make limiter constant writable' if call == 1
                else 'cannot restore limiter constant protection')
        for fps in (30, 165, 0):
            run(f'gui-valid-{fps}', [binary, 'gui-valid', fps], marker='PASS GUI elapsed delta')
        for scenario, values in (
                ('gui-bad-elapsed', ('-0.01', 'nan', 'inf', '-inf')),
                ('gui-bad-reference', ('0', '-30', 'nan', 'inf', '-inf'))):
            for index, value in enumerate(values):
                run(f'{scenario}-{index}', [binary, scenario, 165, value],
                    expected=-signal.SIGABRT, marker='[timing] invalid GUI frame clock:')
        for fps in (30, 120, 165, 0):
            run(f'repeat-valid-{fps}', [binary, 'repeat-valid', fps], marker='PASS repeat elapsed delta')
        for scenario, values in (
                ('repeat-bad-elapsed', ('-0.01', 'nan', 'inf', '-inf')),
                ('repeat-bad-reference', ('0', '-0', '-0.0333333', 'nan', 'inf', '-inf'))):
            for index, value in enumerate(values):
                run(f'{scenario}-{index}', [binary, scenario, 165, value],
                    expected=-signal.SIGABRT, marker='[timing] invalid repeat frame clock:')
        receipt['sources_after'] = snapshot()
        if receipt['sources_after'] != receipt['sources_before']:
            raise RuntimeError('source changed during tests; rerun with stable source')
        receipt['status'] = 'passed'
    except Exception as exc:
        receipt['status'] = 'failed'
        receipt['error'] = str(exc)
    finally:
        save()
    print(receipt['status'], output / 'receipt.json')
    return 0 if receipt['status'] == 'passed' else 1


if __name__ == '__main__':
    raise SystemExit(main())
