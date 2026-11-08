#!/usr/bin/env python3
"""Build Monado II locally and produce the workspace's launcher AppImage.

One explicit invocation verifies the player's existing extracted input, downloads
locked build dependencies, generates/compiles locally and packages only the
launcher. No game or graphical window is opened. Every run uses fresh outputs;
previous builds, saves and a known-good launcher AppImage are preserved.
"""
import argparse
import codecs
import contextlib
import ctypes
from datetime import datetime, timezone
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import platform
import selectors
import shutil
import signal
import stat
import subprocess
import sys
import time
import uuid

ROOT = Path(__file__).resolve().parents[1]
IMAGE = 'Monado-II-x86_64.AppImage'
REQUIRED_TOOLS = ('bash', 'pkg-config', 'dpkg-deb', 'readelf', 'mksquashfs', 'unsquashfs')
DLSS_REQUIRED_TOOLS = ('nm',)
BWRAP = Path('/usr/bin/bwrap')
GNU_CXX_RUNTIME = Path('/usr/lib/x86_64-linux-gnu/libstdc++.so.6')
_run_spec = importlib.util.spec_from_file_location('monado_builder_run_linux', ROOT / 'Monado_Linux/scripts/run_linux.py')
run_linux = importlib.util.module_from_spec(_run_spec)
_run_spec.loader.exec_module(run_linux)
install_layout = run_linux.install_layout


class BuildError(RuntimeError):
    pass


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def ordinary(path, *, directory=False, missing=False):
    path = Path(path).absolute()
    if path.resolve() != path or '..' in path.parts:
        raise BuildError('output/source path must be canonical without symlinks: ' + str(path))
    current = Path(path.anchor)
    for part in path.parts[1:]:
        current /= part
        try:
            info = current.lstat()
        except FileNotFoundError:
            if missing:
                continue
            raise BuildError('missing path: ' + str(current)) from None
        if stat.S_ISLNK(info.st_mode):
            raise BuildError('managed path must not be a symlink: ' + str(current))
        if current != path and not stat.S_ISDIR(info.st_mode):
            raise BuildError('path parent is not a directory: ' + str(current))
        if current == path and ((directory and not stat.S_ISDIR(info.st_mode)) or
                (not directory and (not stat.S_ISREG(info.st_mode) or info.st_nlink != 1))):
            raise BuildError('expected ordinary ' + ('directory: ' if directory else 'single-link file: ') + str(path))
    return path


def json_file(path):
    ordinary(path)
    if path.stat().st_size > 2 * 1024 * 1024:
        raise BuildError('oversized build receipt: ' + str(path))
    try:
        return json.loads(path.read_text())
    except (UnicodeError, ValueError) as error:
        raise BuildError('invalid build receipt: ' + str(path)) from error


def image_pair(path):
    """Only replace a known, intact local image/receipt pair, never unknown files."""
    receipt = Path(str(path) + '.receipt.json')
    ordinary(path, missing=True)
    ordinary(receipt, missing=True)
    if not path.exists() and not receipt.exists():
        return None
    if not path.is_file() or not receipt.is_file():
        raise BuildError('incomplete existing AppImage/receipt pair; preserve it and inspect: ' + str(path))
    with path.open('rb') as stream:
        header = stream.read(12)
    if header[:4] != b'\x7fELF' or header[8:11] != b'AI\x02':
        raise BuildError('expected a verified type-2 AppImage: ' + str(path))
    document = json_file(receipt)
    if document.get('status') != 'passed' or document.get('sha256') != digest(path):
        raise BuildError('existing AppImage does not match its successful receipt; refusing to replace it')
    return document


