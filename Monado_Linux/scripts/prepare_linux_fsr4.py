#!/usr/bin/env python3
"""Prepare optional FSR4 Lite models locally from the pinned official AMD SDK.

The game remains native Vulkan. Proton runs only a headless synthetic compute
recorder in a fresh private prefix. No game data or human Steam state is used.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import pwd
import shutil
import signal
import subprocess
import sys
import time
import urllib.request

import setup_linux_fsr4 as component
import setup_linux_fsr4_tools as tools

ROOT = component.ROOT
BASE = ROOT / '.deps/fsr4'
PASSES = ('spd', 'prepass', 'pass0_post', 'pass1', 'pass1_post', 'pass1_postb',
          'pass2', 'pass2_post', 'pass3', 'pass3_post', 'postpass', 'rcas')
SETS = ('t1080_m0', 't1080_m1', 't2160_m0', 't2160_m1')

def configure_storage(path):
    """Host broker selects its owned install's persistent .deps/fsr4 store."""
    global BASE
    path=component.ordinary(path)
    if path.name!='fsr4' or path.parent.name!='.deps':
        raise ValueError('FSR4 storage must be the owned installation .deps/fsr4 directory')
    BASE=path;tools.BASE=path
    return BASE

def model_identity(lock):
    # Native-module/docs updates do not change an already prepared vendor model.
    return hashlib.sha256(json.dumps({'schema':1,'sdk':lock['sdk'],
        'capture_tools':tools.source_identity(lock),'sets':SETS,'passes':PASSES,
        'weights_bytes':27392},sort_keys=True).encode()).hexdigest()

def progress(stage,current,total,message):
    print('MONADO_FSR4_PROGRESS '+json.dumps({'stage':stage,'current':current,'total':total,'message':message}),flush=True)

def mounted_paths_below(work):
    found=set()
    for line in Path('/proc/self/mountinfo').read_text().splitlines():
        fields=line.split()
        if len(fields)<5:continue
        value=fields[4]
        for escaped,plain in (('\\040',' '),('\\011','\t'),('\\012','\n'),('\\134','\\')):value=value.replace(escaped,plain)
        mount=Path(value)
        if mount==work or mount.is_relative_to(work):found.add(mount)
    return found

def cleanup_work(work,out):
    owned=component.ordinary(work,BASE/'preparation')
    owner=json.loads((owned/'.monado-fsr4-owner.json').read_text())
    if owner.get('output')!=str(out):raise ValueError('preparation ownership changed')
    mounts=mounted_paths_below(owned);device=owned.stat().st_dev;warnings=[]
    def remove(path):
        try:
            if path.is_symlink():path.unlink();return
            if path.is_dir():
                if path in mounts or os.path.ismount(path) or path.stat().st_dev!=device:
                    warnings.append('retained mounted temporary path: '+str(path));return
                for child in path.iterdir():remove(child)
                try:path.rmdir()
                except OSError as error:warnings.append(str(error))
            else:path.unlink(missing_ok=True)
        except OSError as error:warnings.append(str(error))
    for child in owned.iterdir():
        if child.name not in ('logs','.monado-fsr4-owner.json'):remove(child)
    return warnings


def cache_sdk(offline=False):
    sdk = component.verify_source()['sdk']
    cache = component.ordinary(BASE / 'downloads', BASE)
    cache.mkdir(parents=True, exist_ok=True)
    path = component.ordinary(cache / ('amd-upscaler-' + sdk['commit'] + '.dll'), cache)
    if path.exists():
        if component.digest(path) != sdk['sha256']:
            raise ValueError('cached AMD SDK DLL hash mismatch')
        return path
    if offline:
        raise ValueError('offline AMD SDK DLL missing; prepare once with network access')
    request = urllib.request.Request(sdk['url'], headers={'User-Agent': 'Monado-II-local-FSR4-preparation'})
    with urllib.request.urlopen(request, timeout=90) as response:
        data = response.read(64_000_001)
    if len(data) > 64_000_000 or hashlib.sha256(data).hexdigest() != sdk['sha256']:
        raise ValueError('official AMD SDK DLL hash mismatch; nothing executed')
    temporary = path.with_name('.sdk-download.' + str(os.getpid()))
    with temporary.open('xb') as stream:
        stream.write(data)
    os.replace(temporary, path)
    return path


