#!/usr/bin/env python3
"""[XCDE] LINUX-017: actual temporal CPU policy and authored GLSL compiler tests; no Vulkan instance, game, UI or profile writes."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import resource
import shlex
import signal
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parent.parent
SOURCES = ('scripts/temporal_tests_linux.py', 'tests/native/test_temporal_history_linux.c',
           'runtime/nvn/vulkan/temporal.c', 'runtime/nvn/vulkan/temporal.h', 'runtime/nvn/vulkan/dlss.h',
           'tests/native/test_temporal_shaders_linux.c', 'runtime/nvn/vulkan/shaders.c', 'runtime/nvn/vulkan/shaders.h')
def digest(p):
    return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if platform.system() != 'Linux' or platform.machine() != 'x86_64': parser.error('Linux x86-64 required')
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    os.umask(0o077)
    parent = ROOT / 'runs'
    if parent.is_symlink(): parser.error('runs must not be a symlink')
    parent.mkdir(exist_ok=True)
    if args.output:
        out = args.output.absolute()
        if out != out.resolve() or not out.is_relative_to(parent) or out.exists(): parser.error('fresh non-symlink output under runs required')
        out.mkdir(parents=True)
    else: out = Path(tempfile.mkdtemp(prefix='linux-temporal.', dir=parent))
    env = {'PATH': '/usr/bin:/bin', 'LANG': 'C.UTF-8', 'LC_ALL': 'C.UTF-8', 'TMPDIR': str(out)}
    cc = ROOT / '.deps/bin/clang-cc'
    sysroot = Path((ROOT / '.deps/graphics/active-sysroot.txt').read_text().strip()).resolve()
    receipt = {'scope': 'actual temporal CPU policy and authored GLSL shaderc compile/reflect; no GPU/game/UI', 'status': 'running', 'cwd': str(ROOT),
               'host': list(platform.uname()), 'compiler': str(cc), 'compiler_sha256': digest(cc), 'sysroot': str(sysroot),
               'sources_before': {p: digest(ROOT / p) for p in SOURCES}, 'commands': []}
    def save(): (out / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
    def run(name, argv):
        argv = list(map(str, argv))
        row = {'name': name, 'argv': argv, 'cwd': str(ROOT), 'env': env, 'timeout_seconds': 60}
        receipt['commands'].append(row); save()
        start = time.monotonic()
        p = subprocess.Popen(argv, cwd=ROOT, env=env, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, start_new_session=True)
        row['pid'] = p.pid; save()
        try: output, _ = p.communicate(timeout=60)
        except (subprocess.TimeoutExpired, KeyboardInterrupt):
            os.killpg(p.pid, signal.SIGKILL); output, _ = p.communicate(timeout=5)
            row['timed_out_or_interrupted'] = True
        row.update(exit=p.returncode, seconds=time.monotonic() - start)
        log = out / (name + '.log'); log.write_bytes(output); row['log'] = str(log); save()
        if p.returncode: raise RuntimeError(f'{name} failed: {output.decode(errors="replace")}')
        return output.decode()
    try:
        flags = shlex.split(run('pkg-cflags', [ROOT / '.deps/bin/pkg-config-graphics', '--cflags', 'sdl3', 'vulkan']))
        flags += ['-O1', '-UNDEBUG', '-march=x86_64_v2', '-ffp-contract=off', '-fno-strict-aliasing',
                  '-ffunction-sections', '-fdata-sections', '-I' + str(ROOT / 'runtime')]
        exe = out / 'test_temporal_history_linux'
        run('compile', [cc, *flags, ROOT / SOURCES[1], '-Wl,--gc-sections', '-lm', '-o', exe])
        print(run('test', [exe]), end='')
        receipt['binary_sha256'] = digest(exe)
        shader_flags = shlex.split(run('shader-pkg-flags', [ROOT / '.deps/bin/pkg-config-graphics', '--cflags', '--libs', 'shaderc']))
        shaders = out / 'test_temporal_shaders_linux'
        run('compile-shaders', [cc, *flags, ROOT / 'tests/native/test_temporal_shaders_linux.c', *shader_flags,
            '-Wl,--gc-sections', '-Wl,-rpath,' + str(sysroot / 'usr/lib/x86_64-linux-gnu'), '-o', shaders])
        print(run('test-shaders', [shaders]), end='')
        receipt['shader_binary_sha256'] = digest(shaders)
        receipt['sources_after'] = {p: digest(ROOT / p) for p in SOURCES}
        if receipt['sources_before'] != receipt['sources_after']: raise RuntimeError('sources changed during test')
        receipt['status'] = 'passed'
    except Exception as e:
        receipt['status'] = 'failed'; receipt['error'] = str(e); raise
    finally:
        save(); print(out)
if __name__ == '__main__': main()
