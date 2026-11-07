#!/usr/bin/env python3
"""LINUX-009: bounded actual GUI settings/reopen/Play test in fresh private state.

Requires a real desktop session. --run-game explicitly enables the final Play
click and a strict game run ending at capture120; it is not a gameplay test.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys

from boot_smoke_linux import bounded_process, input_metadata
from launcher_linux import clean_environment
from run_linux import plan

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def collect_evidence(receipt, name, operation, failure):
    """[XCDE] LINUX-009: preserve the first failure, even if evidence I/O fails."""
    try:
        operation()
    except BaseException as error:
        receipt.setdefault('evidence_errors', {})[name] = {
            'error': str(error), 'error_type': type(error).__name__}
        if failure is None:
            failure = error
    return failure


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--launcher-executable', type=Path, required=True)
    parser.add_argument('--game-executable', type=Path, default=ROOT / 'build/linux-20261107-023-experimental/xcde-linux')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--settings-inspector', type=Path, required=True)
    parser.add_argument('--run-game', action='store_true')
    parser.add_argument('--trace', action='store_true')
    args = parser.parse_args()
    output = args.output.absolute()
    if output != output.resolve() or not output.is_relative_to(ROOT / 'runs') or output.exists():
        parser.error('output must be a fresh ordinary directory under Linux runs/')
    os.umask(0o077)
    state = output / 'state'
    gui = plan(args.launcher_executable, state)
    game = plan(args.game_executable, state)
    source = Path(game['input'])
    output.mkdir(parents=True)
    (state / 'build').mkdir(parents=True)
    receipt = {'scope': 'actual graphical settings/save/reopen and optional bounded strict Play; not gameplay',
               'status': 'running', 'argv': sys.argv, 'cwd': str(ROOT), 'launcher': gui, 'game': game,
               'input_before': input_metadata(source), 'phases': [], 'sources': {p: digest(ROOT / p) for p in (
                   'scripts/launcher_ui_tests_linux.py', 'scripts/launcher_linux.py', 'scripts/run_linux.py',
                   'scripts/boot_smoke_linux.py', 'scripts/platform_tests_linux.py')}, 'captures': {}}

    def save():
        temporary = output / 'receipt.json.tmp'
        temporary.write_text(json.dumps(receipt, indent=2) + '\n')
        temporary.replace(output / 'receipt.json')

    base = ['/usr/bin/python3', '-B', str(ROOT / 'scripts/launcher_linux.py'),
            '--launcher-executable', gui['argv'][0], '--game-executable', game['argv'][0],
            '--state-root', str(state)]
    environment = clean_environment(gui['library_path'])

    def phase(name, extra, seconds=30):
        folder = output / name
        folder.mkdir()
        argv = base + list(extra)
        if args.trace:
            argv = ['/usr/bin/strace', '--kill-on-exit', '-ff', '-ttt', '-s', '256', '-yy',
                    '-e', 'trace=%file,%process,socket,connect,bind,sendto,sendmsg',
                    '-e', 'raw=sendto,sendmsg', '-o', str(folder / 'syscalls'), *argv]
        row = {'name': name, 'argv': argv, 'seconds_limit': seconds}
        receipt['phases'].append(row)
        completion = {}
        failure = None
        def started(record):
            row['process'] = dict(record)
            save()
            print(name, 'started', record['pid'], flush=True)
        try:
            process, stdout, stderr = bounded_process(argv, cwd=ROOT, env=environment,
                                                      seconds=seconds, on_start=started, completion=completion)
        except BaseException as error:
            failure = error
        finally:
            if completion:
                row['process'] = completion['record']
                for stream in ('stdout', 'stderr'):
                    failure = collect_evidence(row, stream,
                        lambda stream=stream: (folder / (stream + '.log')).write_bytes(completion[stream]), failure)
            def copy_logs():
                for log in (state / 'logs').glob('*'):
                    if log.is_file() and log.suffix in ('.log', '.tsv'):
                        (folder / log.name).write_bytes(log.read_bytes())
            failure = collect_evidence(row, 'phase_logs', copy_logs, failure)
            failure = collect_evidence(row, 'phase_receipt', save, failure)
        if failure is not None:
            raise failure.with_traceback(failure.__traceback__)
        if (process['exit'] or process['timed_out'] or process['remaining_owned_pids'] or
                process.get('killed_owned_pids') or process.get('cleanup_errors')):
            raise RuntimeError(name + ' did not exit cleanly; inspect preserved phase logs')
        print(name, 'passed', flush=True)

    def inspect_settings():
        command = [str(args.settings_inspector.absolute()), 'inspect', str(state / 'xcde.ini')]
        result = subprocess.run(command, cwd=ROOT, stdin=subprocess.DEVNULL, capture_output=True,
                                text=True, timeout=15, env=environment)
        (output / 'settings-inspector.stdout.log').write_text(result.stdout)
        (output / 'settings-inspector.stderr.log').write_text(result.stderr)
        if result.returncode:
            raise RuntimeError('actual runtime settings parser rejected GUI configuration')
        receipt['runtime_settings'] = json.loads(result.stdout)
        if receipt['runtime_settings'].get('window') != [1600, 900] or receipt['runtime_settings'].get('show_fps') != 1:
            raise RuntimeError('runtime parser does not agree with the actual GUI edits')
        receipt['settings_inspector'] = {'argv': command, 'sha256': digest(args.settings_inspector.absolute())}

    save()
    failure = None
    try:
        before = state / 'build/launcher-before.bmp'
        phase('open', ['--shot', str(before)])
        width, height = struct.unpack_from('<ii', before.read_bytes(), 18)
        if width < 980 or height < 680:
            raise RuntimeError('launcher layout smaller than tested base geometry')
        sx, sy = width / 980, height / 680
        events = []
        def click(frame, x, y):
            events.extend((f'{frame} motion {round(x*sx)} {round(y*sy)}',
                           f'{frame+1} down 1', f'{frame+2} up 1'))
        click(20, 100, 190)  # Window size combo; choose the next item with actual keyboard events.
        events += ['25 key-down 81', '26 key-up 81', '29 key-down 40', '30 key-up 40']
        click(40, 20, 582)   # Show FPS checkbox.
        click(55, 440, 648)  # Save settings.
        script = state / 'save.events'
        script.write_text('\n'.join(events) + '\n')
        phase('save', ['--ui-script', str(script), '--shot', str(state / 'build/launcher-saved.bmp'), '--shot-frame', '90'])
        ini = (state / 'xcde.ini').read_text()
        values = dict(line.split('=', 1) for line in ini.splitlines() if '=' in line and not line.startswith('#'))
        values = {key.strip(): value.strip() for key, value in values.items()}
        if values.get('window') != '1600x900' or values.get('show_fps') != '1':
            raise RuntimeError('actual UI did not save the expected window/FPS edits')
        saved_hash = digest(state / 'xcde.ini')
        inspect_settings()
        phase('reopen', ['--shot', str(state / 'build/launcher-reopened.bmp')])
        if digest(state / 'xcde.ini') != saved_hash:
            raise RuntimeError('opening the launcher unexpectedly rewrote settings')
        receipt['saved_ini_sha256'] = saved_hash
        if args.run_game:
            play = state / 'play.events'
            play.write_text(f'20 motion {round(100*sx)} {round(648*sy)}\n21 down 1\n22 up 1\n')
            controls = ['XCDE_SHOTS=30:build/game-030.png,120:build/game-120.png',
                        'XCDE_EXIT_AFTER_SHOTS=1', 'LARECOMP_NO_USER_INPUT=1', 'LARECOMP_AUDIO_GAIN=0',
                        'XCDE_USAGE=logs/usage.tsv', 'XCDE_HLE_COVERAGE=logs/hle.tsv', 'XCDE_STALL=10']
            extra = ['--ui-script', str(play), '--test-exit-after-child']
            for control in controls:
                extra += ['--test-child-env', control]
            phase('play', extra, 90)
            for name in ('game-030.png', 'game-120.png'):
                image = state / 'build' / name
                dimensions = struct.unpack_from('>II', image.read_bytes(), 16)
                if dimensions != (1600, 900):
                    raise RuntimeError('game capture does not reflect the window size saved through the GUI')
        receipt['status'] = 'passed'
    except BaseException as error:
        failure = error
        receipt.update(status='failed', error=str(error), error_type=type(error).__name__)
    finally:
        def check_input_metadata():
            receipt['input_after'] = input_metadata(source)
            receipt['input_metadata_unchanged'] = receipt['input_before'] == receipt['input_after']
            if not receipt['input_metadata_unchanged']:
                receipt['input_error'] = 'game input metadata changed'
                raise RuntimeError(receipt['input_error'])
        def capture_images():
            for image in (state / 'build').glob('*'):
                if image.suffix in ('.bmp', '.png'):
                    receipt['captures'][image.name] = {'bytes': image.stat().st_size, 'sha256': digest(image)}
        failure = collect_evidence(receipt, 'input_after', check_input_metadata, failure)
        failure = collect_evidence(receipt, 'captures', capture_images, failure)
        if failure is not None:
            receipt.update(status='failed', error=str(failure), error_type=type(failure).__name__)
        failure = collect_evidence(receipt, 'final_receipt', save, failure)
    if failure is not None and not isinstance(failure, Exception):
        raise failure.with_traceback(failure.__traceback__)
    if failure is not None:
        receipt.update(status='failed', error=str(failure), error_type=type(failure).__name__)
    print(receipt['status'], output / 'receipt.json', flush=True)
    return 0 if receipt['status'] == 'passed' else 1


if __name__ == '__main__':
    raise SystemExit(main())
