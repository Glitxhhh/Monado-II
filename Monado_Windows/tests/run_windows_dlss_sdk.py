#!/usr/bin/env python3
"""Explicit local approved SDK + actual production integration, authored GPU pixels.

No downloads, agreement acceptance, game input/profile or visible window. This
fixture must be explicitly invoked; ordinary source CI never executes vendor SDKs.
"""
from contextlib import ExitStack
from datetime import datetime, timezone
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import uuid

PORT = Path(__file__).resolve().parents[1]
ROOT = PORT.parent
MODULES = ('sl.interposer.dll', 'sl.common.dll', 'sl.dlss.dll', 'nvngx_dlss.dll', 'nvlowlatencyvk.dll')
LICENSES = ('LICENSE.txt', 'nvngx_dlss.license.txt', 'reflex.license.txt')


def sdk_diagnostics(text, *, mask_format, expect_r8_errors=False):
    """Judge real production SDK callbacks even when dispatch/exit say success."""
    lines = text.splitlines()
    errors = [line for line in lines if '[streamline][error]' in line.lower()]
    undefined = [line for line in errors if 'Cannot have undefined format' in line]
    cleanup = [line for line in lines if '[dlss] resource reset failed:' in line or '[dlss] shutdown failed:' in line]
    result = {'sdk_errors': len(errors), 'undefined_format_errors': len(undefined), 'cleanup_errors': len(cleanup)}
    if expect_r8_errors:
        if mask_format != 'r8' or len(undefined) != 12 or len(errors) != 12 or cleanup:
            raise ValueError('Expected exactly four unsupported R8-mask SDK diagnostics per frame over three frames: '+str(result))
    elif errors or cleanup:
        raise ValueError('SDK callback/cleanup errors invalidate the pixel-only result: '+str(result))
    return result


