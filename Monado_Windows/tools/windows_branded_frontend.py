"""Compile the authored branded Windows setup base with static SDL3/iconv.

The caller authenticates the reviewed source export and dependency receipt.
This helper retains ordinary input leases, every relink object/resource/archive,
and an exact relative link recipe. It never opens a visible window or the game.
"""
from __future__ import annotations

import contextlib
import ctypes
from ctypes import wintypes
import hashlib
import json
import os
from pathlib import Path
import sys

PORT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PORT.parent / 'tools'))
sys.path.insert(0, str(PORT / 'tools'))
import windows_file_guard as guard
try:
    from . import fsr4_process_windows as process
    from .pe_symbols_windows import Image
except ImportError:
    import fsr4_process_windows as process
    from pe_symbols_windows import Image

SOURCES = ('tools/builder/windows_builder.cpp',) + tuple(
    'third_party/imgui/' + name + '.cpp' for name in ('imgui', 'imgui_draw', 'imgui_tables', 'imgui_widgets')) + (
    'third_party/imgui/backends/imgui_impl_sdl3.cpp',
    'third_party/imgui/backends/imgui_impl_sdlrenderer3.cpp')
STATIC = ('libSDL3.a', 'libiconv.a')
SYSTEM_LIBRARIES = ('m', 'kernel32', 'user32', 'gdi32', 'winmm', 'imm32', 'ole32', 'oleaut32',
    'version', 'uuid', 'advapi32', 'setupapi', 'shell32', 'dinput8', 'bcrypt', 'cabinet', 'comctl32')
SYSTEM_DLLS = frozenset(name + '.dll' for name in SYSTEM_LIBRARIES if name not in ('m', 'uuid')) | {
    'ntdll.dll', 'msvcrt.dll', 'ucrtbase.dll'}
FLAGS = ('-std=c++17', '-O1', '-UNDEBUG', '-march=x86_64_v2', '-ffp-contract=off',
    '-fno-fast-math', '-fno-strict-aliasing', '-fvisibility=hidden', '-ffunction-sections', '-fdata-sections')


def encoded(value):
    return (json.dumps(value, sort_keys=True, indent=2) + '\n').encode('utf-8')


def _identity(path):
    with guard.open_regular(path, max_bytes=512 * 1024 * 1024) as stream:
        return {'bytes': os.fstat(stream.fileno()).st_size,
                'sha256': hashlib.file_digest(stream, 'sha256').hexdigest()}


def _compiler(explicit=None):
    candidates = [Path(explicit)] if explicit else [
        Path(sys.prefix) / 'Lib/site-packages/ziglang/zig.exe',
        PORT / '.venv/Lib/site-packages/ziglang/zig.exe',
        PORT / '.deps/python314/Lib/site-packages/ziglang/zig.exe']
    for candidate in candidates:
        if candidate.is_file():
            return Path(guard.canonical_path(candidate))
    raise ValueError('Pinned Zig 0.14.1 compiler is required for the native frontend')


def _system_cwd():
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    function = kernel.GetSystemDirectoryW
    function.argtypes = [wintypes.LPWSTR, wintypes.UINT]
    function.restype = wintypes.UINT
    buffer = ctypes.create_unicode_buffer(32768)
    count = function(buffer, len(buffer))
    if not 0 < count < len(buffer):
        raise ctypes.WinError(ctypes.get_last_error())
    return Path(buffer.value)


def _environment(output, compiler, cwd):
    environment = {name: value for name, value in os.environ.items()
                   if name.upper() in ('SYSTEMROOT', 'WINDIR', 'COMSPEC')}
    environment.update(PATH=os.pathsep.join((str(Path(sys.executable).parent), str(compiler.parent), str(cwd))),
        TMP=str(output / 'tmp'), TEMP=str(output / 'tmp'),
        ZIG_LOCAL_CACHE_DIR=str(output / 'zig-local'), ZIG_GLOBAL_CACHE_DIR=str(output / 'zig-global'),
        PYTHONDONTWRITEBYTECODE='1', PYTHONUTF8='1')
    return environment


