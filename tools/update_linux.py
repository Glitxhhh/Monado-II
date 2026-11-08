#!/usr/bin/env python3
"""Monado II Linux updates: immutable local builds and durable version selection.

This module runs only in the outer launcher supervisor. It never opens, copies,
restores or migrates saves/configuration/caches, and never overwrites installed
source or builds. Network and compilation never run in the sandboxed GUI/game.
"""
from __future__ import annotations

import contextlib
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import signal
import stat
import subprocess
import sys
import threading
import time
import uuid

ROOT = Path(__file__).resolve().parents[1]
# Isolated AppImage/worker Python deliberately omits the script directory.
_import_path = list(sys.path)
sys.path.insert(0, str(ROOT / 'tools'))
try:
    import update_release
    from publication_guard import content_problem, path_problem, read_regular
finally:
    sys.path[:] = _import_path
IMAGE = 'Monado-II-x86_64.AppImage'
IDENTITY = re.compile(r'[a-zA-Z0-9][a-zA-Z0-9._-]{0,95}\Z')
HASH = re.compile(r'[0-9a-f]{64}\Z')
ACTIVE_KEYS = {'schema', 'version', 'id', 'workspace', 'image_sha256',
               'receipt_sha256', 'source_manifest_sha256'}


def ordinary(path, *, missing=False):
    path = Path(path).absolute()
    if '..' in path.parts or path.resolve() != path:
        raise ValueError('update path must be canonical without links or traversal')
    for parent in [*reversed(path.parents), path]:
        try:
            info = parent.lstat()
        except FileNotFoundError:
            if missing:
                continue
            raise
        if stat.S_ISLNK(info.st_mode):
            raise ValueError('update path contains a link')
        if parent != path and not stat.S_ISDIR(info.st_mode):
            raise ValueError('update path parent is not an ordinary directory')
        if parent == path and not (stat.S_ISDIR(info.st_mode) or
                (stat.S_ISREG(info.st_mode) and info.st_nlink == 1)):
            raise ValueError('update path is not an ordinary directory or single-link file')
    return path


def digest(path):
    return hashlib.sha256(read_regular(ordinary(path))).hexdigest()


def json_read(path, limit=2 * 1024 * 1024):
    raw = read_regular(ordinary(path))
    if len(raw) > limit:
        raise ValueError('update metadata exceeds size limit')
    def unique(pairs):
        value = {}
        for name, data in pairs:
            if name in value:
                raise ValueError('duplicate update metadata field')
            value[name] = data
        return value
    value = json.loads(raw, object_pairs_hook=unique)
    if not isinstance(value, dict):
        raise ValueError('update metadata must be an object')
    return value


def write_new(path, value):
    ordinary(path, missing=True)
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC | os.O_NOFOLLOW, 0o600)
    with os.fdopen(fd, 'wb') as stream:
        stream.write((json.dumps(value, indent=2, sort_keys=True) + '\n').encode())
        stream.flush()
        os.fsync(stream.fileno())


def durable_replace(path, value):
    ordinary(path, missing=True)
    temporary = path.parent / ('.' + path.name + '-' + uuid.uuid4().hex + '.partial')
    write_new(temporary, value)
    try:
        os.replace(temporary, path)
        fd = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
        try:
            os.fsync(fd)
        finally:
            os.close(fd)
    finally:
        temporary.unlink(missing_ok=True)


