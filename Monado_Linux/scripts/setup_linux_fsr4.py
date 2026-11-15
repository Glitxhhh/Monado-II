#!/usr/bin/env python3
"""Build and verify the optional GPL native Vulkan FSR 4 Lite component.

Only reviewed, hash-locked source is compiled. No game data, SDK DLL, model,
network access, package installation or Proton invocation is part of this step.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import tarfile
import time

ROOT = Path(__file__).resolve().parents[1]
LOCK = ROOT / 'fsr4-linux.lock.json'
NAME = 'libmonado-fsr4.so'
ABI = 2


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def ordinary(path, boundary=None, fresh=False):
    path = Path(path).absolute()
    if path != path.resolve() or (boundary and not path.is_relative_to(Path(boundary).resolve())):
        raise ValueError('ordinary path within owned boundary required: ' + str(path))
    if fresh and path.exists():
        raise ValueError('refusing to overwrite existing output: ' + str(path))
    return path


def verify_source():
    lock = json.loads(LOCK.read_text())
    if lock.get('schema') != 1 or lock.get('abi') != ABI:
        raise ValueError('unsupported FSR4 source lock')
    for name, item in lock['files'].items():
        p = ordinary(ROOT / name, ROOT)
        if not p.is_file() or p.stat().st_size != item['bytes'] or digest(p) != item['sha256']:
            raise ValueError('FSR4 source identity changed: ' + name)
    return lock


def resource(path):
    path = ordinary(path)
    return {'path': str(path), 'bytes': path.stat().st_size, 'sha256': digest(path)}


def build(output):
    lock = verify_source()
    out = ordinary(output, ROOT / '.deps/fsr4/build', fresh=True)
    out.mkdir(parents=True)
    (out / 'logs').mkdir()
    src = ROOT / 'third_party/fsr4_native'
    sysroot = ordinary(Path((ROOT / '.deps/graphics/active-sysroot.txt').read_text().strip()))
    lib = sysroot / 'usr/lib/x86_64-linux-gnu'
    cc = ROOT / '.deps/bin/clang-cxx'
    env = {'PATH': '/usr/bin:/bin', 'LANG': 'C.UTF-8', 'LC_ALL': 'C.UTF-8',
           'TMPDIR': str(out), 'LD_LIBRARY_PATH': str(lib)}
    receipt = {'schema': 1, 'status': 'running', 'abi_version': ABI,
               'source_commit': lock['source_commit'], 'lock_sha256': digest(LOCK),
               'recipe_sha256': digest(__file__), 'output': str(out), 'commands': [],
               'sysroot': str(sysroot), 'sysroot_marker_sha256': digest(sysroot / '.monado-deps.json')}
    def save():
        (out / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
    def run(name, args):
        t = time.monotonic()
        process=subprocess.Popen(list(map(str,args)),cwd=ROOT,env=env,stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,stderr=subprocess.PIPE,start_new_session=True)
        try:stdout,stderr=process.communicate(timeout=180)
        finally:
            if process.poll() is None:
                os.killpg(process.pid,signal.SIGTERM)
                try:process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid,signal.SIGKILL);process.wait()
        (out / 'logs' / (name + '.stdout.log')).write_bytes(stdout)
        (out / 'logs' / (name + '.stderr.log')).write_bytes(stderr)
        receipt['commands'].append({'name': name, 'argv': list(map(str, args)), 'exit': process.returncode,
                                    'seconds': time.monotonic() - t})
        save()
        if process.returncode:
            raise RuntimeError(name + ' failed: ' + stderr.decode(errors='replace')[-3000:])
    try:
        save()
        flags = ['-target','x86_64-linux-gnu.2.39','-std=c++20', '-O2', '-fPIC', '-fvisibility=hidden', '-ffp-contract=off',
                 '-I' + str(sysroot / 'usr/include'), '-I' + str(ROOT / 'runtime/nvn/vulkan')]
        objects = []
        for name in ('fsr411', 'monado_adapter'):
            obj = out / (name + '.o')
            run('compile-' + name, [cc, *flags, '-c', src / (name + '.cpp'), '-o', obj])
            objects.append(obj)
        (out / 'exports.map').write_text('{ global: monado_fsr4_get_api; local: *; };\n')
        run('link', [cc, '-target','x86_64-linux-gnu.2.39','-shared', *objects, lib / 'libvulkan.so', '-Wl,-z,defs',
                     '-Wl,--version-script,' + str(out / 'exports.map'), '-o', out / NAME])
        run('exports', ['nm', '-D', '--defined-only', out / NAME])
        symbols = {line.split()[-1] for line in (out / 'logs/exports.stdout.log').read_text().splitlines() if line.split()}
        if symbols != {'monado_fsr4_get_api'}:
            raise ValueError('unexpected FSR4 exports: ' + str(symbols))
        run('dynamic', ['readelf', '-d', out / NAME])
        licenses = [('FSR4-GPL-2.0-or-later.txt', src / 'LICENSE'),
                    ('FSR4-AMD-SDK-LICENSE.md', src / 'LICENSE-AMD-SDK.md')]
        for name, path in licenses:
            shutil.copyfile(path, out / name)
        # Corresponding component source is small and contains no model/game data.
        source_archive = out / 'FSR4-component-source.tar.gz'
        with tarfile.open(source_archive, 'w:gz') as tf:
            for name in sorted(lock['files']):
                tf.add(ROOT / name, arcname=name, recursive=False)
            tf.add(__file__, arcname='scripts/setup_linux_fsr4.py', recursive=False)
            tf.add(LOCK, arcname=LOCK.name, recursive=False)
        verify_source()
        receipt['runtime_resources'] = {name: resource(out / name) for name in
            (NAME, *[name for name, _ in licenses], source_archive.name)}
        receipt['status'] = 'passed'
        save()
        marker = ordinary(ROOT / '.deps/fsr4/active-component.json', ROOT / '.deps/fsr4')
        temp = marker.with_name('.active-component.' + str(os.getpid()))
        temp.write_text(json.dumps({'schema': 1, 'receipt_path': str(out / 'receipt.json')}) + '\n')
        os.replace(temp, marker)
    except BaseException as error:
        receipt.update(status='failed', error=str(error))
        raise
    finally:
        save()
    return verify_existing()


def verify_existing():
    lock = verify_source()
    marker = ordinary(ROOT / '.deps/fsr4/active-component.json', ROOT / '.deps/fsr4')
    if not marker.is_file():
        raise ValueError('optional native FSR4 component has not been built')
    selection = json.loads(marker.read_text())
    path = ordinary(selection['receipt_path'], ROOT / '.deps/fsr4/build')
    receipt = json.loads(path.read_text())
    if (selection.get('schema') != 1 or receipt.get('status') != 'passed' or
        receipt.get('abi_version') != ABI or receipt.get('source_commit') != lock['source_commit'] or
        receipt.get('lock_sha256') != digest(LOCK) or receipt.get('recipe_sha256') != digest(__file__)):
        raise ValueError('optional FSR4 component recipe or identity changed')
    out = path.parent
    for name, item in receipt['runtime_resources'].items():
        p = ordinary(out / name, out)
        if p.name != name or not p.is_file() or resource(p) != item:
            raise ValueError('FSR4 component resource changed: ' + name)
    sysroot = ordinary(receipt['sysroot'])
    if digest(sysroot / '.monado-deps.json') != receipt['sysroot_marker_sha256']:
        raise ValueError('FSR4 graphics dependency identity changed')
    library = receipt['runtime_resources'][NAME]
    license = receipt['runtime_resources']['FSR4-GPL-2.0-or-later.txt']
    return {'library_path': library['path'], 'library_sha256': library['sha256'],
            'license_path': license['path'], 'license_sha256': license['sha256'],
            'receipt_path': str(path), 'abi_version': ABI, 'source_commit': lock['source_commit'],
            'runtime_resources': receipt['runtime_resources'],
            'source_licenses': [v for n, v in receipt['runtime_resources'].items() if 'LICENSE' in n or 'GPL' in n]}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--build', action='store_true')
    ap.add_argument('--output', type=Path)
    args = ap.parse_args()
    try:
        if args.build and not args.output:
            raise ValueError('--build requires a fresh --output beneath .deps/fsr4/build')
        print(json.dumps(build(args.output) if args.build else verify_existing(), indent=2))
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        ap.exit(1, str(error) + '\n')


if __name__ == '__main__':
    main()
