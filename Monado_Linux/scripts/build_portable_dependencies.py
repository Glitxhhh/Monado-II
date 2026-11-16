#!/usr/bin/env python3
"""Source-built SDL3 and complete native FFmpeg codec stack targeting GNU 2.39.

All built-in FFmpeg decoders/demuxers/encoders remain enabled. Optional external
encoder/network integrations are not auto-detected; the game opens local media
and retains native VP9/VP8/H264/Vorbis/AAC/Opus decoding and swscale/resampling.
No host installation, game inputs, shader caches or vendor binaries are involved.
"""
import argparse
import contextlib
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import time
import shutil
import shlex
import tempfile
import signal

import setup_portable_baseline as baseline
import setup_linux_build_tools as build_tools

ROOT = baseline.ROOT


# [XCDE] LINUX-111: pkg-config may be a host shell wrapper which resolves
# pkgconf through PATH. A target sysroot's executables must never shadow that
# host helper: their loader/library set belongs to a different distribution.
HOST_TOOL_PATH = '/usr/bin:/bin'


def build_environment(sysroot):
    """Use native build tools and only the pinned target pkg-config database."""
    sysroot = Path(sysroot)
    return {'PATH': HOST_TOOL_PATH, 'LANG': 'C.UTF-8', 'LC_ALL': 'C.UTF-8',
            'PKG_CONFIG_SYSROOT_DIR': str(sysroot),
            'PKG_CONFIG_LIBDIR': str(sysroot / 'usr/lib/x86_64-linux-gnu/pkgconfig')
                + ':' + str(sysroot / 'usr/share/pkgconfig')}


def target_cmake_options():
    """Keep target header/library discovery separate from native programs."""
    return ['-DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER',
            '-DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY',
            '-DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY',
            '-DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY',
            # The pinned baseline intentionally has no liburing development
            # provider. SDL's ordinary threaded async-I/O fallback remains.
            '-DSDL_LIBURING=OFF']