def private_proton(proton, work):
    """Copy only the driver script; installed Proton files remain read only.

    Upstream Proton's default entry point performs installation maintenance and
    may create a default prefix in Steam. Disable exactly those two maintenance
    calls and refuse a missing installed default prefix. Prefix/session setup is
    otherwise unchanged and all its destinations are private below work.
    """
    proton = Path(proton).resolve(strict=True)
    if proton.name != 'proton' or not (proton.parent / 'files/share/default_pfx').is_dir():
        raise ValueError('a complete installed Proton build with a default prefix is required')
    runner = work / 'proton-runner'
    runner.mkdir()
    source = proton.read_text()
    for call in ('    g_proton.cleanup_legacy_dist()', '    g_proton.do_steampipe_fixups()'):
        if source.count(call) != 1:
            raise ValueError('unrecognized Proton maintenance entry point; refusing execution')
        source = source.replace(call, '    # Monado private recorder: installed Proton is read only.')
    old = '    if g_proton.missing_default_prefix():\n        g_proton.make_default_prefix()'
    if source.count(old) != 1:
        raise ValueError('unrecognized Proton default-prefix entry point')
    source = source.replace(old, '    if g_proton.missing_default_prefix():\n        raise RuntimeError("installed Proton default prefix is missing")')
    path = runner / 'proton'
    path.write_text(source)
    path.chmod(0o755)
    (runner / 'files').symlink_to(proton.parent / 'files', target_is_directory=True)
    for name in ('filelock.py', 'version', 'steamclient.dll', 'steamclient64.dll'):
        original = proton.parent / name
        if original.is_file():
            shutil.copyfile(original, runner / name)
    # Wine's known user folders are private before prefix initialization.
    user = work / 'compat/pfx/drive_c/users/steamuser'
    for folder in ('Desktop', 'Documents', 'Downloads', 'Music', 'Pictures', 'Videos'):
        (user / folder).mkdir(parents=True, exist_ok=True)
    return path, {'installed_script': str(proton), 'installed_sha256': component.digest(proton),
                  'private_script_sha256': component.digest(path)}


def user_home():
    """Preserve an existing home, or look up the actual current Unix account.

    Hermetic supervisors may omit HOME. Never replace it with the preparation
    work directory, infer a Steam installation, or create an account directory.
    """
    home = os.environ.get('HOME')
    if home and Path(home).is_absolute() and Path(home).is_dir():
        return home
    try:
        home = pwd.getpwuid(os.getuid()).pw_dir
    except (KeyError, OSError) as error:
        raise ValueError('Cannot determine your home directory. Start FSR4 preparation from your desktop session with HOME set to your user home directory.') from error
    if not home or not Path(home).is_absolute() or not Path(home).is_dir():
        raise ValueError('Cannot determine your home directory. Start FSR4 preparation from your desktop session with HOME set to your user home directory.')
    return home


def environment(work, proton, display=None):
    env = {'PATH': '/usr/bin:/bin', 'LANG': 'C.UTF-8', 'LC_ALL': 'C.UTF-8',
           'HOME': user_home(), 'USER': os.environ.get('USER', ''),
           'TMPDIR': str(work / 'tmp'), 'XDG_CACHE_HOME': str(work / 'cache'),
           'XDG_CONFIG_HOME': str(work / 'config'), 'XDG_DATA_HOME': str(work / 'data'),
           'STEAM_COMPAT_DATA_PATH': str(work / 'compat'),
           'STEAM_COMPAT_CLIENT_INSTALL_PATH': str(work / 'private-steam-client'),
           'STEAM_COMPAT_INSTALL_PATH': str(work), 'STEAM_COMPAT_TOOL_PATHS': str(proton.parent),
           'STEAM_COMPAT_APP_ID': '0', 'SteamAppId': '0', 'SteamGameId': '0',
           'PROTON_LOG_DIR': str(work / 'logs'), 'PROTON_LOG': '0', 'WINEDEBUG': '-all',
           'PROTON_NO_ESYNC': '1', 'PROTON_NO_FSYNC': '1',
           'DXVK_STATE_CACHE_PATH': str(work / 'cache'), 'VKD3D_SHADER_CACHE_PATH': str(work / 'cache/vkd3d'),
           'MESA_SHADER_CACHE_DIR': str(work / 'cache/mesa'),
           '__GL_SHADER_DISK_CACHE_PATH': str(work / 'cache/nvidia'),
           'WINEDLLOVERRIDES': 'amdxc64=n,b;dxgi,d3d12,d3d12core=n',
           'VKD3D_DISABLE_EXTENSIONS': 'VK_KHR_cooperative_matrix,VK_NV_cooperative_matrix2'}
    if display:
        if len(display)>128 or any(ord(c)<32 for c in display):
            raise ValueError('invalid preparation display')
        env['DISPLAY']=display
        if os.environ.get('XAUTHORITY'):env['XAUTHORITY']=os.environ['XAUTHORITY']
    # Explicit process-local ICD selection is useful for multi-GPU preparation.
    for name in ('VK_DRIVER_FILES', 'VK_ICD_FILENAMES', 'DRI_PRIME'):
        if name in os.environ: env[name] = os.environ[name]
    for name in ('tmp', 'cache', 'config', 'data', 'logs', 'private-steam-client'):
        (work / name).mkdir(exist_ok=True)
    return env


