#!/usr/bin/env python3
"""Build native source-only FSR4 Lite or import explicitly prepared local models.

No downloads, game files, drivers, package installation, Proton or automatic
upscaler selection. Native SDK capture/translation preparation remains separate.
"""
from __future__ import annotations
import argparse
import contextlib
import hashlib
import io
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT.parent / 'tools'))
import windows_file_guard as guard
sys.path.insert(0, str(ROOT / 'tools'))
from pe_symbols_windows import Image
LOCK = ROOT / 'fsr4-windows.lock.json'
NAME = 'monado-fsr4.dll'
FLAGS = ['-std=c++20', '-O1', '-UNDEBUG', '-march=x86_64_v2', '-ffp-contract=off', '-fno-fast-math', '-fno-strict-aliasing']
MAX_SPIRV = 16 * 1024 * 1024

def sha(data): return hashlib.sha256(data).hexdigest()
def encoded(value): return (json.dumps(value, indent=2, ensure_ascii=False) + '\n').encode('utf8')

def document(data):
    def pairs(values):
        result = {}
        for key, value in values:
            if key in result: raise ValueError('Duplicate FSR4 receipt key')
            result[key] = value
        return result
    value = json.loads(data, object_pairs_hook=pairs)
    if not isinstance(value, dict): raise ValueError('Expected FSR4 receipt object')
    return value

@contextlib.contextmanager
def sources():
    with guard.locked_directory(ROOT) as root, contextlib.ExitStack() as stack:
        lock_bytes = root.read_regular(LOCK.name, max_bytes=1024*1024)
        lock = document(lock_bytes)
        if lock.get('schema') != 1 or lock.get('abi') != 2 or lock.get('format') != 'windows-amd64-pe':
            raise ValueError('Unsupported Windows FSR4 source lock')
        for name, item in lock['files'].items():
            stream = stack.enter_context(root.open_regular(name, max_bytes=4*1024*1024))
            data = stream.read(4*1024*1024+1)
            if len(data) != item['bytes'] or sha(data) != item['sha256']:
                raise ValueError('FSR4 reviewed source identity changed: ' + name)
        yield lock, lock_bytes

@contextlib.contextmanager
def fresh_output(output, kind):
    output = Path(guard.canonical_path(output))
    base = ROOT / '.deps/fsr4' / kind
    if output.parent != base or output.name in ('', '.', '..'):
        raise ValueError('Fresh FSR4 output must be one child of ' + str(base))
    with guard.locked_directory(ROOT / '.deps') as deps:
        with deps.child('fsr4/' + kind, create=True) as parent:
            # Parent lease makes this exact fresh creation race safe; never reuse
            # or clean a previous preparation directory.
            output.mkdir()
            with parent.child(output.name, owner=True) as result:
                yield result

def progress(stage, message):
    print('MONADO_FSR4_PROGRESS ' + json.dumps({'stage': stage, 'message': message}), flush=True)

def build(output):
    with sources() as (lock, lock_bytes), fresh_output(output, 'build') as out:
        receipt = {'schema': 1, 'status': 'running', 'abi': 2, 'format': 'windows-amd64-pe',
                   'output': str(out.path), 'lock_sha256': sha(lock_bytes), 'recipe_sha256': sha(Path(__file__).read_bytes()),
                   'source_identity': lock['files'], 'scope': 'authored native component only; models/capture absent'}
        prefix_record = document(guard.read_regular(ROOT / '.deps/windows-deps.json', max_bytes=1024*1024))
        prefix = Path(guard.canonical_path(prefix_record['prefix']))
        compiler = ROOT / '.venv/Lib/site-packages/ziglang/zig.exe'
        with guard.open_regular(compiler) as stream:
            receipt['compiler_sha256'] = hashlib.file_digest(stream, 'sha256').hexdigest()
        command = [str(compiler), 'c++', *FLAGS, '-I'+str(prefix/'include'), '-I'+str(ROOT/'runtime/nvn/vulkan'),
                   str(ROOT/'third_party/fsr4_native/fsr411.cpp'), str(ROOT/'third_party/fsr4_native/monado_adapter.cpp'),
                   str(prefix/'lib/libvulkan-1.dll.a'), '-shared', '-o', str(out.path/NAME)]
        environment = os.environ.copy()
        environment['ZIG_LOCAL_CACHE_DIR'] = str(ROOT/'build/zig-cache')
        environment['ZIG_GLOBAL_CACHE_DIR'] = str(ROOT/'build/zig-global-cache')
        progress('build', 'Compiling the reviewed native Vulkan Lite component; no model data')
        started = time.monotonic()
        result = subprocess.run(command, cwd=ROOT, env=environment, stdin=subprocess.DEVNULL,
                                capture_output=True, timeout=180, creationflags=subprocess.CREATE_NO_WINDOW)
        out.write_new('build.log', result.stdout + result.stderr)
        receipt['command'] = {'argv': command, 'exit': result.returncode, 'seconds': time.monotonic()-started}
        if result.returncode:
            receipt['status'] = 'failed';out.write_new('receipt.json', encoded(receipt))
            raise RuntimeError('Native FSR4 build failed; see ' + str(out.path/'build.log'))
        with out.open_regular(NAME, max_bytes=64*1024*1024, owner=True) as stream:
            data = stream.read(); image = Image(out.path/NAME)
            if image.exports() != ['monado_fsr4_get_api']:
                raise ValueError('Unexpected native FSR4 exports')
            imports = image.imports()
            if any(n.lower() not in ('vulkan-1.dll', 'kernel32.dll', 'ntdll.dll', 'ucrtbase.dll', 'msvcrt.dll') and
                   not n.lower().startswith(('api-ms-win-', 'ext-ms-win-')) for n in imports):
                raise ValueError('Unexpected native FSR4 runtime dependency')
        out.write_new(NAME+'.sha256', (sha(data)+'\n').encode('ascii'))
        # Corresponding reviewed component source + recipe/license, no private
        # models, compiled game code or generated shaders.
        archive = io.BytesIO()
        with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED) as z:
            for name in lock['files']: z.writestr('Monado_Windows/'+name, guard.read_regular(ROOT/name, max_bytes=4*1024*1024))
            z.writestr('Monado_Windows/'+LOCK.name, lock_bytes)
            z.writestr('tools/windows_file_guard.py',guard.read_regular(ROOT.parent/'tools/windows_file_guard.py',max_bytes=1024*1024))
            z.writestr('LICENSE',guard.read_regular(ROOT.parent/'LICENSE',max_bytes=1024*1024))
            z.writestr('Monado_Windows/LICENSE',guard.read_regular(ROOT/'LICENSE',max_bytes=1024*1024))
        out.write_new('FSR4-component-source.zip', archive.getvalue())
        receipt.update(status='passed', dll_sha256=sha(data), imports=imports, models_prepared=False)
        out.write_new('receipt.json', encoded(receipt))
        progress('complete', 'Native component built; local models and GPU qualification are still required')
        return receipt

