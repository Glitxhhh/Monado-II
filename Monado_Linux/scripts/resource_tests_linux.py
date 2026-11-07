#!/usr/bin/env python3
"""[XCDE] LINUX-004: synthetic resource/path regressions, never game execution."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import resource
import signal
import subprocess

ROOT = Path(__file__).resolve().parent.parent
SOURCES = [ROOT / ('runtime/' + name) for name in
           ('services/kbicons.c', 'services/fs.c', 'nvn/vulkan/precompile.c')]
TESTS = ('kbicons', 'save_mount', 'precompile_path')
SOURCES += [ROOT / f'tests/native/test_{name}_linux.c' for name in TESTS]
SOURCES += [Path(__file__).resolve()]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def limits():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    output = args.output.absolute()
    if output.resolve() != output or not output.is_relative_to(ROOT / 'runs'):
        parser.error('--output must be an ordinary new directory under Monado_Linux/runs')
    output.mkdir(parents=True, exist_ok=False)
    before = {str(p.relative_to(ROOT)): digest(p) for p in SOURCES}
    sysroot = Path((ROOT / '.deps/graphics/active-sysroot.txt').read_text().strip())
    lib = sysroot / 'usr/lib/x86_64-linux-gnu'
    base = [str(ROOT / '.deps/bin/clang-cc'), '-O1', '-march=x86_64_v2',
            '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
            '-I' + str(ROOT / 'runtime'), '-I' + str(ROOT / 'third_party'),
            '-I' + str(sysroot / 'usr/include'), '-L' + str(lib), '-lm', '-lz', '-pthread',
            str(lib / 'libSDL3.so'), '-Wl,-rpath,' + str(lib)]
    receipt = {'status': 'running', 'sources': before, 'sysroot': str(sysroot), 'commands': []}
    env = {key: value for key, value in os.environ.items()
           if not key.startswith(('XCDE_', 'LARECOMP_', 'MONADO_'))}

    def record(label, argv, cwd, expected=0):
        child = subprocess.Popen(argv, cwd=cwd, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                 start_new_session=True, preexec_fn=limits)
        timed_out = False
        try:
            stdout, stderr = child.communicate(timeout=45)
        except subprocess.TimeoutExpired:
            timed_out = True
            os.killpg(child.pid, signal.SIGKILL)
            stdout, stderr = child.communicate()
        log = output / (label + '.log')
        log.write_bytes(stdout + stderr)
        receipt['commands'].append({'label': label, 'argv': argv, 'cwd': str(cwd), 'exit': child.returncode,
                                    'timed_out': timed_out,
                                    'expected_exit': expected, 'log': str(log), 'log_sha256': digest(log)})
        (output / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
        if timed_out or child.returncode != expected:
            raise RuntimeError(f'{label}: exit {child.returncode}, expected {expected}, timeout={timed_out}; see {log}')

    try:
        for name in TESTS:
            record(name + '-compile', base + [str(ROOT / f'tests/native/test_{name}_linux.c'),
                                             '-o', str(output / name)], ROOT)
        scenarios = [('kbicons', ['font'], 0), ('kbicons', ['cache'], 0), ('kbicons', ['fallback'], 0),
                     ('save_mount', ['valid'], 0), ('save_mount', ['boundary'], 0),
                     ('save_mount', ['overflow'], 97), ('precompile_path', [], 0)]
        for name, arguments, expected in scenarios:
            label = name + '-' + (arguments[0] if arguments else 'overflow')
            cwd = output / label
            cwd.mkdir()
            record(label, [str(output / name), *arguments], cwd, expected)
            if label in ('save_mount-overflow', 'precompile_path-overflow') and list(cwd.iterdir()):
                raise RuntimeError(label + ': rejected root created state')
        receipt['sources_after'] = {str(p.relative_to(ROOT)): digest(p) for p in SOURCES}
        if before != receipt['sources_after']:
            raise RuntimeError('source changed during focused checks')
        receipt['status'] = 'passed'
    except Exception as error:
        receipt.update(status='failed', error=str(error))
        raise
    finally:
        (output / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
    print(f"3 compilations and 7 synthetic executions passed: {output / 'receipt.json'}")


if __name__ == '__main__':
    main()