def _hold_tree(root, leases):
    """Hold ordinary source/header/asset inputs and every traversed ancestor."""
    count = 0
    held = {}
    def unreadable(error):
        raise OSError('Cannot enumerate the complete frontend input tree') from error
    for directory, children, names in os.walk(root, followlinks=False, onerror=unreadable):
        current = leases.enter_context(guard.locked_directory(directory))
        for name in sorted(children):
            # Enter before os.walk can enumerate the child, rejecting junctions.
            leases.enter_context(current.child(name))
        for name in sorted(names):
            count += 1
            if count > 4096:
                raise ValueError('Frontend input tree exceeds the reviewed member bound')
            stream = leases.enter_context(current.open_regular(name, max_bytes=8 * 1024 * 1024))
            path = Path(directory) / name
            held[path] = {'bytes': os.fstat(stream.fileno()).st_size,
                          'sha256': hashlib.file_digest(stream, 'sha256').hexdigest()}
    return held


def _record(path, base, **extra):
    return dict(path=path.relative_to(base).as_posix(), **_identity(path), **extra)


def _imports(executable):
    imports = sorted(Image(executable).imports(), key=str.lower)
    unexpected = [name for name in imports if name.lower() not in SYSTEM_DLLS and not
                  (name.lower().startswith('api-ms-win-crt-') and name.lower().endswith('.dll'))]
    if unexpected:
        raise ValueError('Branded frontend has non-system startup DLL dependencies: ' + ', '.join(unexpected))
    return imports