def prepare(output, proton, offline=False, display=None):
    if not display:
        raise ValueError('FSR4 model preparation needs an X11/XWayland display for Proton DXGI initialization. Run it from your desktop session; no game window is opened.')
    lock = component.verify_source()
    tool_receipt = tools.verify_existing()
    sdk = cache_sdk(offline)
    out = component.ordinary(output, BASE / 'assets', fresh=True)
    out.mkdir(parents=True)
    work = component.ordinary(BASE / 'preparation' / out.name, BASE / 'preparation', fresh=True)
    work.mkdir(parents=True)
    (work/'.monado-fsr4-owner.json').write_text(json.dumps({'schema':1,'recipe_sha256':component.digest(__file__),'output':str(out)})+'\n')
    receipt = {'schema': 1, 'status': 'running', 'model': 'int8_lite', 'output': str(out),
               'recipe_sha256': component.digest(__file__), 'model_identity': model_identity(lock),
               'sdk': lock['sdk'], 'tools_receipt_sha256': component.digest(Path(tool_receipt['output']) / 'receipt.json'),
               'scope': 'headless synthetic SDK compute only; no game input', 'commands': []}
    def save():
        (out / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
    env = None
    def run(name, args, run_env, timeout=240):
        start = time.monotonic()
        log_path = work / 'logs' / (name + '.log')
        with log_path.open('wb') as log:
            process = subprocess.Popen(list(map(str, args)), cwd=work, env=run_env,
                stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
            try:
                code = process.wait(timeout=timeout)
            finally:
                if process.poll() is None:
                    os.killpg(process.pid, signal.SIGTERM)
                    try: process.wait(timeout=3)
                    except subprocess.TimeoutExpired:
                        os.killpg(process.pid, signal.SIGKILL); process.wait()
        receipt['commands'].append({'name': name, 'argv': list(map(str, args)), 'exit': code,
                                    'seconds': time.monotonic() - start, 'log': str(log_path)})
        save()
        if code:
            raise RuntimeError(name + ' failed; see ' + str(log_path))
    try:
        runner, receipt['proton'] = private_proton(proton, work)
        env = environment(work, Path(proton).resolve(), display)
        save()
        progress('capture',0,4,'Preparing local FSR4 Lite models with Proton')
        for name in ('fsr4cap.exe', 'amdxc64.dll'):
            shutil.copyfile(tool_receipt['tools'][name]['path'], work / name)
        cap = ROOT / 'third_party/fsr4cap'
        # Only the verified immutable SDK copy is read; output DLL is private.
        run('select-lite-model', [sys.executable, cap / 'driver_model.py', 'force', sdk,
            work / 'amd_fidelityfx_upscaler_dx12.dll', 'lite'], env)
        receipt['private_dll_sha256'] = component.digest(work / 'amd_fidelityfx_upscaler_dx12.dll')
        dimensions = [('1280x720', '1920x1080'), ('640x360', '1920x1080'),
                      ('2560x1440', '3840x2160'), ('1280x720', '3840x2160')]
        for i, (render, output_size) in enumerate(dimensions):
            run('capture-' + SETS[i], [sys.executable, runner, 'waitforexitandrun', work / 'fsr4cap.exe',
                '4.1.', render, output_size, '3'], env)
            message = (work / 'fsr4cap.log').read_text(errors='replace')
            shutil.copyfile(work / 'fsr4cap.log', work / 'logs' / (SETS[i] + '-capture.log'))
            if not message.rstrip().endswith('frames done'):
                raise RuntimeError('SDK Lite capture did not complete: ' + message[-1500:])
            progress('capture',i+1,4,'Captured model '+SETS[i])
        translate_env = dict(env, FSR4CAP_MODEL='lite', PATH=str(Path(tool_receipt['output']) / 'bin') + ':/usr/bin:/bin')
        translate_env['LD_LIBRARY_PATH'] = str(Path(tool_receipt['output']) / 'bin')
        progress('translate',0,0,'Translating and checking the captured model graphs')
        run('translate-models', [sys.executable, cap / 'extract.py', tool_receipt['tools']['dxil-spirv']['path'], work, out], translate_env, timeout=600)
        resources = {}
        checked=0
        for tier in SETS:
            directory = out / 'lite' / tier
            weights = directory / 'initializer.bin'
            if weights.is_symlink() or weights.stat().st_size != 27392:
                raise ValueError('wrong Lite model weight size: ' + tier)
            for name in (*[x + '.spv' for x in PASSES], 'initializer.bin'):
                path = component.ordinary(directory / name, out)
                if not path.is_file(): raise ValueError('missing prepared model: ' + str(path))
                resources[str(path.relative_to(out))] = component.resource(path)
                if name.endswith('.spv'):
                    run('validate-' + tier + '-' + name, [tool_receipt['tools']['spirv-val']['path'],
                        '--target-env', 'vulkan1.3', path], translate_env)
                    checked+=1;progress('validate',checked,48,'Checking prepared FSR4 shaders')
        receipt.update(status='passed', resources=resources)
        save()
    except BaseException as error:
        receipt.update(status='cancelled' if isinstance(error,(KeyboardInterrupt,InterruptedError)) else 'failed', error=str(error))
        raise
    finally:
        # Stop only this fresh private prefix's wineserver, including cancellation.
        if env:
            server = Path(proton).resolve().parent / 'files/bin/wineserver'
            try:
                subprocess.run([str(server), '-k'], env=dict(env, WINEPREFIX=str(work / 'compat/pfx')),
                               stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=10)
                subprocess.run([str(server), '-w'], env=dict(env, WINEPREFIX=str(work / 'compat/pfx')),
                               stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=10)
            except (OSError, subprocess.SubprocessError): pass
        # Large Wine prefixes and SDK copies are temporary. Only this freshly
        # created, marked preparation folder is eligible; symlinks are unlinked,
        # never traversed. Keep logs/receipt for a useful support report.
        try:
            warnings=cleanup_work(work,out)
            if warnings:receipt['cleanup_warnings']=warnings
        except (OSError,ValueError) as cleanup_error:
            receipt['cleanup_error']=str(cleanup_error)
        save()
    marker = component.ordinary(BASE / 'active-assets.json', BASE)
    temp = marker.with_name('.active-assets.' + str(os.getpid()))
    temp.write_text(json.dumps({'schema': 1, 'receipt_path': str(out / 'receipt.json'),
                               'receipt_sha256': component.digest(out / 'receipt.json')}) + '\n')
    os.replace(temp, marker)
    progress('complete',1,1,'FSR4 Lite models are ready. Your selected upscaler has not changed.')
    return verify_assets()


def verify_assets():
    lock=component.verify_source()
    marker = component.ordinary(BASE / 'active-assets.json', BASE)
    selection = json.loads(marker.read_text())
    path = component.ordinary(selection['receipt_path'], BASE / 'assets')
    if selection.get('schema') != 1 or component.digest(path) != selection['receipt_sha256']:
        raise ValueError('FSR4 asset receipt changed')
    receipt = json.loads(path.read_text())
    if (receipt.get('status') != 'passed' or receipt.get('model') != 'int8_lite' or
        receipt.get('model_identity') != model_identity(lock)):
        raise ValueError('FSR4 model preparation identity changed')
    required = {'lite/' + tier + '/' + name for tier in SETS for name in
                (*[x + '.spv' for x in PASSES], 'initializer.bin')}
    if set(receipt['resources']) != required:
        raise ValueError('incomplete FSR4 Lite asset manifest')
    for name, item in receipt['resources'].items():
        p = component.ordinary(path.parent / name, path.parent)
        if component.resource(p) != item:
            raise ValueError('FSR4 model file changed: ' + name)
    return {'asset_root': str(path.parent), 'receipt_path': str(path),
            'receipt_sha256': component.digest(path), 'resources': receipt['resources'], 'model': 'int8_lite'}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--prepare', action='store_true'); ap.add_argument('--output', type=Path)
    ap.add_argument('--proton', type=Path); ap.add_argument('--offline', action='store_true')
    ap.add_argument('--display', default=os.environ.get('DISPLAY'))
    ap.add_argument('--cache-sdk', action='store_true')
    ap.add_argument('--storage-root', type=Path)
    args = ap.parse_args()
    if args.storage_root:configure_storage(args.storage_root)
    if args.prepare:
        def cancelled(signum,frame): raise InterruptedError('FSR4 model preparation cancelled')
        signal.signal(signal.SIGTERM,cancelled)
    try:
        if args.cache_sdk: result = {'sdk_path': str(cache_sdk(args.offline))}
        elif args.prepare:
            if not args.proton or not args.output: raise ValueError('--prepare requires --proton and fresh --output')
            result = prepare(args.output, args.proton, args.offline, args.display)
        else: result = verify_assets()
        print(json.dumps(result, indent=2))
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        ap.exit(1, str(error) + '\n')


if __name__ == '__main__':
    main()