def validate_reused_game(port, binary, state, *, dlss):
    """Retain an original successful build; never relocate or rewrite its receipt."""
    binary = ordinary(binary)
    install_layout.build_port(port, binary)
    receipt = json_file(binary.parent / 'receipt.json')
    if (receipt.get('status') != 'passed' or receipt.get('artifact_kind') == 'linux-launcher' or
            not isinstance(receipt.get('generated'), dict) or not receipt['generated'] or receipt.get('errors')):
        raise BuildError('--reuse-game requires a successful complete generated-game receipt')
    if type(receipt.get('dlss_enabled')) is not bool or receipt['dlss_enabled'] != dlss:
        raise BuildError('reused game DLSS flag differs from the requested build; preserve the existing feature selection')
    before = receipt.get('sources_before')
    if not isinstance(before, dict) or not before or before != receipt.get('sources_after'):
        raise BuildError('reused game has no stable native source snapshot')
    # The release classifier separately checks generator/profile/launch protocol
    # changes. This builder also refuses changed or newly added native inputs.
    required = {'scripts/build_linux.py', 'graphics-deps-linux.lock.json', 'requirements-linux.lock',
                'tools/gen_smaa_inc.py', 'dlss-linux.lock.json', 'scripts/setup_linux_dlss.py',
                'fsr-linux.lock.json', 'scripts/setup_linux_fsr.py'}
    native = {p.relative_to(port).as_posix() for name in ('runtime', 'third_party')
              for p in (port / name).rglob('*') if p.is_file() and
              p.suffix in ('.c', '.cpp', '.h', '.hpp', '.inc', '.hlsl')}
    icon = port / 'local_assets/monado-icon.png'
    if icon.exists() or icon.is_symlink():
        required.add('local_assets/monado-icon.png')
    if set(before) != required | native:
        raise BuildError('reused game native source closure differs; a full local game build is required')
    for name, expected in before.items():
        source = ordinary(port / name)
        if not source.is_relative_to(port) or digest(source) != expected:
            raise BuildError('reused game native source differs: ' + name + '; a full local game build is required')
    launch = run_linux.plan(binary, state, port=port)
    selected = Path(launch['input'])
    expected_input = receipt.get('input', {})
    exefs = {p.name: digest(p) for p in sorted((selected / 'exefs').iterdir()) if p.is_file()}
    if expected_input.get('path') != str(selected) or expected_input.get('exefs') != exefs:
        raise BuildError('reused game input differs from its build receipt')
    return {'path': str(binary), 'sha256': launch['binary_sha256'],
            'receipt_sha256': digest(binary.parent / 'receipt.json'), 'build_port': launch['build_port']}