def compile_native_frontend(reviewed_port, prefix, output, label, *, compiler=None):
    """Create output/frontend exclusively; output itself is caller-owned/existing.

    Return JSON-compatible commands/input/output records, base_exe and an exact
    relative relink recipe. No output artifact or earlier attempt is overwritten.
    """
    if os.name != 'nt':
        raise OSError('The branded frontend is built natively on Windows')
    reviewed_port, prefix, output = (Path(guard.canonical_path(value)) for value in (reviewed_port, prefix, output))
    if (not isinstance(label, str) or not label or len(label) > 256 or
            any(ord(char) < 32 or ord(char) == 127 for char in label)):
        raise ValueError('A bounded printable native Windows baseline label is required')
    compiler = _compiler(compiler)
    build = output / 'frontend'
    commands = []
    with contextlib.ExitStack() as leases:
        leases.enter_context(guard.locked_directory(output, owner=True))
        leases.enter_context(guard.locked_directory(reviewed_port))
        leases.enter_context(guard.locked_directory(prefix))
        leases.enter_context(guard.open_regular(compiler, max_bytes=512 * 1024 * 1024))
        source_inputs = {}
        for relative in ('tools/builder', 'tools/launcher', 'third_party/imgui'):
            source_inputs.update(_hold_tree(reviewed_port / relative, leases))
        sdl_inputs = _hold_tree(prefix / 'include/SDL3', leases)
        compiler_identity = _identity(compiler)
        inventory_file = leases.enter_context(guard.open_regular(prefix / 'content-inventory.json', max_bytes=16 * 1024 * 1024))
        inventory_raw = inventory_file.read()
        inventory_identity = {'bytes': len(inventory_raw), 'sha256': hashlib.sha256(inventory_raw).hexdigest()}
        def unique(pairs):
            document = {}
            for key, value in pairs:
                if key in document:
                    raise ValueError('Duplicate dependency inventory key')
                document[key] = value
            return document
        inventory = json.loads(inventory_raw, object_pairs_hook=unique)
        if not isinstance(inventory, dict):
            raise ValueError('Expected the checked native dependency content inventory')
        for path, identity in sdl_inputs.items():
            if inventory.get(path.relative_to(prefix).as_posix()) != identity:
                raise ValueError('SDL3 header differs from native dependency inventory')
        static_bytes = {}
        for name in STATIC:
            stream = leases.enter_context(guard.open_regular(prefix / 'lib' / name, max_bytes=32 * 1024 * 1024))
            data = stream.read()
            if inventory.get('lib/' + name) != {'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}:
                raise ValueError('Static frontend archive differs from native dependency inventory: ' + name)
            static_bytes[name] = data
        build.mkdir()
        target = leases.enter_context(guard.locked_directory(build, owner=True))
        for name in ('objects', 'static-libs', 'logs', 'tmp', 'zig-local', 'zig-global'):
            (build / name).mkdir()
        for name, data in static_bytes.items():
            target.write_new('static-libs/' + name, data)
        cwd = _system_cwd()
        environment = _environment(build, compiler, cwd)

        def run(argv, timeout=600):
            record = process.run(argv, cwd=cwd, log=build / 'logs' / ('command-' + str(len(commands)) + '.txt'),
                environment=environment, timeout=timeout)
            commands.append(record)
            if record['exit']:
                raise RuntimeError('Branded frontend command failed; inspect ' + record['log'])
            return Path(record['log']).read_text('utf-8', errors='replace')

        version = run([compiler, 'version'], timeout=30).strip()
        if version != '0.14.1':
            raise ValueError('Branded frontend requires the pinned Zig 0.14.1 compiler')
        resource_text = run([sys.executable, '-I', '-S', '-B', '-X', 'utf8',
            reviewed_port / 'tools/launcher/theme_resource.py', '--assets',
            reviewed_port / 'tools/launcher/assets', '--output', build / 'resources'])
        resources = json.loads(resource_text)
        if resources.get('features') != {'fonts': True, 'artwork': True}:
            raise ValueError('Branded frontend requires both reviewed embedded fonts and artwork')
        resource = build / 'objects/builder.res'
        run([compiler, 'rc', '/I', reviewed_port / 'tools/builder', '/fo', resource,
             reviewed_port / 'tools/builder/builder.rc'])
        defines = ['-DMONADO_BRANDED_SETUP=1', '-DSDL_MAIN_HANDLED=1', '-DUNICODE=1', '-D_UNICODE=1',
            '-DMONADO_LAUNCHER_RESOURCES=1', '-DMONADO_LAUNCHER_ARTWORK=1',
            '-DMONADO_WINDOWS_BASELINE_LABEL=' + json.dumps(label, ensure_ascii=False)]
        includes = ['-I' + str(build / 'resources'), '-I' + str(prefix / 'include'),
            '-I' + str(reviewed_port / 'third_party/imgui'), '-I' + str(reviewed_port / 'third_party/imgui/backends')]
        objects = []
        for relative in SOURCES:
            obj = build / 'objects' / (relative.replace('/', '_') + '.o')
            run([compiler, 'c++', *FLAGS, *defines, *includes, '-c', reviewed_port / relative, '-o', obj])
            objects.append(_record(obj, build, source=relative))
        relink_args = ['c++', '-march=x86_64_v2', '-static',
            *(item['path'] for item in objects), 'objects/builder.res',
            *('static-libs/' + name for name in STATIC), *('-l' + name for name in SYSTEM_LIBRARIES),
            '-Wl,--subsystem,windows', '-municode', '-o', 'builder-base.exe']
        # Expand only known relative file arguments. Flags and system-library
        # names stay literal, and no shell interprets the baseline label/path.
        files = {item['path'] for item in objects} | {'objects/builder.res', 'builder-base.exe'} | {
            'static-libs/' + name for name in STATIC}
        run([compiler, *(str(build / value) if value in files else value for value in relink_args)])
        executable = build / 'builder-base.exe'
        recipe = dict(schema=1, compiler='Zig 0.14.1', working_directory='.', argv=relink_args,
            objects=objects, resource=_record(resource, build),
            static_libraries=[_record(build / 'static-libs' / name, build, dependency_path='lib/' + name) for name in STATIC],
            system_libraries=list(SYSTEM_LIBRARIES), output='builder-base.exe')
        target.write_new('relink.json', encoded(recipe))
        target.write_new('theme-resources.json', encoded(resources))
        archive_inputs = {prefix / 'lib' / name: inventory['lib/' + name] for name in STATIC}
        for path, identity in {**source_inputs, **sdl_inputs, **archive_inputs, compiler: compiler_identity,
                               prefix / 'content-inventory.json': inventory_identity}.items():
            if _identity(path) != identity:
                raise ValueError('Retained frontend input changed during compilation')
        result = dict(schema=1, kind='native-branded-static-windows-frontend',
            base_exe=str(executable), base_executable=_record(executable, build),
            compiler=dict(version=version, **compiler_identity), commands=commands,
            objects=objects, resource=recipe['resource'], static_libraries=recipe['static_libraries'],
            system_libraries=list(SYSTEM_LIBRARIES), imports=_imports(executable),
            resources=resources, relink_recipe=recipe, relink_recipe_path=str(build / 'relink.json'),
            source_inputs={path.relative_to(reviewed_port).as_posix(): identity for path, identity in source_inputs.items()},
            sdl_headers={path.relative_to(prefix).as_posix(): identity for path, identity in sdl_inputs.items()},
            dependency_inventory=inventory_identity,
            input_hashes_rechecked=True)
        target.write_new('frontend-receipt.json', encoded(result))
        return result