def identity(data):
    return {'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}


def main():
    # Receipts/logs retain UTF-8 independently of the launching console's code
    # page, including diagnostics containing the explicitly selected long path.
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, 'reconfigure'):
            stream.reconfigure(encoding='utf-8', errors='replace')
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk-directory', type=Path, help='Explicit approved staged provider ROOT containing windows-providers.json')
    parser.add_argument('--output', type=Path, help='Fresh private fixture output directory')
    parser.add_argument('--prefix', type=Path, help='Checked native Vulkan/SDL dependencies prefix')
    parser.add_argument('--compile-only', action='store_true', help='Compile authored fixture; do not read/map vendor files or execute fixture')
    parser.add_argument('--mask-format', choices=('r16','r8'), default='r16', help='Real fifth resource tag; R8 is an explicit diagnostic negative control')
    parser.add_argument('--expect-r8-errors', action='store_true', help='Require the pinned SDK unsupported-R8 diagnostic; never a positive qualification')
    args = parser.parse_args()
    if args.expect_r8_errors and args.mask_format != 'r8':
        parser.error('--expect-r8-errors requires --mask-format r8')
    if os.name != 'nt':
        raise RuntimeError('Native Windows required')
    if not args.compile_only and not args.sdk_directory:
        parser.error('--sdk-directory is required for explicit vendor SDK execution')
    output = (args.output or PORT / 'runs' / ('dlss-sdk-' + datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ') + '-' + uuid.uuid4().hex[:8])).absolute()
    if output.exists():
        raise ValueError('Fixture output must be fresh; existing files are preserved')
    if args.sdk_directory:
        source = args.sdk_directory.absolute()
        if output == source or output.is_relative_to(source) or source.is_relative_to(output):
            raise ValueError('Fixture output and approved SDK input must be disjoint')
    output.mkdir(parents=True)
    prefix = args.prefix or Path(json.loads((PORT / '.deps/windows-deps.json').read_text())['prefix'])
    binary = output / 'build' / 'authored-dlss.exe'
    binary.parent.mkdir()
    # CreateProcessW still imposes MAX_PATH on lpCurrentDirectory even when
    # the executable and all authored inputs use valid extended Win32 paths.
    # Keep incidental vendor/compiler logs private and off the long input/output
    # path, without changing any guarded identity used to stage or verify it.
    launch_state = PORT / 'runs' / ('dlss-sdk-state-' + uuid.uuid4().hex[:8])
    launch_state.mkdir()
    if len(str(launch_state)) >= 248:
        raise ValueError('The private fixture launch directory must fit the Win32 current-directory limit')
    sources = [PORT/'runtime/nvn/vulkan/dlss.cpp', PORT/'runtime/nvn/vulkan/dlss.h',
               PORT/'runtime/nvn/vulkan/vk.h', PORT/'runtime/nvn/vulkan/temporal.h',
               PORT/'runtime/nvn/vulkan/temporal_camera.h', PORT/'runtime/settings.h',
               PORT/'runtime/platform/windows_optional_library.h', PORT/'runtime/platform/windows_module_path.h', PORT/'runtime/platform/windows_save_io.h',
               PORT/'tools/windows_utf8.rc', PORT/'tools/windows_utf8.manifest',
               PORT/'tests/native/test_dlss_sdk_windows.cpp', Path(__file__), ROOT/'tools/windows_file_guard.py']
    hashes = lambda: {str(path.relative_to(ROOT)): hashlib.sha256(path.read_bytes()).hexdigest() for path in sources}
    receipt = {'status': 'failed', 'sources_before': hashes(), 'commands': [], 'working_directory': str(launch_state),
               'mask_format':args.mask_format,'expected_r8_negative':args.expect_r8_errors,
               'scope': 'actual production DLSS, explicit approved local SDK and authored hidden SDL/Vulkan GPU frames; no game, profile, FPS or full renderer qualification'}
    env = os.environ.copy()
    for key in list(env):
        if key.startswith('PYTHON'):
            del env[key]
    env.update(PYTHONNOUSERSITE='1', PYTHONDONTWRITEBYTECODE='1', PYTHONUNBUFFERED='1',
               ZIG_GLOBAL_CACHE_DIR=str(PORT/'.deps/zig-global-cache'), ZIG_LOCAL_CACHE_DIR=str(output/'zig-cache'))
    env['PATH'] = str(prefix/'bin') + os.pathsep + env.get('PATH', '')
    def run(label, command, timeout):
        argv = [str(item) for item in command]
        if len(argv[0]) >= 248 and not argv[0].startswith('\\\\?\\'):
            argv[0] = '\\\\?\\UNC\\' + argv[0][2:] if argv[0].startswith('\\\\') else '\\\\?\\' + argv[0]
        # Supply lpApplicationName explicitly: otherwise CreateProcessW searches
        # argv[0] through its legacy MAX_PATH parser even for extended spelling.
        result = subprocess.run(argv, executable=argv[0], cwd=launch_state, env=env, capture_output=True, text=True,
                                encoding='utf-8', errors='replace', timeout=timeout)
        receipt['commands'].append({'argv': argv, 'returncode': result.returncode})
        (output/(label+'.log')).write_text(result.stdout+result.stderr, encoding='utf-8')
        print(result.stdout, end='', flush=True)
        if result.returncode:
            raise RuntimeError(label+' failed: '+result.stderr[-12000:])
        return result
    try:
        flags = ['-std=gnu++20', '-O1', '-UNDEBUG', '-march=x86_64_v2', '-ffp-contract=off', '-fno-fast-math',
                 '-fno-strict-aliasing', '-Wall', '-Wextra', '-Werror', '-Wno-missing-field-initializers',
                 '-Wno-unused-parameter', '-Wno-unused-function', '-Wno-missing-braces', '-Wno-nullability-completeness']
        receipt['flags'] = flags
        resource = output/'windows_utf8.res'
        run('compile-native-manifest', [sys.executable, '-B', '-m', 'ziglang', 'rc', '/I', PORT/'tools',
            '/fo', resource, PORT/'tools/windows_utf8.rc'], 60)
        run('compile', [sys.executable, '-B', '-m', 'ziglang', 'c++', *flags,
            '-I'+str(PORT/'runtime'), '-I'+str(prefix/'include'), '-I'+str(PORT/'third_party/streamline/include'),
            PORT/'tests/native/test_dlss_sdk_windows.cpp', PORT/'runtime/nvn/vulkan/dlss.cpp',
            resource, prefix/'lib/libSDL3.dll.a', prefix/'lib/libvulkan-1.dll.a', '-ladvapi32', '-luser32', '-o', binary], 180)
        receipt['executable'] = identity(binary.read_bytes())
        if args.compile_only:
            receipt['status'] = 'compiled-not-run'
        else:
            sys.path.insert(0, str(ROOT/'tools'))
            import windows_file_guard as guard
            # No SDK execution occurs until exact receipted input members have
            # been copied into the fresh fixture, with source leases retained.
            with ExitStack() as leases:
                source_guard = leases.enter_context(guard.locked_directory(source, owner=True))
                raw = source_guard.read_regular('windows-providers.json', max_bytes=4*1024*1024, owner=True)
                staged = json.loads(raw)
                if staged.get('schema') != 1 or staged.get('platform') != 'windows-amd64':
                    raise ValueError('Expected checked staged Windows provider receipt')
                selected = staged.get('selection', {}).get('streamline', {})
                if set(selected.get('stage', {})) != set(MODULES):
                    raise ValueError('Actual SDK oracle requires exactly five approved production NVIDIA modules')
                selected_licenses = {Path(name).name for name in selected.get('licenses', [])}
                if selected_licenses != set(LICENSES):
                    raise ValueError('Actual SDK oracle requires all three approved license files')
                receipt['provider_receipt'] = identity(raw)
                target = leases.enter_context(guard.locked_directory(output, owner=True))
                expected = [('build/'+name, 'build/'+name) for name in MODULES]
                expected += [('providers/streamline/'+name, 'providers/streamline/'+name) for name in LICENSES]
                for relative, destination in expected:
                    stream = leases.enter_context(source_guard.open_regular(relative, max_bytes=512*1024*1024, owner=True))
                    data = stream.read()
                    if identity(data) != staged['files'].get(relative):
                        raise ValueError('Approved staged SDK member changed: '+relative)
                    directory, leaf = destination.rsplit('/', 1)
                    with target.child(directory, create=True, owner=True) as child:
                        child.write_new(leaf, data, owner=True)
                checked_exe = leases.enter_context(target.open_regular('build/authored-dlss.exe', max_bytes=512*1024*1024, owner=True))
                if identity(checked_exe.read()) != receipt['executable']:
                    raise ValueError('Authored fixture image changed before execution')
                result = run('native', [binary, '--mask-'+args.mask_format], 120)
                if 'PASS actual production DLSS SDK:' not in result.stdout:
                    raise ValueError('Missing actual SDK evaluation/readback completion')
                receipt['sdk_diagnostics'] = sdk_diagnostics(result.stderr,mask_format=args.mask_format,expect_r8_errors=args.expect_r8_errors)
                receipt['status'] = 'expected-sdk-error-reproduced' if args.expect_r8_errors else 'passed'
        receipt['sources_after'] = hashes()
        if receipt['sources_before'] != receipt['sources_after']:
            receipt['status'] = 'failed'
            raise RuntimeError('Production/fixture source closure changed during qualification')
    finally:
        (output/'receipt.json').write_text(json.dumps(receipt, indent=2)+'\n', encoding='utf-8')
        print('Native DLSS SDK evidence: '+str(output), flush=True)


if __name__ == '__main__':
    main()
