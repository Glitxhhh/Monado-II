#!/usr/bin/env python3
"""[XCDE] LINUX-016: exact native PNG/window-icon component checks.

Run with an existing Python/Pillow installation for the independent pixel
oracle. Real pinned SDL uses only the dummy video driver and a hidden window;
the selected game/launcher executables are hashed, never executed. All input
folders, cwd decoys, state and captures are fresh synthetic children of runs/.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import resource
import signal
import subprocess
import sys
import time


ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / 'tests/native/test_window_icon_linux.c'


def digest(path):
    with Path(path).open('rb') as file:
        return hashlib.file_digest(file, 'sha256').hexdigest()


def identity(path):
    info = path.stat()
    return {'sha256': digest(path), 'bytes': info.st_size, 'mtime_ns': info.st_mtime_ns}


def selected(path):
    path = path.absolute()
    if path != path.resolve(strict=True) or not path.is_file() or not path.is_relative_to(ROOT / 'build'):
        raise ValueError('selected executables must be ordinary canonical Linux build files')
    receipt_path = path.parent / 'receipt.json'
    receipt = json.loads(receipt_path.read_text())
    saved = receipt.get('executable', {})
    with path.open('rb') as file:
        header = file.read(64)
    if (header[:6] != b'\x7fELF\x02\x01' or int.from_bytes(header[18:20], 'little') != 62 or
            receipt.get('status') != 'passed' or saved.get('path') != str(path) or
            saved.get('sha256') != digest(path)):
        raise ValueError('selected ELF must match a passed native build receipt: ' + str(path))
    icon = path.parent / 'icon.png'
    if not icon.is_file() or icon.is_symlink():
        raise ValueError('selected executable has no ordinary adjacent icon.png: ' + str(path))
    return path, {'executable': identity(path), 'receipt': identity(receipt_path), 'icon': identity(icon)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--icon', type=Path, required=True)
    parser.add_argument('--game-executable', type=Path, required=True)
    parser.add_argument('--launcher-executable', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    try:
        from PIL import Image
    except ImportError:
        parser.error('pixel oracle requires existing Pillow; use /usr/bin/python3 if its Pillow is available')
    output = args.output.absolute()
    if output != output.resolve() or not output.is_relative_to(ROOT / 'runs') or output.exists():
        parser.error('--output must be an absent ordinary child of Linux runs/')
    icon = args.icon.absolute()
    if icon != icon.resolve(strict=True) or not icon.is_file() or icon.is_symlink():
        parser.error('--icon must be an ordinary canonical PNG file')
    game, game_before = selected(args.game_executable)
    launcher, launcher_before = selected(args.launcher_executable)
    icon_before = identity(icon)
    for build in (game_before, launcher_before):
        if build['icon']['sha256'] != icon_before['sha256']:
            parser.error('selected adjacent icon is not the exact canonical PNG')
    with Image.open(icon) as image:
        rgba = image.convert('RGBA')
        width, height = rgba.size
        expected_bytes = rgba.tobytes()
    alpha = expected_bytes[3::4]
    oracle = {'width': width, 'height': height, 'rgba_bytes': len(expected_bytes),
              'rgba_sha256': hashlib.sha256(expected_bytes).hexdigest(),
              'alpha_zero': alpha.count(0), 'alpha_full': alpha.count(255),
              'alpha_partial': len(alpha) - alpha.count(0) - alpha.count(255),
              'alpha_min': min(alpha), 'alpha_max': max(alpha)}
    os.umask(0o077)
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    output.mkdir(parents=True)
    for directory in ('tmp', 'home', 'xdg-config', 'xdg-data', 'xdg-cache'):
        (output / directory).mkdir()
    compiler = ROOT / '.deps/bin/clang-cc'
    sysroot = Path((ROOT / '.deps/graphics/active-sysroot.txt').read_text().strip()).resolve()
    if not sysroot.is_relative_to(ROOT / '.deps/graphics/sysroots'):
        raise ValueError('selected SDK is not project-local')
    if json.loads((sysroot / '.monado-deps.json').read_text()).get('lock_sha256') != digest(ROOT / 'graphics-deps-linux.lock.json'):
        raise ValueError('selected SDK does not match the project graphics lock')
    library = sysroot / 'usr/lib/x86_64-linux-gnu'
    env = {'PATH': '/usr/bin:/bin', 'LANG': 'C.UTF-8', 'LC_ALL': 'C.UTF-8',
           'HOME': str(output / 'home'), 'TMPDIR': str(output / 'tmp'),
           'XDG_CONFIG_HOME': str(output / 'xdg-config'), 'XDG_DATA_HOME': str(output / 'xdg-data'),
           'XDG_CACHE_HOME': str(output / 'xdg-cache'), 'SDL_VIDEO_DRIVER': 'dummy',
           'SDL_AUDIO_DRIVER': 'dummy', 'LD_LIBRARY_PATH': str(library)}
    source_paths = [FIXTURE, Path(__file__).resolve(), ROOT / 'runtime/paths.c', ROOT / 'runtime/paths.h',
                    ROOT / 'runtime/platform.h', ROOT / 'runtime/platform/linux_compat.h',
                    ROOT / 'graphics-deps-linux.lock.json']
    sources = lambda: {str(path.relative_to(ROOT)): digest(path) for path in source_paths}
    receipt = {'scope': __doc__, 'status': 'running', 'started_utc': datetime.now(timezone.utc).isoformat(),
               'argv': sys.argv, 'cwd': str(ROOT), 'environment': env, 'sources_before': sources(),
               'canonical_icon_before': icon_before, 'selected_before': {'game': game_before, 'launcher': launcher_before},
               'independent_oracle': {**oracle, 'decoder': 'Pillow ' + Image.__version__,
                                      'python': sys.executable, 'python_version': sys.version,
                                      'pillow_image_module': identity(Path(Image.__file__))},
               'dependencies': {'compiler': identity(compiler), 'sysroot': str(sysroot),
                                'sdl': identity(library / 'libSDL3.so'),
                                'sysroot_marker': identity(sysroot / '.monado-deps.json')},
               'commands': [], 'cases': [], 'errors': []}

    def save():
        temporary = output / 'receipt.json.tmp'
        temporary.write_text(json.dumps(receipt, indent=2) + '\n')
        temporary.replace(output / 'receipt.json')

    def run(name, command, cwd=ROOT):
        command = list(map(str, command))
        start = time.monotonic()
        entry = {'name': name, 'argv': command, 'cwd': str(cwd)}
        receipt['commands'].append(entry)
        save()
        child = subprocess.Popen(command, cwd=cwd, env=env, stdin=subprocess.DEVNULL,
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE, start_new_session=True)
        entry['pid'] = child.pid
        failure = None
        try:
            stdout, stderr = child.communicate(timeout=30)
        except BaseException as error:
            failure = type(error).__name__ + ': ' + str(error)
            try:
                os.killpg(child.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            stdout, stderr = child.communicate(timeout=5)
        finally:
            if child.poll() is None:
                os.killpg(child.pid, signal.SIGKILL)
                child.communicate(timeout=5)
        for kind, data in (('stdout', stdout), ('stderr', stderr)):
            path = output / (name + '.' + kind)
            path.write_bytes(data)
            entry[kind] = str(path)
            entry[kind + '_sha256'] = digest(path)
        entry.update(exit=child.returncode, seconds=time.monotonic() - start, failure=failure)
        save()
        if child.returncode or failure:
            raise RuntimeError(name + ' failed; see preserved stderr')
        print(name, 'PASS', flush=True)

    save()
    try:
        flags = ['-O1', '-UNDEBUG', '-march=x86_64_v2', '-ffp-contract=off', '-fno-strict-aliasing',
                 '-ffunction-sections', '-fdata-sections', '-I' + str(sysroot / 'usr/include'), '-Iruntime']
        run('compiler-version', [compiler, '--version'])
        obj, binary = output / 'paths-icon-spy.o', output / 'window-icon-component'
        run('compile-paths', [compiler, *flags, '-DSDL_SetWindowIcon=icon_spy', '-c',
                              ROOT / 'runtime/paths.c', '-o', obj])
        run('link-fixture', [compiler, *flags, '-Wl,--gc-sections', FIXTURE, obj, library / 'libSDL3.so',
                             '-Wl,-rpath,' + str(library), '-pthread', '-lm', '-o', binary])
        receipt['artifacts'] = {'paths_object_sha256': digest(obj), 'component_sha256': digest(binary)}
        save()
        cases = [('selected-game', game, 'adjacent', 1), ('selected-launcher', launcher, 'adjacent', 1),
                 ('missing-resource', None, 'missing', 0), ('bad-png', None, 'bad', 0),
                 ('adjacent-priority', None, 'priority', 1), ('resources-fallback', None, 'fallback', 1)]
        for name, selected_exe, mode, expected_calls in cases:
            case = output / name
            case.mkdir()
            synthetic_input = case / 'input'
            (synthetic_input / 'exefs').mkdir(parents=True)
            (synthetic_input / 'romfs').mkdir()
            sentinel = synthetic_input / 'romfs/sentinel'
            sentinel.write_bytes(b'synthetic untouched input only\n')
            state = case / 'state'
            state.mkdir()
            # A different cwd icon must never replace executable resources.
            (state / 'icon.png').write_bytes(icon.read_bytes() if mode == 'missing' else b'cwd decoy, not a PNG')
            start = case / 'unrelated-start'
            start.mkdir()
            (start / 'icon.png').write_bytes(b'starting cwd decoy, not a PNG')
            if selected_exe is None:
                executable_directory = case / 'bundle/bin'
                executable_directory.mkdir(parents=True)
                selected_exe = executable_directory / 'not-executed'
                selected_exe.write_bytes(b'synthetic executable-path identity only\n')
                if mode in ('bad', 'priority'):
                    (executable_directory / 'icon.png').write_bytes(b'bad PNG' if mode == 'bad' else icon.read_bytes())
                if mode in ('bad', 'priority', 'fallback'):
                    resources = executable_directory.parent / 'Resources'
                    resources.mkdir()
                    (resources / 'icon.png').write_bytes(b'lower-priority decoy' if mode == 'priority' else icon.read_bytes())
            if mode == 'missing':
                expected_resource = '-'
            elif mode == 'fallback':
                expected_resource = str(selected_exe.parent / '../Resources/icon.png')
            else:
                expected_resource = str(selected_exe.parent / 'icon.png')
            pixels, metadata_path = state / 'pixels.rgba', state / 'metadata.json'
            sentinel_before = identity(sentinel)
            run(name, [binary, selected_exe, synthetic_input, state, pixels, metadata_path,
                       expected_resource, expected_calls], start)
            metadata = json.loads(metadata_path.read_text())
            if metadata['calls'] != expected_calls or identity(sentinel) != sentinel_before:
                raise AssertionError(name + ': API calls or synthetic input changed')
            if expected_calls:
                for key, value in oracle.items():
                    actual = digest(pixels) if key == 'rgba_sha256' else metadata[key]
                    if actual != value:
                        raise AssertionError(name + ': decoded ' + key + ' differs from canonical Pillow oracle')
            elif pixels.exists():
                raise AssertionError(name + ': unexpected pixels after missing/bad icon')
            receipt['cases'].append({'name': name, 'mode': mode, 'metadata': metadata,
                                     'metadata_sha256': digest(metadata_path),
                                     'rgba_sha256': digest(pixels) if pixels.exists() else None,
                                     'synthetic_input_unchanged': True})
            save()
        receipt['sources_after'] = sources()
        receipt['canonical_icon_after'] = identity(icon)
        receipt['selected_after'] = {'game': selected(game)[1], 'launcher': selected(launcher)[1]}
        receipt['dependencies_after'] = {'compiler': identity(compiler), 'sysroot': str(sysroot),
                                         'sdl': identity(library / 'libSDL3.so'),
                                         'sysroot_marker': identity(sysroot / '.monado-deps.json')}
        if (receipt['sources_after'] != receipt['sources_before'] or receipt['canonical_icon_after'] != icon_before or
                receipt['selected_after'] != receipt['selected_before'] or
                receipt['dependencies_after'] != receipt['dependencies']):
            raise AssertionError('source/icon/selected binary/receipt/resource identity changed during tests')
        receipt['status'] = 'passed'
    except BaseException as error:
        receipt['status'] = 'failed'
        receipt['errors'].append(type(error).__name__ + ': ' + str(error))
    finally:
        receipt['finished_utc'] = datetime.now(timezone.utc).isoformat()
        save()
    print(receipt['status'], output / 'receipt.json')
    return 0 if receipt['status'] == 'passed' else 1


if __name__ == '__main__':
    sys.exit(main())