def verify_model_record(record, lock):
    if (type(record.get('schema')) is not int or record.get('schema') != 1 or record.get('status') != 'passed' or record.get('model') != 'int8_lite' or
            record.get('model_identity') != lock['model_identity'] or record.get('sdk') != lock['sdk']):
        raise ValueError('Prepared Lite SDK/model identity differs from the accepted source lock')
    required = {'lite/'+tier+'/'+leaf for tier in lock['sets'] for leaf in
                [*[name+'.spv' for name in lock['passes']], 'initializer.bin']}
    if not isinstance(record.get('resources'),dict) or set(record['resources']) != required: raise ValueError('All four complete Lite sets are required')
    for name, item in record['resources'].items():
        if not isinstance(item,dict): raise ValueError('Expected a prepared Lite resource object')
        size = item.get('bytes')
        if (not isinstance(size, int) or isinstance(size, bool) or not isinstance(item.get('sha256'),str) or not re.fullmatch(r'[0-9a-f]{64}', item['sha256']) or
                (size != 27392 if name.endswith('.bin') else not 20 <= size <= MAX_SPIRV or size % 4)):
            raise ValueError('Malformed prepared Lite resource: '+name)
    return required

def import_models(source, output):
    with sources() as (lock, _), guard.locked_directory(source) as original:
        record = document(original.read_regular('receipt.json', max_bytes=4*1024*1024))
        required = verify_model_record(record, lock)
        with fresh_output(output, 'assets') as out:
            resources = {}
            for index, name in enumerate(sorted(required)):
                item = record['resources'][name]
                data = original.read_regular(name, max_bytes=MAX_SPIRV)
                if len(data) != item['bytes'] or sha(data) != item['sha256']:
                    raise ValueError('Prepared model content differs: '+name)
                if name.endswith('.spv') and data[:4] != b'\x03\x02\x23\x07':
                    raise ValueError('Prepared SPIR-V magic invalid: '+name)
                leaf = Path(name)
                with out.child(leaf.parent.as_posix(), create=True, owner=True) as directory:
                    directory.write_new(leaf.name, data)
                    directory.write_new(leaf.name+'.sha256', (sha(data)+'\n').encode('ascii'))
                resources[name] = {'bytes':len(data), 'sha256':sha(data)}
                progress('import', f'Verified local Lite file {index+1}/52')
            native = {'schema':1, 'status':'passed', 'model':'int8_lite', 'model_identity':lock['model_identity'],
                      'sdk':lock['sdk'], 'resources':resources, 'scope':'explicit local prepared asset import; no capture or GPU qualification'}
            out.write_new('receipt.json', encoded(native))
            # Written LAST. Runtime still validates each exact file/size/hash
            # under held native handles and never trusts an ambient environment.
            out.write_new('models.id', (lock['model_identity']+'\n').encode('ascii'))
            progress('complete', 'Local Lite models imported; selected upscaler is unchanged')
            return native

def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    operation = parser.add_mutually_exclusive_group(required=True)
    operation.add_argument('--build', action='store_true')
    operation.add_argument('--import-assets', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        result = build(args.output) if args.build else import_models(args.import_assets, args.output)
        print(json.dumps(result, indent=2));return 0
    except (OSError, ValueError, KeyError, RuntimeError, subprocess.SubprocessError) as error:
        parser.exit(1, str(error)+'\n')

if __name__ == '__main__': main()