def log_excerpt(path, limit=16384):
    """Bounded beginning/end context, including configure's early loader errors."""
    try:
        with Path(path).open('rb') as stream:
            beginning = stream.read(limit // 2)
            stream.seek(0, os.SEEK_END)
            size = stream.tell()
            if size <= limit // 2:
                return beginning.decode('utf-8', errors='replace').strip()
            stream.seek(max(limit // 2, size - limit // 2))
            ending = stream.read(limit // 2)
    except OSError as error:
        return 'Log excerpt unavailable: ' + str(error)
    separator = '\n... intermediate log output omitted ...\n' if size > limit else '\n'
    return (beginning.decode('utf-8', errors='replace').rstrip() + separator
            + ending.decode('utf-8', errors='replace').lstrip()).strip()


def phase_failure(name, path, code=None, *, display_path=None):
    label = name + ' failed' + ((' (exit ' + str(code) + ')') if code is not None else '')
    return RuntimeError(label + '; log: ' + str(display_path or path) + '\n' + log_excerpt(path))


def wait_for_build(process, name, *, timeout=1200, interval=15):
    """Keep outer setup logs alive without changing the original phase deadline."""
    started = time.monotonic()
    deadline = started + timeout
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise subprocess.TimeoutExpired(process.args, timeout)
        try:
            return process.wait(timeout=min(interval, remaining))
        except subprocess.TimeoutExpired:
            elapsed = time.monotonic() - started
            if elapsed >= timeout:
                raise subprocess.TimeoutExpired(process.args, timeout) from None
            print(name + ': still working (' + str(int(elapsed)) + 's)', flush=True)


def verify_existing():
    marker = baseline.ordinary(ROOT / '.deps/portable/active-build.json')
    selected = json.loads(marker.read_text())
    path = baseline.ordinary(selected['receipt'])
    if not path.is_relative_to(ROOT / '.deps/portable/build') or baseline.sha(path) != selected['sha256']:
        raise ValueError('portable source-build receipt identity differs')
    receipt = json.loads(path.read_text())
    if (receipt.get('status') != 'passed' or receipt.get('lock_sha256') != baseline.sha(baseline.LOCK)
            or receipt.get('recipe_sha256') != baseline.sha(__file__)):
        raise ValueError('portable source-build recipe/lock changed')
    for row in receipt.get('outputs', {}).values():
        p = baseline.ordinary(row['path'])
        if not p.is_relative_to(path.parent / 'prefix/lib') or p.stat().st_size != row['bytes'] or baseline.sha(p) != row['sha256']:
            raise ValueError('portable dependency library changed')
        elf_floor(p)
    return path, receipt


def elf_floor(path):
    import re
    result = subprocess.run(['/usr/bin/readelf', '-VW', path], capture_output=True, text=True, check=True)
    versions = {tuple(map(int, value.split('.'))) for value in re.findall(r'Name: GLIBC_([0-9.]+)', result.stdout)}
    floor = max(versions, default=(0,))
    if floor > (2, 39):
        raise ValueError('portable library exceeds GLIBC_2.39: ' + str(path) + ' ' + str(floor))
    return '.'.join(map(str, floor))


def terminate_owned_group(process, grace=5):
    """Finish this Popen-created session, including a lingering grandchild."""
    try: os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError: pass
    try: process.wait(timeout=grace)
    except subprocess.TimeoutExpired: pass
    finally:
        # A parent can exit on TERM while a compiler descendant ignores it.
        try: os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError: pass
        process.wait()


# A complete FFmpeg codec shared-library link opens well over the desktop's
# common 1024-descriptor soft limit. Reserve headroom for its object inputs and
# linker workers without changing the hard cap or any system configuration.
PORTABLE_MIN_NOFILE = 4096
PORTABLE_TARGET_NOFILE = 65536


@contextlib.contextmanager
def portable_file_limit(receipt):
    """Give only this synchronous build worker's children descriptor headroom."""
    import resource
    soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
    limits = receipt['file_descriptor_limits'] = {
        'initial': {'soft': soft, 'hard': hard},
        'effective': {'soft': soft, 'hard': hard},
        'minimum_soft': PORTABLE_MIN_NOFILE, 'target_soft': PORTABLE_TARGET_NOFILE,
        'scope': 'temporary soft limit in portable build worker; hard limit unchanged'}
    changed = False
    pending = None
    try:
        if hard != resource.RLIM_INFINITY and hard < PORTABLE_MIN_NOFILE:
            raise RuntimeError(
                'Portable FFmpeg build needs an open-file soft limit of at least '
                + str(PORTABLE_MIN_NOFILE) + '; inherited soft=' + str(soft)
                + ', hard=' + str(hard) + '. The hard limit is too low for the full codec link. '
                'Run the build in an environment with a higher hard open-file limit. '
                'Monado II does not change system limits or request sudo.')
        desired = soft
        if soft != resource.RLIM_INFINITY and soft < PORTABLE_TARGET_NOFILE:
            desired = (PORTABLE_TARGET_NOFILE if hard == resource.RLIM_INFINITY
                       else min(PORTABLE_TARGET_NOFILE, hard))
        if desired != soft:
            # Mark the restore obligation before the syscall: cancellation may
            # arrive immediately after the kernel has applied the new limit.
            changed = True
            resource.setrlimit(resource.RLIMIT_NOFILE, (desired, hard))
        effective = resource.getrlimit(resource.RLIMIT_NOFILE)
        limits['effective'] = {'soft': effective[0], 'hard': effective[1]}
        if effective != (desired, hard):
            raise RuntimeError('Portable build open-file limit differs from the requested soft-only adjustment')
        yield limits
    except BaseException as error:
        pending = error
        raise
    finally:
        try:
            # The caller exits this context only after its owned compiler
            # process groups have finished, including cancellation cleanup.
            if changed:
                resource.setrlimit(resource.RLIMIT_NOFILE, (soft, hard))
            restored = resource.getrlimit(resource.RLIMIT_NOFILE)
            limits['restored'] = {'soft': restored[0], 'hard': restored[1]}
            if restored != (soft, hard):
                raise RuntimeError('Portable build open-file limit was not restored')
        except BaseException as restore_error:
            limits['restoration_error'] = str(restore_error)
            if pending is None:
                raise
            # Retain the actual configure/compiler/linker exception even when
            # reporting a separate cleanup problem.
            pending.add_note('Open-file limit restoration also failed: ' + str(restore_error))


def build(output, jobs=4):
    prepared = baseline.prepare(offline=True)
    tools = build_tools.setup()
    out = baseline.ordinary(Path(output), directory=True)
    if out.exists() or not out.is_relative_to(ROOT / '.deps/portable/build'):
        raise ValueError('fresh .deps/portable/build output required')
    out.mkdir(parents=True)
    logs = out / 'logs'; logs.mkdir()
    prefix = out / 'prefix'
    scratch_context = tempfile.TemporaryDirectory(prefix='monado-portable-', dir='/tmp')
    work = Path(scratch_context.name)
    if work.resolve() != work or any(c.isspace() for c in str(work)):
        scratch_context.cleanup(); raise ValueError('ordinary space-free portable scratch required')
    work.chmod(0o700)
    sysroot = work / 'sysroot'; sources = work / 'sources'
    # FFmpeg explicitly rejects whitespace in its build/source paths. Keep
    # every configure-visible path private and space-free, regardless of the
    # player's chosen installation/output path. Project caches stay immutable.
    shutil.copytree(prepared['sysroot'], sysroot, symlinks=True)
    shutil.copytree(prepared['sources'], sources, symlinks=True)
    tools_dir = work / 'tools'; tools_dir.mkdir()
    cmake_data = Path(tools['cmake']['path']).parents[1]
    shutil.copytree(cmake_data, tools_dir / 'cmake', symlinks=True)
    shutil.copyfile(tools['ninja']['path'], tools_dir / 'ninja'); (tools_dir / 'ninja').chmod(0o755)
    c, cxx = tools_dir / 'cc', tools_dir / 'cxx'
    for target, compiler in ((c, ROOT / '.deps/bin/clang-cc'), (cxx, ROOT / '.deps/bin/clang-cxx')):
        target.write_text('#!/bin/sh\nexec ' + shlex.quote(str(compiler)) + ' "$@"\n'); target.chmod(0o755)
    prefix = work / 'prefix'
    scratch_logs = work / 'logs'; scratch_logs.mkdir()
    env = build_environment(sysroot)
    flags = '-target x86_64-linux-gnu.2.39 -march=x86_64_v2 -fPIC -ffp-contract=off -fno-strict-aliasing'
    receipt = {'schema': 1, 'status': 'running', 'target_glibc': '2.39', 'lock_sha256': prepared['lock_sha256'],
               'recipe_sha256': baseline.sha(__file__), 'source_root': prepared['sources'], 'source_manifest_sha256': prepared['source_manifest_sha256'],
               'scratch': str(work), 'scratch_policy': 'private space-free disposable source/build/sysroot/prefix; relocated outputs only',
               'compiler_wrappers': {str(x.relative_to(ROOT)): baseline.sha(x) for x in (ROOT / '.deps/bin/clang-cc', ROOT / '.deps/bin/clang-cxx')},
               'build_environment': env, 'commands': [], 'outputs': {}, 'features': {
                   'ffmpeg_builtin_codecs': 'all', 'ffmpeg_builtin_demuxers': 'all', 'ffmpeg_external_autodetect': False,
                   'sdl_video_audio_input': 'Noble platform development interfaces; actual CMake feature report retained'}}
    def save():
        (out / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
    def run(name, args, cwd=work):
        started = time.monotonic()
        process = None
        print(name + ': starting; log: ' + str(logs / (name + '.log')), flush=True)
        try:
            with (scratch_logs / (name + '.log')).open('wb') as stream:
                process = subprocess.Popen(list(map(str, args)), cwd=cwd, env=env, stdin=subprocess.DEVNULL,
                    stdout=stream, stderr=subprocess.STDOUT, start_new_session=True)
                try:
                    wait_for_build(process, name)
                except BaseException:
                    terminate_owned_group(process)
                    raise
        finally:
            shutil.copyfile(scratch_logs / (name + '.log'), logs / (name + '.log'))
            receipt['commands'].append({'name': name, 'argv': list(map(str, args)), 'cwd': str(cwd),
                'exit': process.returncode if process is not None else None, 'seconds': time.monotonic() - started})
            save()
        if process.returncode:
            raise phase_failure(name, logs / (name + '.log'), process.returncode)
        print(name + ': completed (' + format(time.monotonic() - started, '.1f') + 's)', flush=True)
    try:
        save()
        with portable_file_limit(receipt):
            cmake = str(tools_dir / 'cmake/bin/cmake'); ninja = str(tools_dir / 'ninja')
            tool_dir = tools_dir
            scanner = tool_dir / 'wayland-scanner'
            # The old scanner needs libxml2.so.2, absent on some newer hosts.
            # Invoke its own pinned loader/libraries together; never put an old
            # libc on the current process's inherited LD_LIBRARY_PATH.
            scanner.write_text('#!/bin/sh\nexec ' + ' '.join(shlex.quote(str(x)) for x in (
                sysroot / 'usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2', '--library-path',
                sysroot / 'usr/lib/x86_64-linux-gnu', sysroot / 'usr/bin/wayland-scanner')) + ' "$@"\n')
            scanner.chmod(0o755)
            run('sdl-configure', [cmake, '-S', sources / 'SDL3-3.4.8', '-B', work / 'sdl', '-G', 'Ninja',
                '-DCMAKE_MAKE_PROGRAM=' + ninja, '-DCMAKE_C_COMPILER=' + str(c), '-DCMAKE_CXX_COMPILER=' + str(cxx),
                '-DCMAKE_SYSROOT=' + str(sysroot), '-DCMAKE_C_FLAGS=' + flags, '-DCMAKE_CXX_FLAGS=' + flags,
                *target_cmake_options(),
                '-DCMAKE_LIBRARY_ARCHITECTURE=x86_64-linux-gnu', '-DCMAKE_LIBRARY_PATH=' + str(sysroot / 'usr/lib/x86_64-linux-gnu'),
                '-DCMAKE_PREFIX_PATH=' + str(sysroot / 'usr'), '-DPKG_CONFIG_EXECUTABLE=/usr/bin/pkg-config',
                '-DWAYLAND_SCANNER=' + str(scanner),
                '-DCMAKE_INSTALL_PREFIX=' + str(prefix), '-DCMAKE_INSTALL_LIBDIR=lib', '-DCMAKE_BUILD_TYPE=Release',
                '-DSDL_SHARED=ON', '-DSDL_STATIC=OFF', '-DSDL_TEST_LIBRARY=OFF', '-DSDL_TESTS=OFF', '-DSDL_EXAMPLES=OFF', '-DSDL_UNIX_CONSOLE_BUILD=ON'])
            # Configuration success alone can silently yield a dummy-only SDL.
            # Require both desktop backends and real audio before accepting it.
            report = (logs / 'sdl-configure.log').read_text()
            for driver in ('X11', 'Wayland', 'ALSA', 'PULSEAUDIO', 'PIPEWIRE'):
                if not __import__('re').search(r'SDL_' + driver + r'\s+\([^\n]*\):\s+ON', report, __import__('re').IGNORECASE):
                    raise ValueError('portable SDL lost required platform feature: ' + driver
                        + '; configure log: ' + str(logs / 'sdl-configure.log')
                        + '\n' + log_excerpt(logs / 'sdl-configure.log'))
            run('sdl-build', [cmake, '--build', work / 'sdl', '--parallel', jobs])
            run('sdl-install', [cmake, '--install', work / 'sdl'])
            ff = work / 'ffmpeg'; ff.mkdir()
            ff_source = work / 'ffmpeg-source'
            shutil.copytree(sources / 'ffmpeg-8.0.1', ff_source, symlinks=True)
            configure = ff_source / 'configure'
            original = configure.read_text()
            if original.count('check_func  sysctl\n') != 1:
                raise ValueError('FFmpeg sysctl probe source changed')
            # Zig's glibc stubs retain the removed sysctl symbol, but modern glibc
            # has no header. Test both declaration and link, as FFmpeg already does
            # for sysctlbyname; do not invent a sysctl implementation.
            configure.write_text(original.replace('check_func  sysctl\n', 'check_func_headers sys/sysctl.h sysctl\n'))
            receipt['source_patches'] = {'ffmpeg/configure': {'before_sha256': hashlib.sha256(original.encode()).hexdigest(), 'after_sha256': baseline.sha(configure)}}
            run('ffmpeg-configure', [configure, '--prefix=' + str(prefix),
                '--cc=' + str(c), '--cxx=' + str(cxx), '--extra-cflags=' + flags, '--extra-ldflags=-target x86_64-linux-gnu.2.39',
                '--enable-shared', '--disable-static', '--disable-programs', '--disable-doc', '--disable-autodetect',
                '--disable-avdevice', '--disable-avfilter', '--enable-pic', '--pkg-config=/usr/bin/pkg-config', '--x86asmexe=' + str(sysroot / 'usr/bin/nasm')], cwd=ff)
            make = sysroot / 'usr/bin/make'
            run('ffmpeg-build', [make, '-j' + str(jobs)], cwd=ff)
            run('ffmpeg-install', [make, 'install'], cwd=ff)
            final_prefix = out / 'prefix'
            shutil.copytree(prefix, final_prefix, symlinks=True)
            for metadata in final_prefix.rglob('*'):
                if metadata.is_file() and not metadata.is_symlink() and metadata.suffix in ('.pc', '.cmake', '.la'):
                    metadata.write_text(metadata.read_text().replace(str(prefix), str(final_prefix)))
            prefix = final_prefix
            # Revalidate the immutable archive-derived source cache after copying/
            # compiling it; no modified cache can become an accepted provider.
            current = baseline.prepare(offline=True)
            if current['source_manifest_sha256'] != prepared['source_manifest_sha256']:
                raise ValueError('portable source cache changed during build')
            for library in sorted((prefix / 'lib').glob('*.so*')):
                if library.is_symlink() or not library.is_file():
                    continue
                receipt['outputs'][library.name] = {'path': str(library), 'bytes': library.stat().st_size,
                    'sha256': baseline.sha(library), 'glibc_max': elf_floor(library)}
            if not any(n.startswith('libSDL3.so.') for n in receipt['outputs']) or not any(n.startswith('libavcodec.so.') for n in receipt['outputs']):
                raise ValueError('portable source-build libraries are incomplete')
            if baseline.sha(__file__) != receipt['recipe_sha256']:
                raise ValueError('portable dependency recipe changed during build')
        receipt['status'] = 'passed'
        marker = ROOT / '.deps/portable/active-build.json'
        temp = marker.with_suffix('.partial'); temp.write_text(json.dumps({'receipt': str(out / 'receipt.json'), 'sha256': baseline.sha(out / 'receipt.json')}) + '\n')
        # Save the final status before binding its hash in the active marker.
        save(); temp.write_text(json.dumps({'receipt': str(out / 'receipt.json'), 'sha256': baseline.sha(out / 'receipt.json')}) + '\n'); temp.replace(marker)
    except BaseException as error:
        receipt.update(status='failed', error=str(error)); raise
    finally:
        save()
        scratch_context.cleanup()
    return receipt


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--jobs', type=int, choices=range(1, 9), default=4)
    args = ap.parse_args()
    def cancelled(_signum, _frame): raise InterruptedError('portable dependency build cancelled')
    signal.signal(signal.SIGTERM, cancelled)
    print(json.dumps(build(args.output, args.jobs), indent=2))


if __name__ == '__main__':
    main()
