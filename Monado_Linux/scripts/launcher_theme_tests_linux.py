#!/usr/bin/env python3
"""[XCDE] LINUX-015: bounded actual-launcher presentation verification. Never Play/game.

Compile from passed launcher receipt, changing only its launcher translation unit
for test-only extent/DPI/event/widget instrumentation. Retains actual ImGui/SDL
renderer and all real launcher handlers. Dummy backend never displays a window.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import struct
import sys
ROOT = Path(__file__).resolve().parents[1]
HARNESS = ROOT / 'tools/launcher/linux_theme_test.cpp'
from boot_smoke_linux import bounded_process
from launcher_linux import arguments as launcher_arguments

def digest(p):
    with Path(p).open('rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()

def now():
    return datetime.now(timezone.utc).isoformat()

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--build', type=Path, required=True)
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--source', type=Path, help='optional saved baseline launcher.cpp under runs/')
    ap.add_argument('--case', action='append')
    ap.add_argument('--production', action='store_true', help='hidden production ELF captures only, all four tabs in both languages')
    ap.add_argument('--game-executable', type=Path, default=launcher_arguments([])[1].game_executable, help='path checked for existence only; never launched')
    args = ap.parse_args()
    build = args.build.resolve()
    out = args.output.absolute()
    if not (build.is_relative_to(ROOT / 'build') or build.is_relative_to(ROOT / 'runs')):
        ap.error('build must be under this port build/ or runs/')
    if args.source and (args.production or not args.source.resolve().is_relative_to(ROOT / 'runs')):
        ap.error('baseline source requires component mode and a saved source under runs/')
    if out.exists() or out != out.resolve() or (not out.is_relative_to(ROOT / 'runs')):
        ap.error('fresh canonical runs/ output required')
    r = json.loads((build / 'receipt.json').read_text())
    if r.get('status') != 'passed' or r.get('artifact_kind') != 'linux-launcher':
        ap.error('a passed native Linux launcher build receipt is required')
    os.umask(0o077)
    out.mkdir(parents=True)
    (out / 'render-harness.cpp').write_bytes(HARNESS.read_bytes())
    (out / 'render-checks.py').write_bytes(Path(__file__).read_bytes())
    env = {'PATH': '/usr/bin:/bin', 'LANG': 'C.UTF-8', 'LC_ALL': 'C.UTF-8', 'LD_LIBRARY_PATH': r['dependencies']['sysroot'] + '/usr/lib/x86_64-linux-gnu', 'SDL_VIDEO_DRIVER': 'dummy', 'SDL_RENDER_DRIVER': 'software', 'TMPDIR': str(out)}
    snapshot = {k: digest(ROOT / k) for k in r['sources_after']}
    if not args.source:
        assert snapshot == r['sources_after'], 'current sources do not match passed build receipt'
    receipt = {'scope': 'Actual SDL3/ImGui component renders with injected extent/scale, dummy/software renderer; production hidden dummy captures separately marked. No physical desktop DPI/WM or game validation.', 'started_utc': now(), 'status': 'running', 'build_receipt_sha256': digest(build / 'receipt.json'), 'sources_before': {'tools/launcher/launcher.cpp': digest(ROOT / 'tools/launcher/launcher.cpp'), 'harness': digest(HARNESS), 'runner': digest(Path(__file__))}, 'commands': [], 'cases': [], 'errors': [], 'production_sources_before': snapshot}

    def save():
        p = out / 'receipt.json.tmp'
        p.write_text(json.dumps(receipt, indent=2) + '\n')
        p.replace(out / 'receipt.json')

    def run(name, argv, seconds=90):
        row = {'name': name, 'argv': list(map(str, argv)), 'cwd': str(ROOT), 'started_utc': now(), 'timeout_seconds': seconds}
        receipt['commands'].append(row)
        save()
        completion = {}
        failure = None

        def started(process):
            row.update(process)
            save()
        try:
            process, stdout, stderr = bounded_process(row['argv'], cwd=ROOT, env=env, seconds=seconds, on_start=started, completion=completion)
        except BaseException as error:
            failure = error
        finally:
            row.update(completion.get('record', {}))
            stdout = completion.get('stdout', b'')
            stderr = completion.get('stderr', b'')
            row.update(stdout_sha256=hashlib.sha256(stdout).hexdigest(), stderr_sha256=hashlib.sha256(stderr).hexdigest())
            (out / (name + '.stdout')).write_bytes(stdout)
            (out / (name + '.stderr')).write_bytes(stderr)
            save()
        if failure is not None:
            raise failure.with_traceback(failure.__traceback__)
        assert not row['timed_out'] and (not row['exit']) and (not row['remaining_owned_pids']) and (not row['killed_owned_pids']) and (not row.get('cleanup_errors')), (name, row)
        return stdout
    try:
        if args.production:
            exe = Path(r['executable']['path'])
            assert digest(exe) == r['executable']['sha256']
            receipt['instrumented'] = False
        else:
            receipt['instrumented'] = True
            cc = next((c for c in r['commands'] if '-c' in c['argv'] and c['argv'][c['argv'].index('-c') + 1].endswith('tools/launcher/launcher.cpp')))
            av = list(cc['argv'])
            av[av.index('-c') + 1] = str(HARNESS)
            av[av.index('-o') + 1] = str(out / 'harness.o')
            av.extend(['-I' + str(ROOT), '-I' + str(ROOT / 'tools/launcher')])
            if args.source:
                av.append('-DTHEME_LAUNCHER_SOURCE=' + json.dumps(str(args.source.resolve())))
                receipt['baseline_source_sha256'] = digest(args.source)
            run('compile-harness', av)
            link = next((c for c in r['commands'] if c['name'] == 'link-launcher'))
            av = list(link['argv'])
            old = r['objects']['tools/launcher/launcher.cpp']['path']
            assert old in av
            av[av.index(old)] = str(out / 'harness.o')
            exe = out / 'render-launcher'
            av[av.index('-o') + 1] = str(exe)
            run('link-harness', av)
            receipt['harness_executable_sha256'] = digest(exe)
        cases = []
        base_width, base_height = (1267, 804) if args.production else (980, 680)
        for language in ('en', 'es'):
            for tab in range(4):
                cases.append((f'base-{language}-tab{tab}', base_width, base_height, 1.0, language, tab, 'normal'))
        if not args.production:
            for language in ('en', 'es'):
                for tab in range(4):
                    cases.append((f'minimum-{language}-tab{tab}',640,480,1.0,language,tab,'normal'))
            cases.extend([('minimum-dpi200-es',1280,960,2.0,'es',1,'normal'),('minimum-es-graphics-bottom',640,480,1.0,'es',0,'bottom'),('minimum-es-controls-bottom',640,480,1.0,'es',1,'bindings-bottom')])
            cases.extend([('large-en-graphics', 1580, 960, 1.0, 'en', 0, 'normal'), ('large-en-controls', 1580, 960, 1.0, 'en', 1, 'normal'), ('small-es-graphics', 680, 640, 1.0, 'es', 0, 'normal'), ('small-es-controls', 680, 640, 1.0, 'es', 1, 'normal'), ('dpi150-es', 1470, 1020, 1.5, 'es', 0, 'normal'), ('dpi200-en', 1960, 1360, 2.0, 'en', 0, 'normal'), ('base-en-graphics-bottom', 980, 680, 1.0, 'en', 0, 'bottom'), ('small-es-controls-bottom', 680, 640, 1.0, 'es', 1, 'bindings-bottom'), ('no-art-en', 980, 680, 1.0, 'en', 0, 'no-art'), ('long-status-es', 680, 640, 1.0, 'es', 0, 'long-status'), ('focus-en', 980, 680, 1.0, 'en', 0, 'focus'), ('popup-en-window', 980, 680, 1.0, 'en', 0, 'popup'), ('popup-small-es-resolution', 680, 640, 1.0, 'es', 0, 'popup'), ('tooltip-en-precompile', 980, 680, 1.0, 'en', 0, 'tooltip'), ('unsaved-fullscreen', 980, 680, 1.0, 'en', 0, 'unsaved'), ('saved-fullscreen', 980, 680, 1.0, 'en', 0, 'save'), ('language-live', 980, 680, 1.0, 'en', 0, 'language'), ('wrapped-probe', 980, 680, 1.0, 'en', 0, 'wrapped-probe'), ('wrapped-mouse', 980, 680, 1.0, 'en', 0, 'wrapped-mouse'), ('wrapped-keyboard', 980, 680, 1.0, 'en', 0, 'wrapped-keyboard')])
        if args.source and (not args.case):
            cases = [c for c in cases if c[0].startswith('base-')]
        if args.case:
            selected = set(args.case)
            assert selected <= set((c[0] for c in cases))
            dependencies = {'popup-en-window': ['base-en-tab0'], 'popup-small-es-resolution': ['base-es-tab0', 'small-es-graphics'], 'tooltip-en-precompile': ['base-en-tab0'], 'unsaved-fullscreen': ['base-en-tab0'], 'saved-fullscreen': ['base-en-tab0'], 'language-live': ['base-en-tab0'], 'wrapped-mouse': ['wrapped-probe']}
            for name in list(selected):
                selected.update(dependencies.get(name, []))
            cases = [c for c in cases if c[0] in selected]
        for name, width, height, scale, lang, tab, mode in cases:
            state = out / name / 'state'
            (state / 'build').mkdir(parents=True)
            ini = state / 'xcde.ini'
            ini.write_text('launcher_lang = ' + lang + '\nwindow = 1600x900\nfullscreen = 0\nfps = 165\nmenu_navigation_speed = fast\nvolume = 67\nbutton_icons = switch\nshow_fps = 0\ncustom_theme_preservation = intact\n')
            before = digest(ini)
            shot = state / 'build/view.bmp'
            events = []
            target = None

            def known(case):
                return next((c['metadata']['widgets'] for c in receipt['cases'] if c['name'] == case))

            def item(items, kind=None, label=None, id=None):
                return next((w for w in items if (kind is None or w['type'] == kind) and (label is None or w['label'] == label) and (id is None or w['id'] == id)))

            def click(frame, w):
                x1, y1, x2, y2 = w['rect']
                x = round(x1 + min(20 * scale, (x2 - x1) / 2))
                y = round((y1 + y2) / 2)
                events.extend([f'{frame} motion {x} {y}', f'{frame + 1} down 1', f'{frame + 2} up 1'])
            if mode in ('popup', 'tooltip', 'unsaved', 'save', 'language', 'wrapped-mouse', 'wrapped-keyboard'):
                base = None if mode.startswith('wrapped') else known('small-es-graphics' if name == 'popup-small-es-resolution' else 'base-en-tab0')
                if mode == 'popup':
                    original = known('base-es-tab0') if lang == 'es' else base
                    label = 'Resolución interna' if lang == 'es' else 'Window size'
                    target = item(original, kind='Combo', label=label)['id']
                    click(10, item(base, id=target))
                elif mode == 'tooltip':
                    w = item(base, kind='Button', label='Precompile shaders')
                    x1, y1, x2, y2 = w['rect']
                    events = [f'10 motion {round((x1 + x2) / 2)} {round((y1 + y2) / 2)}']
                elif mode in ('unsaved', 'save'):
                    click(10, item(base, kind='Checkbox', label='Fullscreen'))
                    if mode == 'save':
                        click(25, item(base, kind='Button', label='Save settings'))
                elif mode == 'wrapped-mouse':
                    w = item(known('wrapped-probe'), kind='WrappedProbe')
                    x1, y1, x2, y2 = w['rect']
                    x = round(x1 + 60 * scale)
                    y = round(y2 - 8 * scale)
                    events = [f'10 motion {x} {y}', '11 down 1', '12 up 1']
                elif mode == 'wrapped-keyboard':
                    events = ['8 key-down 43', '9 key-up 43', '14 key-down 44', '15 key-up 44']
                elif mode == 'language':
                    click(10, item(base, kind='Combo', label='Launcher language'))
                    events.extend(['20 key-down 82', '21 key-up 82', '25 key-down 40', '26 key-up 40'])
            # [XCDE] LINUX-039: shared input may be a link; the native launcher
            # requires the canonical selected path, just like its normal wrapper.
            native = ['--project-root', str(ROOT), '--input-root', str((ROOT.parent / 'game_files').resolve()), '--game-executable', str(args.game_executable.resolve()), '--state-root', str(state), '--shot', str(shot), '--shot-tab', str(tab), '--shot-frame', '90' if mode == 'tooltip' else '50']
            if events:
                script = state / 'test.events'
                script.write_text('\n'.join(events) + '\n')
                native.extend(['--ui-script', str(script)])
            av = [str(exe), *native] if args.production else [str(exe), str(width), str(height), str(scale), mode, *native]
            stdout = run(name, av, 30)
            after = digest(ini)
            if mode == 'save':
                assert after != before, 'actual Save did not write configuration'
                values = {k.strip(): v.strip() for k, v in (line.split('=', 1) for line in ini.read_text().splitlines() if '=' in line and (not line.startswith('#')))}
                assert values['fullscreen'] == '1' and values['menu_navigation_speed'] == 'fast' and (values['custom_theme_preservation'] == 'intact')
            else:
                assert after == before, 'opening/editing without Save rewrote INI'
            data = shot.read_bytes()
            assert data[:2] == b'BM'
            actual = struct.unpack_from('<ii', data, 18)
            assert actual == (width, height), (name, actual, (width, height))
            row = {'name': name, 'width': width, 'height': height, 'scale': scale, 'language': lang, 'tab': tab, 'mode': mode, 'ini_before_sha256': before, 'ini_after_sha256': after, 'ini_unchanged': after == before, 'bmp_sha256': digest(shot), 'capture': str(shot), 'production_binary': args.production}
            if not args.production:
                lines = [line for line in stdout.decode().splitlines() if line.startswith('THEME_TEST ')]
                assert len(lines) == 1
                metadata = json.loads(lines[0][11:])
                row['metadata'] = metadata
                assert metadata['fork_refusals'] == 0
                if not args.source and 'SDL_SetWindowMinimumSize' in (ROOT/'tools/launcher/launcher.cpp').read_text():
                    assert metadata['window_minimum_width']==round(640*scale) and metadata['window_minimum_height']==round(480*scale), 'actual SDL minimum-size contract differs'
                if mode in ('unsaved', 'save'):
                    assert metadata['fullscreen'], 'actual checkbox edit did not occur'
                if mode == 'language':
                    assert metadata['ui_lang'] == 0, 'actual launcher language selector did not change to Spanish'
                if mode in ('wrapped-mouse', 'wrapped-keyboard'):
                    assert metadata['wrapped_value'] and metadata['wrapped_changes'] == 1 and (metadata['wrapped_marked'] == 1), 'wrapped helper activation/edit semantics failed'
                footer = ['PLAY', 'Precompile shaders', 'Save settings', 'Create report', 'Quit'] if metadata['ui_lang'] == 1 else ['JUGAR', 'Precompilar shaders', 'Guardar ajustes', 'Crear reporte', 'Salir']
                items = {w['label']: w for w in metadata['widgets'] if w['type'] == 'Button'}
                row['footer_errors'] = []
                for label in footer:
                    w = items.get(label)
                    if not w:
                        row['footer_errors'].append(label + ' missing')
                        continue
                    x1, y1, x2, y2 = w['rect']
                    if not (x1 >= -0.01 and y1 >= -0.01 and (x2 <= width + 0.01) and (y2 <= height + 0.01) and w['visible']):
                        row['footer_errors'].append(label + ' clipped')
                assert not row['footer_errors'], row['footer_errors']
                if mode in ('bottom', 'bindings-bottom'):
                    tabs = [w for w in metadata['windows'] if '/tabs_' in w['name'] or w['name'].endswith('/tabs')]
                    row['scrolled_window_observed'] = any((w['scroll_y'] > 0 for w in tabs))
                    assert row['scrolled_window_observed']
                    if mode == 'bindings-bottom':
                        binds = [w for w in metadata['windows'] if '/binds_' in w['name']]
                        row['bindings_bottom_observed'] = any((w['scroll_max_y'] > 0 and abs(w['scroll_y'] - w['scroll_max_y']) < 1 for w in binds))
                        assert row['bindings_bottom_observed'], 'nested bindings did not reach last row'
                        row['bindings_right_observed'] = any((w['scroll_max_x'] > 0 and abs(w['scroll_x'] - w['scroll_max_x']) < 1 for w in binds))
                        assert row['bindings_right_observed'], 'nested bindings did not reach last column'
                if mode == 'popup':
                    row['popup_open'] = metadata['popup_count'] > 0
                    assert row['popup_open'], 'actual combo did not open'
                    w = item(metadata['widgets'], kind='Combo', id=target)
                    row['popup_id'] = w['expected_popup_id']
                    assert row['popup_id'] in metadata['popup_ids'], 'actual popup ID mismatch'
                if mode == 'tooltip':
                    row['tooltip_observed'] = any(('Tooltip' in w['name'] for w in metadata['windows']))
                    assert row['tooltip_observed'], 'disabled-feature tooltip not observed'
                if mode == 'focus':
                    assert metadata['nav_id'] and metadata['nav_visible'], 'keyboard navigation cursor not observed'
                (out / name / 'metadata.json').write_text(json.dumps(metadata, indent=2) + '\n')
            try:
                from PIL import Image
                png = shot.with_suffix('.png')
                Image.open(shot).save(png)
                row['png'] = str(png)
                row['png_sha256'] = digest(png)
            except ImportError:
                pass
            receipt['cases'].append(row)
            save()
            print(name, 'PASS render', actual, 'footer', row.get('footer_errors', []), flush=True)
            if mode == 'save':
                reopened = state / 'build/reopen.bmp'
                av = [str(exe), str(width), str(height), str(scale), 'normal', *native]
                av[av.index('--shot') + 1] = str(reopened)
                at = av.index('--ui-script')
                del av[at:at + 2]
                result = run(name + '-reopen', av, 30)
                assert digest(ini) == after
                meta = json.loads(next((line[11:] for line in result.decode().splitlines() if line.startswith('THEME_TEST '))))
                assert meta['fullscreen'] and meta['menu_navigation_speed'] == 1 and (meta['ui_lang'] == 1)
                row['reopen'] = {'ini_unchanged': True, 'metadata': meta, 'bmp_sha256': digest(reopened)}
                save()
        receipt['sources_after'] = {key: digest(HARNESS if key == 'harness' else Path(__file__) if key == 'runner' else ROOT / key) for key in receipt['sources_before']}
        assert receipt['sources_before'] == receipt['sources_after'], 'source changed during capture matrix'
        receipt['production_sources_after'] = {k: digest(ROOT / k) for k in snapshot}
        assert snapshot == receipt['production_sources_after'], 'production source/header changed during capture matrix'
        receipt['status'] = 'passed'
    except BaseException as e:
        receipt['status'] = 'failed'
        receipt['errors'].append(str(e))
        raise
    finally:
        receipt['finished_utc'] = now()
        save()
    return 0
if __name__ == '__main__':
    sys.exit(main())