def layout(root, jobs, *, require_input=True, identity=None, dlss=True, reuse_game=None):
    root = ordinary(root, directory=True)
    port = ordinary(root / 'Monado_Linux', directory=True)
    install_layout.binding(port)
    for name in ('build', 'generated', 'runs', '.deps'):
        ordinary(port / name, directory=True, missing=True)
    for name in ('runs/local-build', '.deps/metadata'):
        ordinary(port / name, directory=True, missing=True)
    image_pair(root / IMAGE)
    selected = root / 'game_files'
    try:
        source = selected.resolve(strict=True)
    except (FileNotFoundError, RuntimeError):
        source = selected
    valid = source.is_dir() and (source / 'exefs').is_dir() and (source / 'romfs').is_dir()
    if require_input and not valid:
        raise BuildError('Put your own already-extracted DE 2.0.0 files in game_files/exefs and game_files/romfs '\
                         'next to Build_Monado_II.sh. An existing game_files directory may be linked instead. '\
                         'No files will be moved or downloaded.')
    if source == port or source.is_relative_to(port) or port.is_relative_to(source):
        raise BuildError('game input must be disjoint from Linux build and runtime directories')
    if require_input:
        install_layout.shared_input(port)
    identity = identity or ('local-' + datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ-') + uuid.uuid4().hex[:8])
    if not identity or any(c not in 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_' for c in identity):
        raise BuildError('unsafe build identity')
    paths = dict(root=root, port=port, input=source, run=port / 'runs/local-build' / identity,
                 generated=port / 'generated' / identity, game=port / 'build' / (identity + '-game') / 'xcde-linux',
                 launcher=port / 'build' / (identity + '-launcher') / 'monado-launcher',
                 image=port / 'build' / (identity + '-appimage') / IMAGE, published=root / IMAGE,
                 state=install_layout.profile_port(port) / 'play/launcher')
    if reuse_game is not None:
        paths['game'] = ordinary(reuse_game)
        validate_reused_game(port, paths['game'], paths['state'], dlss=dlss)
    for name in ('run', 'generated', 'game', 'launcher', 'image'):
        if reuse_game is not None and name in ('game', 'generated'):
            continue
        path = paths[name]
        if name in ('game', 'launcher', 'image'):
            path = path.parent
        ordinary(path, directory=True, missing=True)
        if path.exists():
            raise BuildError('build outputs must be fresh: ' + str(path))
    python = port / '.venv/bin/python'
    commands = [
        ('check-input', [sys.executable, '-I', '-B', port / 'tools/extract/check_dump.py', source]),
        ('install-tools', [sys.executable, '-I', '-B', root / 'tools/setup_linux_tools.py', '--port', port, '--install']),
        ('check-tools', [sys.executable, '-I', '-B', root / 'tools/setup_linux_tools.py', '--port', port, '--check']),
        ('graphics-dependencies', [python, '-B', port / 'scripts/setup_linux_graphics.py']),
    ]
    if dlss:
        commands.append(('native-dlss-dependency', [python, '-B', port / 'scripts/setup_linux_dlss.py']))
    if reuse_game is None:
        commands += [
            ('generate-game', [python, '-B', port / 'tools/gen_all.py', '--exefs', source / 'exefs', '--out', paths['generated'], '--simd', 'c']),
            ('build-game', [python, '-B', port / 'scripts/build_linux.py', '--generated', paths['generated'], '--output', paths['game'].parent, '--jobs', jobs,
                            *(['--dlss'] if dlss else [])]),
        ]
    commands += [
        ('build-launcher', [python, '-B', port / 'scripts/build_launcher_linux.py', '--output', paths['launcher'].parent, '--jobs', jobs]),
        ('check-launcher', [python, '-B', port / 'scripts/launcher_linux.py', '--launcher-executable', paths['launcher'], '--game-executable', paths['game'], '--state-root', paths['state'], '--dry-run']),
        ('package-appimage', [sys.executable, '-I', '-B', root / 'tools/build_launcher_appimage.py', '--launcher-executable', paths['launcher'], '--game-executable', paths['game'], '--output', paths['image']]),
    ]
    return paths, [(name, list(map(str, command))) for name, command in commands]


def preflight(*, dlss=True):
    if sys.platform != 'linux' or platform.machine() != 'x86_64' or sys.version_info[:2] != (3, 14):
        raise BuildError('Build_Monado_II.sh requires Linux x86-64 and Python 3.14')
    required = REQUIRED_TOOLS + (DLSS_REQUIRED_TOOLS if dlss else ())
    missing = [name for name in required if shutil.which(name) is None]
    if missing:
        raise BuildError('Missing host build tools: ' + ', '.join(missing) + '. Install these with your distribution; '\
                         'mksquashfs/unsquashfs are supplied by squashfs-tools. This script never uses sudo.')
    if dlss and (not BWRAP.is_file() or not os.access(BWRAP, os.X_OK)):
        raise BuildError('DLSS/DLAA builds require /usr/bin/bwrap from bubblewrap for the existing private '
                         'runtime namespace. Install bubblewrap, or explicitly use --without-dlss. '
                         'No system packages are installed by this script.')
    if dlss and (not GNU_CXX_RUNTIME.is_file() or not os.access(GNU_CXX_RUNTIME, os.R_OK)):
        raise BuildError('DLSS/DLAA builds require the GNU C++ runtime at ' + str(GNU_CXX_RUNTIME) +
                         '. Install the compatible runtime, or explicitly use --without-dlss.')


def prepare_input_layout(root):
    """Populate only a newly created empty input directory, never an existing link."""
    root = ordinary(root, directory=True)
    selected = root / 'game_files'
    try:
        selected.lstat()
        return
    except FileNotFoundError:
        pass
    # The Linux entry preflight has already run. Directory descriptors prevent a
    # concurrent replacement with a link from redirecting these empty mkdirs.
    selected.mkdir(mode=0o700)
    descriptor = os.open(selected, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC)
    try:
        os.mkdir('exefs', mode=0o700, dir_fd=descriptor)
        os.mkdir('romfs', mode=0o700, dir_fd=descriptor)
    finally:
        os.close(descriptor)
    raise BuildError('Created empty game_files/exefs and game_files/romfs next to Build_Monado_II.sh. '\
                     'Put your own already-extracted DE 2.0.0 files there, then run the same build command again. '\
                     'No dependencies or game files were downloaded.')


def write_json(path, document):
    temporary = path.with_name(path.name + '.partial')
    ordinary(temporary, missing=True)
    with temporary.open('w') as stream:
        stream.write(json.dumps(document, indent=2) + '\n')
        stream.flush()
        os.fsync(stream.fileno())
    temporary.replace(path)


@contextlib.contextmanager
def build_lock(port):
    import fcntl  # Linux-only execution; source fixtures are also run on Windows.
    metadata = ordinary(port / '.deps/metadata', directory=True, missing=True)
    metadata.mkdir(parents=True, exist_ok=True)
    path = ordinary(metadata / 'appimage-build.lock', missing=True)
    descriptor = os.open(path, os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW | os.O_CLOEXEC, 0o600)
    try:
        info = os.fstat(descriptor)
        if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1:
            raise BuildError('build lock is not an ordinary single-link file')
        try:
            fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise BuildError('Another Monado II build is already running in this folder') from None
        yield
    finally:
        os.close(descriptor)


def child_id(pid):
    """Kernel process identity/ancestry, as used by the precompile supervisor."""
    try:
        data = Path('/proc', str(pid), 'stat').read_text()
        fields = data[data.rfind(')') + 2:].split()
        return (pid, int(fields[19])), int(fields[1]), fields[0]
    except (FileNotFoundError, ProcessLookupError):
        return None


def descendants():
    pending, found = [os.getpid()], {}
    for parent in pending:
        try:
            threads = list(Path('/proc', str(parent), 'task').iterdir())
        except (FileNotFoundError, ProcessLookupError):
            continue
        if len(threads) > 256:
            raise BuildError('build worker thread limit exceeded')
        for thread in threads:
            try:
                children = (thread / 'children').read_text().split()
            except (FileNotFoundError, ProcessLookupError):
                continue
            for word in children:
                pid = int(word)
                if pid in found:
                    continue
                item = child_id(pid)
                if item is None or item[1] != parent:
                    continue
                found[pid] = item
                pending.append(pid)
                if len(found) > 256:
                    raise BuildError('build worker process limit exceeded')
    return found


def signal_owned(owned, number):
    """Pidfds and a fresh kernel starttime check avoid PID-reuse/name guesses."""
    for pid, (identity, _parent, state) in owned.items():
        if state == 'Z':
            continue
        try:
            descriptor = os.pidfd_open(pid, 0)
        except ProcessLookupError:
            continue
        try:
            current = child_id(pid)
            if current is not None and current[0] == identity and current[2] != 'Z':
                signal.pidfd_send_signal(descriptor, number)
        except ProcessLookupError:
            pass
        finally:
            os.close(descriptor)


def reap_owned(child):
    if child is not None:
        child.poll()  # Preserve Popen's direct-child exit status.
    for pid, item in descendants().items():
        if (child is None or pid != child.pid) and item[2] == 'Z' and item[1] == os.getpid():
            try:
                os.waitpid(pid, os.WNOHANG)
            except ChildProcessError:
                pass
    return descendants()


@contextlib.contextmanager
def owned_children():
    if sys.platform != 'linux' or not hasattr(os, 'pidfd_open') or not hasattr(signal, 'pidfd_send_signal'):
        raise BuildError('Linux pidfd process ownership is required for local build supervision')
    if descendants():
        raise BuildError('build supervisor must not own unrelated existing children')
    libc = ctypes.CDLL(None, use_errno=True)
    previous = ctypes.c_int()
    if libc.prctl(37, ctypes.byref(previous), 0, 0, 0) or libc.prctl(36, 1, 0, 0, 0):
        raise OSError(ctypes.get_errno(), 'cannot enable build child subreaper')
    try:
        yield
    finally:
        # Never silently abandon adopted workers or call a stage successful while
        # they remain. A kernel-uninterruptible worker is a reported hard failure.
        remaining = descendants()
        if remaining:
            raise BuildError('owned build workers remain after cleanup: ' + repr(sorted(remaining)))
        if libc.prctl(36, previous.value, 0, 0, 0):
            raise OSError(ctypes.get_errno(), 'cannot restore build child subreaper')


def stop_owned(child, *, grace=5, timeout=30):
    started = time.monotonic()
    signal_owned(reap_owned(child), signal.SIGINT)
    while True:
        owned = reap_owned(child)
        if not owned:
            if child is not None:
                child.wait()
            return
        elapsed = time.monotonic() - started
        if elapsed >= grace:
            signal_owned(owned, signal.SIGKILL)
        if elapsed >= timeout:
            raise BuildError('owned build workers did not exit within the 30-second cleanup bound: ' + repr(sorted(owned)))
        time.sleep(0.02)


def execute(command, cwd, log, *, cancel_grace=5, cleanup_timeout=30):
    if not 0 < cancel_grace <= cleanup_timeout <= 30:
        raise BuildError('build cleanup requires 0 < grace <= timeout <= 30 seconds')
    environment = {'PATH': '/usr/bin:/bin', 'LANG': 'C.UTF-8', 'LC_ALL': 'C.UTF-8',
                   'PYTHONDONTWRITEBYTECODE': '1'}
    with owned_children(), log.open('x') as output, selectors.DefaultSelector() as selector:
        pending = []
        def request(number, _frame):
            if not pending:
                pending.append(number)
        handlers = {number: signal.signal(number, request) for number in
                    (signal.SIGINT, signal.SIGTERM, signal.SIGHUP)}
        child = None
        decoder = codecs.getincrementaldecoder('utf-8')(errors='replace')
        try:
            child = subprocess.Popen(command, cwd=cwd, env=environment, stdin=subprocess.DEVNULL,
                                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                     start_new_session=True, close_fds=True)
            selector.register(child.stdout, selectors.EVENT_READ)
            while True:
                owned = reap_owned(child)
                if pending:
                    raise KeyboardInterrupt
                if child.returncode is not None and not owned and not selector.get_map():
                    return child.wait()
                if child.returncode is not None and any(item[2] != 'Z' for item in owned.values()):
                    raise BuildError('build stage exited while owned workers were still running')
                for key, _mask in selector.select(timeout=0.05):
                    data = os.read(key.fileobj.fileno(), 65536)
                    text = decoder.decode(data, final=not data)
                    if text:
                        output.write(text)
                        output.flush()
                        print(text, end='', flush=True)
                    if not data:
                        selector.unregister(key.fileobj)
        finally:
            try:
                if child is not None and descendants():
                    stop_owned(child, grace=cancel_grace, timeout=cleanup_timeout)
                elif child is not None:
                    child.wait()
            finally:
                if child is not None:
                    child.stdout.close()
                for number, handler in handlers.items():
                    signal.signal(number, handler)


def copy_new(source, destination):
    ordinary(source)
    ordinary(destination, missing=True)
    with source.open('rb') as incoming, destination.open('xb') as output:
        shutil.copyfileobj(incoming, output)
        output.flush()
        os.fsync(output.fileno())
    destination.chmod(source.stat().st_mode & 0o777)
    if digest(source) != digest(destination):
        raise BuildError('copy identity changed: ' + str(source))


def publish(paths):
    """Preserve prior pair; caller rolls back if the final headless check fails."""
    source, target = paths['image'], paths['published']
    if image_pair(source) is None:
        raise BuildError('packager did not produce a verified AppImage/receipt pair')
    previous = image_pair(target)
    names = [IMAGE, IMAGE + '.receipt.json']
    backup = source.parent / 'previous'
    backup.mkdir()
    if previous is not None:
        for name in names:
            copy_new(target.parent / name, backup / name)
        image_pair(backup / IMAGE)
    staged = []
    for name in names:
        temporary = target.parent / ('.' + name + '-' + paths['run'].name + '.partial')
        copy_new(source.parent / name, temporary)
        staged.append(temporary)
    try:
        for temporary, name in zip(staged, names):
            temporary.replace(target.parent / name)
        image_pair(target)
    except BaseException:
        restore(paths, backup, previous is not None)
        raise
    return backup, previous is not None


def restore(paths, backup, had_previous):
    target = paths['published']
    for name in (IMAGE, IMAGE + '.receipt.json'):
        current = target.parent / name
        if current.exists():
            ordinary(current)
            current.rename(paths['image'].parent / ('failed-' + name))
        if had_previous:
            copy_new(backup / name, current)
    if had_previous:
        image_pair(target)


def build(root=ROOT, jobs=4, *, runner=execute, locker=build_lock, identity=None, dlss=True, reuse_game=None):
    preflight(dlss=dlss)
    prepare_input_layout(root)
    paths, commands = layout(root, jobs, identity=identity, dlss=dlss, reuse_game=reuse_game)
    with locker(paths['port']):
        # Recheck after obtaining the lease; another completed build may now be
        # present, but its known pair will be preserved rather than overwritten.
        image_pair(paths['published'])
        paths['run'].mkdir(parents=True)
        receipt_path = paths['run'] / 'receipt.json'
        receipt = dict(schema=1, status='running', scope='local game build and launcher-only AppImage; no game/UI execution',
                       started_utc=datetime.now(timezone.utc).isoformat(), jobs=jobs,
                       dlss_enabled=dlss,
                       reused_game=validate_reused_game(paths['port'], paths['game'], paths['state'], dlss=dlss) if reuse_game is not None else None,
                       paths={name: str(path) for name, path in paths.items()}, commands=[])
        write_json(receipt_path, receipt)
        print('Downloads: locked Python/compiler and Ubuntu development packages, plus the pinned AppImage runtime.'
              + (' The default DLSS/DLAA build also downloads the pinned official NVIDIA SDK into this workspace.' if dlss else
                 ' DLSS/DLAA support is explicitly omitted (--without-dlss).') + '\n'
              'Game files are never downloaded. Existing input and saves are preserved.\n'
              'Fresh build logs: ' + str(paths['run']), flush=True)
        backup = None
        try:
            for number, (name, command) in enumerate(commands, 1):
                log = paths['run'] / (f'{number:02d}-' + name + '.log')
                entry = dict(name=name, argv=command, cwd=str(paths['port']), log=str(log), status='running')
                receipt['commands'].append(entry)
                write_json(receipt_path, receipt)
                print(f'[{number}/{len(commands) + 1}] {name}', flush=True)
                started = time.monotonic()
                code = runner(command, paths['port'], log)
                entry.update(exit=code, seconds=time.monotonic() - started, status='passed' if code == 0 else 'failed')
                write_json(receipt_path, receipt)
                if code:
                    raise BuildError(f'{name} failed (exit {code}). Log: {log}')
            if reuse_game is not None and validate_reused_game(paths['port'], paths['game'], paths['state'], dlss=dlss) != receipt['reused_game']:
                raise BuildError('reused game receipt or identity changed during launcher build; previous image is preserved')
            backup = publish(paths)
            log = paths['run'] / f'{len(commands) + 1:02d}-check-appimage.log'
            command = [str(paths['published']), '--appimage-extract-and-run', '--dry-run']
            entry = dict(name='check-appimage', argv=command, cwd=str(paths['root']), log=str(log), status='running')
            receipt['commands'].append(entry)
            write_json(receipt_path, receipt)
            print(f'[{len(commands) + 1}/{len(commands) + 1}] check-appimage (no window or game)', flush=True)
            code = runner(command, paths['root'], log)
            entry.update(exit=code, status='passed' if code == 0 else 'failed')
            if code:
                raise BuildError(f'AppImage startup check failed (exit {code}). Log: {log}')
            receipt.update(status='passed', appimage_sha256=digest(paths['published']))
        except BaseException as error:
            if backup is not None:
                restore(paths, *backup)
            status = 'cancelled' if isinstance(error, KeyboardInterrupt) else 'failed'
            if receipt['commands'] and receipt['commands'][-1]['status'] == 'running':
                receipt['commands'][-1]['status'] = status
            receipt.update(status=status, error=str(error))
            raise
        finally:
            receipt['finished_utc'] = datetime.now(timezone.utc).isoformat()
            write_json(receipt_path, receipt)
        print('\nReady: ' + str(paths['published']) + '\nRun ./Play_Monado_II.sh to configure settings and press Play. '
              'Keep it beside Monado_Linux and game_files. No game was launched.', flush=True)
        return receipt


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--jobs', type=int, choices=range(1, 7), default=4, help='parallel compiler jobs (1–6)')
    parser.add_argument('--plan', action='store_true', help='print the local build steps without downloads or writes')
    parser.add_argument('--without-dlss', action='store_true',
                        help='explicitly omit the native NVIDIA DLSS/DLAA provider and SDK download')
    parser.add_argument('--reuse-game', type=Path,
                        help='retain an unchanged successful game build from this installation; rebuild only the launcher')
    args = parser.parse_args(argv)
    try:
        if args.plan:
            paths, commands = layout(ROOT, args.jobs, require_input=False, dlss=not args.without_dlss, reuse_game=args.reuse_game)
            print(json.dumps(dict(mode='plan', paths={name: str(path) for name, path in paths.items()},
                                  commands=[dict(name=name, argv=command) for name, command in commands],
                                  dlss_enabled=not args.without_dlss,
                                  reuses_game=args.reuse_game is not None,
                                  launches_game=False, launches_gui=False, network='none in plan; explicit build fetches locked dependencies'), indent=2))
        else:
            build(jobs=args.jobs, dlss=not args.without_dlss, reuse_game=args.reuse_game)
    except KeyboardInterrupt:
        print('Build cancelled; completed local work and previous AppImage preserved. See the build receipt/logs.', file=sys.stderr)
        return 130
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        print('Monado II build stopped: ' + str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
