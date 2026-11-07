#!/usr/bin/env python3
"""[XCDE] LINUX-018: hidden actual SDL/ImGui precompile lifecycle with fake workers.

Production GUI at1267x804 plus an explicitly marked minimum-size test adapter.
SDL dummy/software never opens a desktop window. No game input, real shader
compiler, guest Play, human profile, external service or production source edit.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import resource
import shutil
import struct
import sys

from boot_smoke_linux import bounded_process

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_BUILD = ROOT / 'build/linux-20261107-023-experimental-gui'
FAKE = r'''# Authored UI fixture only; no game/compiler execution.
import argparse,json,os,pathlib,signal,sys,time
p=argparse.ArgumentParser();p.add_argument('--executable');p.add_argument('--state-root');p.add_argument('--session');a=p.parse_args()
state=pathlib.Path(a.state_root);history=state/'history.jsonl'
old=[json.loads(line) for line in history.read_text().splitlines()] if history.exists() else []
attempt=sum(x['event']=='start' for x in old); plan=json.loads((pathlib.Path(__file__).parent.parent/'plan.json').read_text());mode=plan[min(attempt,len(plan)-1)]
def event(kind,**values):
    with history.open('a') as f:f.write(json.dumps(dict(event=kind,session=a.session,pid=os.getpid(),mode=mode,ns=time.time_ns(),**values))+'\n');f.flush();os.fsync(f.fileno())
fields=dict(session=a.session,build='b'*64,identity='c'*64,supervisor_pid=os.getpid(),state='running',phase='scan',forced=0,
            files_total=10,files_done=4,shaders=5,programs=3,programs_done=0,seed_total=0,seed_done=0,variants_total=0,variants_done=0,
            attempted=0,built=0,failed=0,skipped=0,unsupported=2,errors=0,gpu_vendor=4318,gpu_device=10115,gpu_driver=1,
            gpu_cache_uuid='d'*32,message='Authored worker scanning files')
target=state/'cache/precompile_status.txt'
def write():
    temp=target.with_suffix('.tmp');temp.write_text('monado_precompile_status 1\n'+''.join(str(k)+' '+str(v)+'\n' for k,v in fields.items()));temp.replace(target)
def cancel(number,frame):
    fields.update(state='cancelling',message='Authored worker finishing current job');write();event('cancel-request')
    time.sleep(.15);fields.update(state='cancelled',phase='done',message='Authored worker cancelled');write();event('end',code=130);raise SystemExit(130)
signal.signal(signal.SIGTERM,cancel);event('start');write()
if mode=='hold':
    end=time.monotonic()+12
    while time.monotonic()<end:time.sleep(.005)
    event('end',code=9);sys.exit(9)
time.sleep(.08);fields.update(phase='build',files_done=10,programs_done=1,attempted=2,built=2,message='Authored worker preparing programs');write()
time.sleep(.08);fields.update(phase='done',files_done=10,programs_done=3,attempted=6,built=6,message='Authored worker complete')
code=0
if mode=='partial':fields.update(state='partial',attempted=5,built=4,failed=1,unsupported=3);code=2
elif mode=='partial-errors':fields.update(state='partial',attempted=4,built=4,failed=0,unsupported=3,errors=2);code=2
elif mode=='fatal':fields.update(state='failed',errors=1,message='Authored worker fatal failure');code=7
else:fields['state']='complete'
write();event('end',code=code);sys.exit(code)
'''


def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=DEFAULT_BUILD)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--framegen-package', action='store_true', help='authored companion-presence/INI fixtures only; exercise actual FG checkbox/save handlers without loading a provider')
    parser.add_argument('--framegen-settings-only', action='store_true', help='focus the FG settings cases, retaining the two baseline captures; requires --framegen-package')
    args = parser.parse_args()
    if args.framegen_settings_only and not args.framegen_package:
        parser.error('--framegen-settings-only requires --framegen-package')
    build, out = args.build.absolute(), args.output.absolute()
    if build != build.resolve() or not build.is_relative_to(ROOT / 'build'):
        parser.error('build must be a canonical project build directory')
    if out != out.resolve() or not out.is_relative_to(ROOT / 'runs') or out.exists():
        parser.error('output must be a fresh ordinary directory under Linux runs/')
    document = json.loads((build / 'receipt.json').read_text())
    if document.get('status') != 'passed' or document.get('artifact_kind') != 'linux-launcher':
        parser.error('a successful native launcher build is required')
    production = Path(document['executable']['path'])
    if digest(production) != document['executable']['sha256']:
        parser.error('launcher binary hash differs from its receipt')
    current = {name: digest(ROOT / name) for name in document['sources_after']}
    if current != document['sources_after']:
        parser.error('production sources differ from selected launcher build')
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0)); os.umask(0o077); out.mkdir(parents=True)
    library = Path(document['dependencies']['sysroot']) / 'usr/lib/x86_64-linux-gnu'
    env = {'PATH': '/usr/bin:/bin', 'LANG': 'C.UTF-8', 'LC_ALL': 'C.UTF-8',
           'LD_LIBRARY_PATH': str(library), 'SDL_VIDEO_DRIVER': 'dummy', 'SDL_RENDER_DRIVER': 'software',
           'TMPDIR': str(out), 'PYTHONDONTWRITEBYTECODE': '1'}
    source_names = ('scripts/launcher_precompile_ui_tests_linux.py', 'scripts/boot_smoke_linux.py',
                    'scripts/platform_tests_linux.py', 'scripts/run_linux.py', 'tools/launcher/linux_theme_test.cpp')
    receipt = {'scope': __doc__, 'status': 'running', 'argv': sys.argv, 'cwd': str(ROOT),
               'started_utc': datetime.now(timezone.utc).isoformat(), 'build_receipt_sha256': digest(build / 'receipt.json'),
               'binary_before': {'path': str(production), 'sha256': digest(production)},
               'production_sources_before': current, 'sources_before': {name: digest(ROOT / name) for name in source_names},
               'dependencies': {str(path.resolve()): digest(path) for path in (library / 'libSDL3.so', Path(document['dependencies']['sysroot']) / '.monado-deps.json')},
               'commands': [], 'cases': [], 'artifacts': {}}

    def save():
        (out / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')

    def run(name, argv, seconds=20):
        command = list(map(str, argv)); command_env = dict(env)
        if name == 'framegen-keyboard-save':
            command_env['MONADO_TEST_FG_KEYBOARD_FOCUS'] = '1'
        row = {'name': name, 'argv': command, 'cwd': str(ROOT), 'environment': command_env, 'seconds_limit': seconds}
        receipt['commands'].append(row); save(); completion = {}; failure = None
        try:
            bounded_process(command, cwd=ROOT, env=command_env, seconds=seconds, completion=completion)
        except BaseException as error:
            failure = error
        row.update(completion.get('record', {}))
        for stream in ('stdout', 'stderr'):
            path = out / (name + '.' + stream + '.log'); path.write_bytes(completion.get(stream, b'')); row[stream + '_sha256'] = digest(path)
        row['passed'] = not failure and row.get('exit') == 0 and not row.get('timed_out') and not row.get('killed_owned_pids') and not row.get('remaining_owned_pids') and not row.get('cleanup_errors')
        if failure:
            row['error'] = str(failure)
        save(); print(name, 'PASS' if row['passed'] else 'FAIL', flush=True)
        if isinstance(failure, (KeyboardInterrupt, SystemExit)):
            raise failure
        if not row['passed']:
            raise RuntimeError(name + ': inspect preserved logs')
        return completion.get('stdout', b'').decode(errors='replace')

    def widget(row, label, kind='Button'):
        kinds = ('Checkbox', 'FramegenCheckbox') if kind == 'FramegenCheckbox' else (kind,)
        return next(item for item in row['metadata']['widgets'] if item['type'] in kinds and item['label'] == label)

    def click(events, frame, item):
        x1, y1, x2, y2 = item['rect']; x, y = round((x1 + x2) / 2), round((y1 + y2) / 2)
        events.extend([f'{frame} motion {x} {y}', f'{frame + 1} down 1', f'{frame + 2} up 1'])

    save(); error = None; interrupted = None
    try:
        # Existing test adapter retains actual SDL/ImGui handlers. Enable only
        # authored children (remove its old fork-refusal macro), resize and
        # observe the shot frame. No widget/event activation is intercepted.
        harness = (ROOT / 'tools/launcher/linux_theme_test.cpp').read_text()
        needle = '#define fork test_forbid_fork\n'; assert harness.count(needle) == 1
        harness = harness.replace(needle, '')
        if args.framegen_package:
            needle = 'record_widget("CustomItem","",false); return result;'; assert harness.count(needle) == 1
            harness = harness.replace(needle, '''const char *label="";
    if(id==GetID(T("Generación de frames (FSR 3, experimental)")))label="Frame generation (FSR 3, experimental)";
    if(id==GetID(T("Generación de frames solo en cinemáticas (experimental)")))label="Frame generation in cutscenes only (experimental)";
    record_widget(*label?"FramegenCheckbox":"CustomItem",label,false); return result;''')
            needle = 'bool result=Checkbox(label,value); record_widget("Checkbox",label,result); return result;'; assert harness.count(needle) == 1
            harness = harness.replace(needle, '''if(getenv("MONADO_TEST_FG_KEYBOARD_FOCUS") && test_frame==30 && !strcmp(label,T("Generación de frames (FSR 3, experimental)")))SetKeyboardFocusHere();
    bool result=Checkbox(label,value); record_widget("Checkbox",label,result); return result;''')
            needle = 'static void TestNewFrame() {\n    NewFrame();'; assert harness.count(needle) == 1
            harness = harness.replace(needle, 'static void TestNewFrame() {\n    if(getenv("MONADO_TEST_FG_KEYBOARD_FOCUS") && test_frame==45)SetNavCursorVisible(true);\n    NewFrame();')
        needle = 'struct Window {'; assert harness.count(needle) == 1
        harness = harness.replace(needle, 'static std::vector<Widget> shot_widgets;\nstatic std::string shot_status;\nstatic float shot_fraction;\nstatic int shot_active, shot_observe_frame=20;\n' + needle)
        needle = 'last_widgets=frame_widgets;frame_widgets.clear();last_windows.clear();'; assert harness.count(needle) == 1
        harness = harness.replace(needle, 'last_widgets=frame_widgets;\nif(test_frame+1==shot_observe_frame){shot_widgets=frame_widgets;shot_status=g_status;shot_fraction=g_pc_frac;shot_active=launcher_linux::precompile_pid!=0;}\nframe_widgets.clear();last_windows.clear();')
        needle = 'int result=launcher_main_under_test(argc-4,argv+4);'; assert harness.count(needle) == 1
        harness = harness.replace(needle, 'for(int i=5;i+1<argc;i++)if(!strcmp(argv[i],"--shot-frame"))shot_observe_frame=atoi(argv[i+1]);\n' + needle + '\nif(!shot_widgets.empty())last_widgets=shot_widgets;\nfputs("PRECOMPILE_CAPTURE {\\"active\\":",stdout);printf("%s,\\"fraction\\":%.4f,\\"status\\":",shot_active?"true":"false",shot_fraction);quoted(shot_status);puts("}");')
        if args.framegen_package:
            needle = 'quoted(shot_status);puts("}");'; assert harness.count(needle) == 1
            harness = harness.replace(needle, r'quoted(shot_status);printf(",\"frame_gen\":%s,\"cut_fg\":%s}\n",g.frame_gen?"true":"false",g.cut_fg?"true":"false");')
        harness_source = out / 'hidden-precompile-harness.cpp'; harness_source.write_text(harness)
        compile_record = next(row for row in document['commands'] if '-c' in row['argv'] and row['argv'][row['argv'].index('-c') + 1].endswith('tools/launcher/launcher.cpp'))
        argv = list(compile_record['argv']); argv[argv.index('-c') + 1] = str(harness_source); argv[argv.index('-o') + 1] = str(out / 'harness.o')
        argv += ['-I' + str(ROOT), '-I' + str(ROOT / 'tools/launcher')]
        run('compile-hidden-harness', argv, 60)
        argv = list(next(row for row in document['commands'] if row['name'] == 'link-launcher')['argv'])
        original_object = document['objects']['tools/launcher/launcher.cpp']['path']; assert original_object in argv
        argv[argv.index(original_object)] = str(out / 'harness.o'); harness_binary = out / 'hidden-precompile-launcher'
        argv[argv.index('-o') + 1] = str(harness_binary); run('link-hidden-harness', argv, 60)
        receipt['artifacts'].update({harness_source.name: digest(harness_source), harness_binary.name: digest(harness_binary)})
        if (build / 'icon.png').is_file():
            shutil.copyfile(build / 'icon.png', out / 'icon.png'); receipt['artifacts']['icon.png'] = digest(out / 'icon.png')

        def case(name, *, width=1267, height=804, instrumented=False, modes=('complete',), events=(), shot_frame=220, expected_codes=(), framegen_package=None, dlss_package=None, initial_ini=None, expect_ini_change=False):
            work = out / name; project = work / 'project'; source = work / 'authored-input'; state = work / 'state'
            (project / 'scripts').mkdir(parents=True); (source / 'exefs').mkdir(parents=True); (source / 'romfs').mkdir()
            (source / 'exefs/main').write_text('AUTHORED TEST PLACEHOLDER; NO GAME DATA\n')
            fake_game = project / 'fake-game'; fake_game.write_text('AUTHORED NEVER EXECUTED\n'); fake_game.chmod(0o700)
            if framegen_package is None: framegen_package = args.framegen_package
            if dlss_package is None: dlss_package = framegen_package
            if dlss_package:
                for filename in ('libmonado-ngx.so', 'libnvidia-ngx-dlss.so.310.9.1'):
                    (project / filename).write_text('AUTHORED GUI PRESENCE FIXTURE ONLY; NEVER LOADED\n')
            if framegen_package:
                for filename in ('libmonado-fidelityfx.so', 'FidelityFX-MIT.txt'):
                    (project / filename).write_text('AUTHORED GUI PRESENCE FIXTURE ONLY; NEVER LOADED\n')
            supervisor = project / 'scripts/precompile_linux.py'; supervisor.write_text(FAKE); (project / 'plan.json').write_text(json.dumps(list(modes)))
            (project / 'scripts/run_linux.py').write_text('import pathlib,sys\ns=pathlib.Path(sys.argv[sys.argv.index("--state-root")+1]);(s/"PLAY_WAS_LAUNCHED").write_text("BUG")\nraise SystemExit(99)\n')
            (state / 'build').mkdir(parents=True)
            ini = state / 'xcde.ini'; ini.write_text(initial_ini if initial_ini is not None else 'launcher_lang = en\nfullscreen = 0\nwindow = 1600x900\nupscaler = '+('dlss' if dlss_package else 'bilinear')+'\nfps = 60\nvolume = 67\n')
            before_ini = digest(ini); shot = state / 'build/view.bmp'
            native = ['--project-root', str(project), '--input-root', str(source), '--game-executable', str(fake_game),
                      '--state-root', str(state), '--shot', str(shot), '--shot-frame', str(shot_frame)]
            if events:
                script = state / 'ui.events'; script.write_text('\n'.join(events) + '\n'); native += ['--ui-script', str(script)]
            argv = [str(harness_binary), str(width), str(height), '1.0', 'normal', *native] if instrumented else [str(production), *native]
            stdout = run(name, argv)
            data = shot.read_bytes(); assert data[:2] == b'BM' and struct.unpack_from('<ii', data, 18) == (width, height)
            unchanged = digest(ini) == before_ini
            assert unchanged != expect_ini_change, 'GUI settings save behavior differs from the scenario'
            assert not (state / 'PLAY_WAS_LAUNCHED').exists(), 'Play was incorrectly launched'
            history = [json.loads(line) for line in (state / 'history.jsonl').read_text().splitlines()] if (state / 'history.jsonl').exists() else []
            starts = [item for item in history if item['event'] == 'start']; ends = [item for item in history if item['event'] == 'end']
            assert [item['code'] for item in ends] == list(expected_codes), history
            assert len(starts) == len(ends) and len({item['session'] for item in starts}) == len(starts)
            row = {'name': name, 'instrumented_extent_adapter': instrumented, 'dimensions': [width, height],
                   'history': history, 'shot': str(shot), 'bmp_sha256': digest(shot), 'ini_unchanged': unchanged,
                   'fake_supervisor_sha256': digest(supervisor), 'fake_input_sha256': digest(source / 'exefs/main')}
            row['ini_values'] = {line.split('=',1)[0].strip():line.split('=',1)[1].strip() for line in ini.read_text().splitlines() if '=' in line and not line.lstrip().startswith('#')}
            row['authored_framegen_presence_fixture'] = bool(framegen_package)
            if instrumented:
                row['metadata'] = json.loads(next(line[len('THEME_TEST '):] for line in stdout.splitlines() if line.startswith('THEME_TEST ')))
                row['capture'] = json.loads(next(line[len('PRECOMPILE_CAPTURE '):] for line in stdout.splitlines() if line.startswith('PRECOMPILE_CAPTURE ')))
                assert not row['metadata']['fork_refusals']
            log = state / 'logs/launcher.log'; row['launcher_log'] = log.read_text()
            assert 'launcher exit 0' in row['launcher_log']
            receipt['cases'].append(row); save(); return row

        baselines = {}
        for width, height in ((1267, 804), (640, 480)):
            row = case(f'baseline-{width}', width=width, height=height, instrumented=True, modes=(), shot_frame=20)
            baselines[width] = row
            for label in ('PLAY', 'Precompile shaders', 'Save settings', 'Create report', 'Quit'):
                item = widget(row, label); x1, y1, x2, y2 = item['rect']
                assert item['visible'] and not item['disabled'] and 0 <= x1 < x2 <= width and 0 <= y1 < y2 <= height
        if not args.framegen_settings_only:
            probes = {}
            for width, height in ((1267, 804), (640, 480)):
                events = []; click(events, 10, widget(baselines[width], 'Precompile shaders')); click(events, 100, widget(baselines[width], 'Quit'))
                row = case(f'progress-quit-{width}', width=width, height=height, instrumented=True, modes=('hold',), events=events, shot_frame=55, expected_codes=(130,))
                assert row['capture']['active'] and abs(row['capture']['fraction'] - .4) < .01
                assert widget(row, 'PLAY')['disabled'] and widget(row, 'Precompile shaders')['disabled']
                stop = widget(row, 'Stop'); x1, y1, x2, y2 = stop['rect']
                assert stop['visible'] and not stop['disabled'] and 0 <= x1 < x2 <= width and 0 <= y1 < y2 <= height
                assert 'cancelled' in row['launcher_log']; probes[width] = row
            # Exact production ELF: scripted real mouse events drive its existing
            # handlers. Only the selected supervisor/project/input are authored fake.
            events = []; click(events, 10, widget(baselines[1267], 'Precompile shaders'))
            row = case('production-complete', events=events, expected_codes=(0,))
            assert '6 built, 2 not precompiled' in row['launcher_log']
            events = []; click(events, 10, widget(baselines[640], 'Precompile shaders')); events.append('100 motion 600 100')
            row = case('partial-errors-640', width=640, height=480, instrumented=True, modes=('partial-errors',), events=events, expected_codes=(2,))
            assert not row['capture']['active'] and 'Incomplete:' in row['capture']['status'] and '0 failed, 2 errors' in row['capture']['status']
            for label in ('PLAY', 'Precompile shaders', 'Save settings', 'Create report', 'Quit'):
                item = widget(row, label); x1, y1, x2, y2 = item['rect']
                assert item['visible'] and not item['disabled'] and 0 <= x1 < x2 <= 640 and 0 <= y1 < y2 <= 480
            for first, code in (('partial', 2), ('fatal', 7)):
                events = []; click(events, 10, widget(baselines[1267], 'Precompile shaders')); click(events, 140, widget(baselines[1267], 'Precompile shaders'))
                row = case('production-' + first + '-retry', modes=(first, 'complete'), events=events, shot_frame=240, expected_codes=(code, 0))
                assert '(exit ' + str(code) + ')' in row['launcher_log'] and '6 built, 2 not precompiled' in row['launcher_log']
            for width, height in ((1267, 804), (640, 480)):
                events = []; click(events, 10, widget(baselines[width], 'Precompile shaders')); click(events, 65, widget(baselines[width], 'PLAY'))
                click(events, 80, widget(probes[width], 'Stop')); click(events, 170, widget(baselines[width], 'Precompile shaders'))
                row = case(f'cancel-retry-{width}', width=width, height=height, instrumented=width == 640,
                           modes=('hold', 'complete'), events=events, shot_frame=270, expected_codes=(130, 0))
                assert 'cancelled' in row['launcher_log'] and '6 built, 2 not precompiled' in row['launcher_log']
            events = []; click(events, 10, widget(baselines[1267], 'Precompile shaders')); click(events, 100, widget(baselines[1267], 'Quit'))
            row = case('production-quit-while-compiling', modes=('hold',), events=events, shot_frame=55, expected_codes=(130,))
            assert 'cancelled' in row['launcher_log']
        if args.framegen_package:
            whole = widget(baselines[1267], 'Frame generation (FSR 3, experimental)', 'FramegenCheckbox')
            cut = widget(baselines[1267], 'Frame generation in cutscenes only (experimental)', 'FramegenCheckbox')
            assert whole['visible'] and not whole['disabled'] and cut['visible'] and not cut['disabled']
            events = []; click(events, 10, whole); click(events, 70, widget(baselines[1267], 'Save settings'))
            mouse = case('framegen-mouse-save', modes=(), events=events, shot_frame=120, expect_ini_change=True)
            assert mouse['ini_values']['frame_gen']=='1' and mouse['ini_values']['upscaler']=='dlss'
            saved = (out/'framegen-mouse-save/state/xcde.ini').read_text()
            reload = case('framegen-enabled-reload', instrumented=True, modes=(), initial_ini=saved, shot_frame=20)
            assert reload['capture']['frame_gen'] and widget(reload,'Frame generation in cutscenes only (experimental)','FramegenCheckbox')['disabled']
            events = []; click(events,10,whole);events += ['50 key-down 44','51 key-up 44'];click(events,90,widget(baselines[1267],'Save settings'))
            # The existing observer supplies a synthetic window-focus event.
            # SDL dummy has no physical keyboard focus; use actual handlers and
            # activation with explicit test keyboard focus before the actual
            # checkbox, rather than claiming native desktop navigation/focus.
            keyboard = case('framegen-keyboard-save',instrumented=True,modes=(),events=events,shot_frame=120,expect_ini_change=True)
            assert keyboard['ini_values']['frame_gen']=='0' and keyboard['ini_values']['fullscreen']=='0' and keyboard['ini_values']['upscaler']=='dlss'
            events=[];click(events,10,cut);click(events,70,widget(baselines[1267],'Save settings'))
            cutrow=case('framegen-cutscene-save',modes=(),events=events,shot_frame=120,expect_ini_change=True)
            assert cutrow['ini_values']['frame_gen']=='0' and cutrow['ini_values']['cutscenes']=='30fg'
            saved=(out/'framegen-cutscene-save/state/xcde.ini').read_text()
            reload=case('framegen-cutscene-reload',instrumented=True,modes=(),initial_ini=saved,shot_frame=20)
            assert not reload['capture']['frame_gen'] and reload['capture']['cut_fg']
            fallback=case('framegen-old-package-disabled',instrumented=True,modes=(),framegen_package=False,dlss_package=True,initial_ini=saved,shot_frame=20)
            assert not fallback['capture']['frame_gen'] and not fallback['capture']['cut_fg']
            assert widget(fallback,'Frame generation (FSR 3, experimental)','FramegenCheckbox')['disabled'] and widget(fallback,'Frame generation in cutscenes only (experimental)','FramegenCheckbox')['disabled']
    except BaseException as failure:
        error = str(failure) or type(failure).__name__
        if isinstance(failure, (KeyboardInterrupt, SystemExit)):
            interrupted = failure
    finally:
        receipt['production_sources_after'] = {name: digest(ROOT / name) for name in current}
        receipt['sources_after'] = {name: digest(ROOT / name) for name in source_names}
        receipt['binary_after_sha256'] = digest(production)
        if receipt['production_sources_after'] != current or receipt['sources_before'] != receipt['sources_after'] or receipt['binary_after_sha256'] != receipt['binary_before']['sha256']:
            error = (error + '; ' if error else '') + 'production/test source or binary changed during test'
        receipt.update(status='failed' if error else 'passed', error=error, finished_utc=datetime.now(timezone.utc).isoformat())
        save()
    print(receipt['status'], out / 'receipt.json')
    if interrupted:
        raise interrupted
    return 1 if error else 0


if __name__ == '__main__':
    sys.exit(main())