def layout_module(workspace):
    path = ordinary(Path(workspace) / 'Monado_Linux/scripts/install_layout.py')
    spec = importlib.util.spec_from_file_location('_monado_update_install_layout', path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def installed_sources(workspace):
    """Verify release-owned source bytes while leaving all runtime directories alone.

    Unlike the clean archive guard this intentionally does not enumerate runtime
    files. No game input, generated output, dependency cache or save is read.
    """
    workspace = ordinary(workspace)
    manifest = json_read(workspace / 'publication-manifest.json')
    policy = json_read(workspace / 'publication-policy.json')
    files = manifest.get('files')
    if (manifest.get('schema') != 1 or manifest.get('source_only') is not True or
            manifest.get('project') != 'Monado II' or not isinstance(files, dict) or
            not 1 <= len(files) <= 4096 or 'publication-policy.json' not in files):
        raise ValueError('installation has no supported exact source manifest')
    for name, identity in files.items():
        if (path_problem(name) or name == 'publication-manifest.json' or not isinstance(identity, dict)):
            raise ValueError('unsafe installed source manifest')
        data = read_regular(ordinary(workspace / name))
        if len(data) != identity.get('bytes') or hashlib.sha256(data).hexdigest() != identity.get('sha256'):
            raise ValueError('Local source changes detected; keep this installation and merge them manually: ' + name)
        problem = content_problem(name, data, policy.get('licensed_media', {}))
        if problem:
            raise ValueError('installed source policy rejected ' + name + ': ' + problem)
    return manifest


def version(workspace):
    metadata = json_read(Path(workspace) / 'update-version.json', 4096)
    if metadata.get('schema') != 1 or metadata.get('protocol') != 1:
        raise ValueError('unsupported installed updater version protocol')
    value = metadata.get('version')
    if not isinstance(value, str) or not re.fullmatch(r'0|[1-9][0-9]*', value.split('.')[0]) or not re.fullmatch(r'(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)', value):
        raise ValueError('invalid installed stable version')
    return value


def pair(image):
    # Image may contain a GUI bigger than the source guard's 8 MiB input limit.
    image = ordinary(image)
    if not image.is_file() or image.stat().st_size > 64 * 1024 * 1024 or not os.access(image, os.X_OK):
        raise ValueError('missing ordinary local launcher image')
    receipt = json_read(Path(str(image) + '.receipt.json'))
    with image.open('rb') as stream:
        header = stream.read(12)
        stream.seek(0)
        actual = hashlib.file_digest(stream, 'sha256').hexdigest()
    if (header[:4] != b'\x7fELF' or header[8:11] != b'AI\x02' or
            receipt.get('status') != 'passed' or receipt.get('artifact_kind') != 'local-launcher-appimage' or
            receipt.get('sha256') != actual or receipt.get('bytes') != image.stat().st_size or
            receipt.get('archive_readback_verified') is not True or
            any(receipt.get(key) is not False for key in ('contains_game_executable',
                'contains_game_files', 'contains_generated_game_code', 'contains_vendor_sdk'))):
        raise ValueError('updated image/receipt pair failed validation')
    return receipt


def active_record(owner, record, *, verify_sources=True):
    owner = ordinary(owner)
    if (set(record) != ACTIVE_KEYS or record.get('schema') != 1 or
            not isinstance(record.get('id'), str) or not IDENTITY.fullmatch(record['id']) or
            not isinstance(record.get('workspace'), str) or
            not all(isinstance(record.get(key), str) and HASH.fullmatch(record[key]) for key in
                    ('image_sha256', 'receipt_sha256', 'source_manifest_sha256'))):
        raise ValueError('invalid active update selection')
    expected = owner / 'Monado_Linux/updates' / record['id'] / 'workspace'
    if record['workspace'] != expected.relative_to(owner).as_posix():
        raise ValueError('active update escaped its immutable version directory')
    workspace = ordinary(expected)
    binding = json_read(workspace / 'installation.json', 8192)
    if binding.get('owner') != str(owner) or binding.get('workspace') != str(workspace):
        raise ValueError('active installation binding mismatch')
    module = layout_module(ROOT)
    if module.installation_root(workspace / 'Monado_Linux') != owner:
        raise ValueError('active update has a different owner')
    if version(workspace) != record['version']:
        raise ValueError('active version metadata mismatch')
    if digest(workspace / 'publication-manifest.json') != record['source_manifest_sha256']:
        raise ValueError('active source manifest changed')
    if verify_sources:
        installed_sources(workspace)
    receipt = pair(workspace / IMAGE)
    if receipt['sha256'] != record['image_sha256'] or digest(workspace / (IMAGE + '.receipt.json')) != record['receipt_sha256']:
        raise ValueError('active image identity mismatch')
    return workspace


def dispatch_active(workspace, argv):
    """The original workspace image is a stable bootstrap; versions stay put.

    Explicitly opening a managed version remains possible for checks/rollback.
    No network, GUI, game, save write or source execution from a download here.
    """
    workspace = ordinary(workspace)
    module = layout_module(workspace)
    owner = module.installation_root(workspace / 'Monado_Linux')
    if owner != workspace:
        return None
    selected = ordinary(owner / 'Monado_Linux/updates/active.json', missing=True)
    if not selected.exists():
        return None
    active = active_record(owner, json_read(selected, 8192))
    return [str(active / IMAGE), '--appimage-extract-and-run', *argv]


def build_command(workspace, game, rebuild):
    command = [str(workspace / 'Build_Monado_II.sh')]
    receipt = json_read(Path(game).parent / 'receipt.json')
    if not isinstance(receipt.get('dlss_enabled'), bool):
        raise ValueError('old game has no explicit feature receipt')
    framegen = receipt.get('framegen_enabled', False)
    if type(framegen) is not bool or (framegen and not receipt['dlss_enabled']):
        raise ValueError('old game has an invalid frame-generation feature receipt')
    if not receipt['dlss_enabled']:
        command.append('--without-dlss')
    if not rebuild:
        if framegen:
            command.append('--framegen')
        command += ['--reuse-game', str(game)]
    elif receipt.get('framegen_enabled') or receipt.get('foley_trace'):
        raise ValueError('This custom diagnostic/experimental game build requires a manual rebuild; the current version is preserved.')
    return command


def run_build(command, workspace, log, emit, cancelled, *, lock_fd=None):
    """A separate trusted subreaper owns only build workers, including setsid jobs."""
    invocation = [sys.executable, '-I', '-B', str(ROOT / 'tools/update_linux.py'),
                  '--build-worker', str(workspace), '--worker-log', str(log),
                  '--update-lock-fd', str(lock_fd), '--', *command]
    environment = {'PATH': '/usr/bin:/bin', 'LANG': 'C.UTF-8', 'LC_ALL': 'C.UTF-8'}
    output_path = log.with_suffix('.worker-console.log')
    with output_path.open('xb') as output:
        child = subprocess.Popen(invocation, cwd=workspace, env=environment,
                                 stdin=subprocess.DEVNULL, stdout=output, stderr=subprocess.STDOUT,
                                 close_fds=True, pass_fds=(lock_fd,), start_new_session=True)
        interrupted = None
        started = time.monotonic()
        offset = 0
        pending = b''
        while child.poll() is None:
            if (cancelled() or time.monotonic() - started > 3600) and interrupted is None:
                child.send_signal(signal.SIGINT)
                interrupted = time.monotonic()
            # Never forcibly kill the trusted cleanup owner. It retains its
            # subreaper and the update lease until detached children are gone.
            if log.exists():
                with log.open('rb') as incoming:
                    incoming.seek(offset)
                    data = incoming.read(65536)
                    offset += len(data)
                pending += data
                lines = pending.split(b'\n')
                pending = lines.pop()[-4096:]
                for line in lines:
                    stage = re.match(rb'\[(\d+)/(\d+)\] ([a-z-]+)', line)
                    if stage:
                        done, total = int(stage[1]), int(stage[2])
                        if 0 < done <= total <= 100:
                            emit({'phase': 'building', 'fraction': (done - 1) / total,
                                  'message': stage[3].decode().replace('-', ' ').capitalize() +
                                             f' ({done}/{total})'})
            time.sleep(0.1)
        code = child.wait()
    if interrupted is not None:
        raise update_release.UpdateCancelled('Update build cancelled; previous version remains selected.')
    if code:
        raise ValueError('Update build failed; previous version remains selected. Log: ' + str(log))


def build_worker(workspace, log, command, lock_fd):
    """Trusted cleanup process, never the GUI's broad session reaper."""
    workspace, log = ordinary(workspace), ordinary(log, missing=True)
    if not log.is_relative_to(workspace.parent) or command[0] != str(workspace / 'Build_Monado_II.sh'):
        raise ValueError('build worker must use its fixed candidate build command and log')
    import fcntl
    binding = json_read(workspace / 'installation.json', 8192)
    expected = ordinary(Path(binding['owner']) / 'Monado_Linux/updates/install.lock')
    info, held = expected.stat(), os.fstat(lock_fd)
    if not stat.S_ISREG(held.st_mode) or held.st_nlink != 1 or (info.st_dev, info.st_ino) != (held.st_dev, held.st_ino):
        raise ValueError('build worker update lease mismatch')
    fcntl.flock(lock_fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
    os.set_inheritable(lock_fd, False)
    previous = list(sys.path)
    sys.path.insert(0, str(ROOT / 'tools'))
    try:
        import build_linux_app
    finally:
        sys.path[:] = previous
    try:
        return build_linux_app.execute(command, workspace, log)
    finally:
        # execute already performs bounded pidfd cleanup. If an uninterruptible
        # worker remains, retain ownership until it exits rather than orphan it
        # or permit a second updater to claim that cleanup completed.
        while True:
            remaining = build_linux_app.reap_owned(None)
            if not remaining:
                break
            build_linux_app.signal_owned(remaining, signal.SIGKILL)
            time.sleep(0.1)


class Session:
    """One offer/install transaction owned by the existing launcher session lease."""
    def __init__(self, workspace, state, launcher, game, *, discover=None, fetch=None, runner=None):
        self.workspace = ordinary(workspace)
        self.state = ordinary(state, missing=True)
        self.launcher, self.game = ordinary(launcher), ordinary(game)
        self.layout = layout_module(self.workspace)
        self.owner = self.layout.installation_root(self.workspace / 'Monado_Linux')
        self.current_version = version(self.workspace)
        stable = self.owner / 'Monado_Linux'
        if not any(self.state != parent and self.state.is_relative_to(parent) for parent in (stable / 'play', stable / 'runs')):
            raise ValueError('updater session has no stable original profile')
        self.discover = discover or update_release.discover
        self.fetch = fetch or update_release.fetch_and_extract
        self.runner = runner or run_build
        self.offer = None
        self.lock_fd = None
        self.prepared = None
        self._busy = threading.Lock()

    def check(self):
        result = self.discover(self.current_version)
        if result is None:
            return None
        self.offer = dict(result, id=uuid.uuid4().hex)
        return {'id': self.offer['id'], 'version': self.offer['version'], 'rebuild': True,
                'message': 'This update may rebuild the game locally. Saves and settings stay in place.'}

    def _lock(self):
        import fcntl
        directory = ordinary(self.owner / 'Monado_Linux/updates', missing=True)
        directory.mkdir(mode=0o700, exist_ok=True)
        path = ordinary(directory / 'install.lock', missing=True)
        fd = os.open(path, os.O_RDWR | os.O_CREAT | os.O_CLOEXEC | os.O_NOFOLLOW, 0o600)
        if os.fstat(fd).st_nlink != 1 or not stat.S_ISREG(os.fstat(fd).st_mode):
            os.close(fd)
            raise ValueError('unsafe workspace update lock')
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BaseException:
            os.close(fd)
            raise ValueError('Another Monado II update is already running.') from None
        self.lock_fd = fd
        return directory

    def close(self):
        if self.lock_fd is not None:
            os.close(self.lock_fd)
            self.lock_fd = None

    def install(self, offer_id, progress, cancel):
        if not self._busy.acquire(blocking=False):
            raise ValueError('update already in progress')
        try:
            return self._install(offer_id, progress, cancel)
        except BaseException:
            self.close()
            raise
        finally:
            self._busy.release()

    def _install(self, offer_id, progress, cancel):
        if self.offer is None or offer_id != self.offer['id'] or self.prepared is not None:
            raise ValueError('stale or repeated update offer')
        cancelled = cancel.is_set
        if cancelled():
            raise update_release.UpdateCancelled('Update cancelled.')
        baseline = installed_sources(self.workspace)
        directory = self._lock()
        active = directory / 'active.json'
        prior = json_read(active, 8192) if active.exists() else None
        if prior is not None:
            active_record(self.owner, prior)
            current_active = self.owner / prior['workspace']
            if self.workspace != current_active:
                raise ValueError('This launcher is no longer the active version. Reopen the main launcher before updating.')
        identifier = 'update-' + uuid.uuid4().hex
        transaction = directory / identifier
        transaction.mkdir(mode=0o700)
        write_new(transaction / 'transaction.json', {'schema': 1, 'status': 'staging',
                  'version': self.offer['version'], 'previous': prior})
        source = self.fetch(self.offer, transaction / 'download', progress=progress, cancel=cancelled)
        candidate_manifest = update_release.verified_source(source)
        if version(source) != self.offer['version']:
            raise ValueError('downloaded version metadata differs from release')
        rebuild = update_release.classify(baseline, candidate_manifest) == 'game'
        workspace = transaction / 'workspace'
        source.rename(workspace)  # Before any build: absolute paths never change afterward.
        port = workspace / 'Monado_Linux'
        game_port = port if rebuild else self.layout.build_port(self.workspace / 'Monado_Linux', self.game)
        write_new(workspace / 'installation.json', {'schema': 1, 'owner': str(self.owner),
                  'workspace': str(workspace), 'game_port': str(game_port)})
        shared_input = (self.owner / 'game_files').resolve(strict=True)
        (workspace / 'game_files').symlink_to(shared_input, target_is_directory=True)
        command = build_command(workspace, self.game, rebuild)
        progress({'phase': 'building', 'message': 'Rebuilding the game and launcher locally. Saves stay in place.' if rebuild else
                  'Updating the launcher; keeping the verified game build and saves.'})
        if self.runner is run_build:
            self.runner(command, workspace, transaction / 'build.log', progress, cancelled, lock_fd=self.lock_fd)
        else:
            self.runner(command, workspace, transaction / 'build.log', progress, cancelled)
        if cancelled():
            raise update_release.UpdateCancelled('Update cancelled; previous version remains selected.')
        installed_sources(self.workspace)
        installed_sources(workspace)
        image_receipt = pair(workspace / IMAGE)
        # The builder's final AppImage --dry-run checks the exact input, selected
        # native receipts/resources and stable state without opening a game/UI.
        record = {'schema': 1, 'version': self.offer['version'], 'id': identifier,
                  'workspace': workspace.relative_to(self.owner).as_posix(),
                  'image_sha256': image_receipt['sha256'],
                  'receipt_sha256': digest(workspace / (IMAGE + '.receipt.json')),
                  'source_manifest_sha256': digest(workspace / 'publication-manifest.json')}
        active_record(self.owner, record)
        self.prepared = {'record': record, 'previous': prior, 'transaction': str(transaction), 'rebuild': rebuild}
        write_new(transaction / 'prepared.json', self.prepared)
        progress({'phase': 'ready', 'message': 'Update built and checked. Restarting the launcher; saves stay in place.'})
        return self.prepared

    def activate(self, prepared):
        """Call only after GUI/activity descendants exit, while profile lease is held."""
        if self.lock_fd is None or prepared is not self.prepared:
            raise ValueError('no owned prepared update to activate')
        record = prepared['record']
        workspace = active_record(self.owner, record)
        directory = self.owner / 'Monado_Linux/updates'
        selected = directory / 'active.json'
        actual = json_read(selected, 8192) if selected.exists() else None
        if actual != prepared['previous']:
            raise ValueError('active selection changed; preserve the prepared update and retry safely')
        # Previous record is durable before the one-file commit. Until replace,
        # crashes leave the old selection; no source/image/save files are replaced.
        durable_replace(directory / 'previous.json', {'schema': 1, 'selection': actual})
        try:
            durable_replace(selected, record)
        except BaseException:
            if actual is None:
                selected.unlink(missing_ok=True)
                fd = os.open(directory, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
                try: os.fsync(fd)
                finally: os.close(fd)
            else:
                durable_replace(selected, actual)
            raise
        self.close()
        return [str(workspace / IMAGE), '--appimage-extract-and-run', '--state-root', str(self.state)]


def rollback(owner):
    """Manual local rollback; source/game/profile data are never moved or restored."""
    owner = ordinary(owner)
    module = layout_module(owner)
    if module.installation_root(owner / 'Monado_Linux') != owner:
        raise ValueError('run rollback from the original installation')
    # Acquire the ordinary default profile lease as well as the update lock.
    scripts = owner / 'Monado_Linux/scripts'
    previous = list(sys.path)
    sys.path.insert(0, str(scripts))
    try:
        import launcher_linux
        lease = launcher_linux.acquire_lock(owner / 'Monado_Linux/play/launcher')
    finally:
        sys.path[:] = previous
    session = object.__new__(Session)
    session.owner, session.lock_fd = owner, None
    try:
        directory = session._lock()
        prior = json_read(directory / 'previous.json', 8192)
        if prior.get('schema') != 1 or set(prior) != {'schema', 'selection'}:
            raise ValueError('invalid previous version record')
        selection = prior['selection']
        if selection is None:
            (directory / 'active.json').unlink(missing_ok=True)
            fd = os.open(directory, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
            try: os.fsync(fd)
            finally: os.close(fd)
        else:
            active_record(owner, selection)
            durable_replace(directory / 'active.json', selection)
    finally:
        session.close()
        os.close(lease)


if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--rollback', action='store_true',
                        help='select the previous version while the original default profile is closed')
    parser.add_argument('--build-worker', type=Path, help=argparse.SUPPRESS)
    parser.add_argument('--worker-log', type=Path, help=argparse.SUPPRESS)
    parser.add_argument('--update-lock-fd', type=int, help=argparse.SUPPRESS)
    parser.add_argument('command', nargs=argparse.REMAINDER)
    options = parser.parse_args()
    if options.build_worker:
        command = options.command[1:] if options.command[:1] == ['--'] else options.command
        if not options.worker_log or not command or options.rollback or options.update_lock_fd is None:
            parser.error('invalid private build worker invocation')
        try:
            sys.exit(build_worker(options.build_worker, options.worker_log, command, options.update_lock_fd))
        except KeyboardInterrupt:
            sys.exit(130)
    elif options.rollback and not options.command:
        rollback(ROOT)
        print('Previous version selected. Open Play_Monado_II.sh; saves and settings are unchanged.')
    else:
        parser.error('choose --rollback')
