#!/usr/bin/env python3
"""[XCDE] LINUX-009: synthetic actual runtime settings/binding parser checks."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import resource
import signal
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = 'tests/native/test_launcher_settings_linux.c'
CASES = ('roundtrip', 'selection', 'clamps', 'bindings-invalid', 'linux-fallback', 'menu-navigation')
GAPS = ('invalid-volume', 'invalid-fps', 'invalid-window', 'unknown-key', 'save-io-error')


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def sources():
    pending = [ROOT / FIXTURE, Path(__file__).resolve()]
    found = {}
    while pending:
        path = pending.pop().resolve()
        name = str(path.relative_to(ROOT))
        if name in found:
            continue
        found[name] = digest(path)
        if path.suffix in ('.c', '.h'):
            for include in re.findall(r'^\s*#\s*include\s+"([^"]+)"', path.read_text(), re.M):
                pending.append(path.parent / include)
    return dict(sorted(found.items()))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--gap-probes', action='store_true', help='also run known failing regressions; failures return nonzero')
    args = parser.parse_args()
    output = args.output.absolute()
    if output != output.resolve() or not output.is_relative_to(ROOT / 'runs') or output.exists():
        parser.error('--output must be an absent ordinary directory beneath Linux runs/')
    output.mkdir(parents=True)
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    compiler = ROOT / '.deps/bin/clang-cc'
    sysroot = Path((ROOT / '.deps/graphics/active-sysroot.txt').read_text().strip())
    lib = sysroot / 'usr/lib/x86_64-linux-gnu'
    env = {'PATH': '/usr/bin:/bin', 'LANG': 'C.UTF-8', 'LC_ALL': 'C.UTF-8', 'TMPDIR': str(output)}
    receipt = {'scope': 'Actual runtime settings_ini.c and bindings.c, real SDL scancode conversion; no window, game, human config or saves',
               'status': 'running', 'argv': sys.argv, 'cwd': str(ROOT), 'environment': env,
               'sources_before': sources(), 'compiler_sha256': digest(compiler),
               'sysroot': str(sysroot), 'sdl_sha256': digest(lib / 'libSDL3.so'), 'commands': []}

    def save():
        (output / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')

    def run(name, command, cwd):
        command = list(map(str, command)); start = time.monotonic()
        child = subprocess.Popen(command, cwd=cwd, env=env, stdin=subprocess.DEVNULL,
                                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT, start_new_session=True)
        failure = None
        try:
            log, _ = child.communicate(timeout=30)
        except BaseException as error:
            failure = type(error).__name__ + ': ' + str(error)
            try:
                os.killpg(child.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            log, _ = child.communicate(timeout=5)
            if not isinstance(error, subprocess.TimeoutExpired):
                raise
        finally:
            if child.poll() is None:
                os.killpg(child.pid, signal.SIGKILL); child.communicate(timeout=5)
        path = output / (name + '.log'); path.write_bytes(log)
        passed = child.returncode == 0 and failure is None
        receipt['commands'].append({'name': name, 'argv': command, 'cwd': str(cwd), 'exit': child.returncode,
                                    'seconds': time.monotonic() - start, 'failure': failure,
                                    'passed': passed, 'log_sha256': digest(path)})
        save(); print(name, 'PASS' if passed else 'FAIL', flush=True); return passed

    save()
    try:
        passed = run('compiler-version', [compiler, '--version'], ROOT)
        binary = output / 'test_launcher_settings_linux'
        compiled = run('compile', [compiler, '-O1', '-UNDEBUG', '-march=x86_64_v2',
                                  '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                                  '-Iruntime', '-I' + str(sysroot / 'usr/include'), '-pthread',
                                  FIXTURE, lib / 'libSDL3.so', '-Wl,-rpath,' + str(lib), '-lm', '-o', binary], ROOT)
        passed &= compiled
        if compiled:
            receipt['binary_sha256'] = digest(binary)
            for case in CASES + (GAPS if args.gap_probes else ()):
                work = output / case; work.mkdir()
                passed &= run(case, [binary, case], work)
        receipt['sources_after'] = sources()
        passed &= receipt['sources_after'] == receipt['sources_before']
        receipt['status'] = 'passed' if passed else 'failed'
    except BaseException as error:
        receipt.update(status='error', error=type(error).__name__ + ': ' + str(error))
        raise
    finally:
        save()
    print(receipt['status'], output / 'receipt.json'); return 0 if passed else 1


if __name__ == '__main__':
    sys.exit(main())
