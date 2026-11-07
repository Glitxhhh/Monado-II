#!/usr/bin/env python3
"""[XCDE] LINUX-018: actual native controller with authored fake supervisors.

No SDL initialization, window, game input, gameplay, real precompile or profile.
Each bounded case gets an independent fake project and guarded private state.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import resource
import sys

from boot_smoke_linux import bounded_process

ROOT = Path(__file__).resolve().parent.parent
FIXTURE = 'tests/native/test_launcher_precompile_linux.cpp'
CASES = ('complete', 'partial', 'partial-errors', 'fatal', 'missing', 'stale-session', 'wrong-pid',
         'oversize', 'duplicate', 'truncated-counter', 'bad-header', 'negative-counter',
         'unknown-state', 'bad-build', 'done-over-total', 'unfinished', 'forced-complete',
         'no-final-record', 'linked-file', 'symlink-file', 'cancel', 'cancel-forced',
         'game-busy', 'parent-death')

FAKE_SUPERVISOR = r'''# Authored protocol fixture, never a game/compiler.
import argparse, os, pathlib, signal, sys, time
p=argparse.ArgumentParser(); p.add_argument('--executable'); p.add_argument('--state-root'); p.add_argument('--session'); a=p.parse_args()
state=pathlib.Path(a.state_root); mode=(pathlib.Path(__file__).parent.parent/'mode').read_text()
fields={'session':a.session,'build':'b'*64,'identity':'c'*64,'supervisor_pid':os.getpid(),
        'state':'complete','phase':'done','forced':0,'files_total':4,'files_done':4,
        'shaders':5,'programs':3,'programs_done':3,'seed_total':2,'seed_done':2,
        'variants_total':1,'variants_done':1,'attempted':6,'built':6,'failed':0,'skipped':0,
        'unsupported':2,'errors':0,'gpu_vendor':4318,'gpu_device':10115,'gpu_driver':1,
        'gpu_cache_uuid':'d'*32,'message':'authored fake supervisor'}
target=state/'cache/precompile_status.txt'
def write(text=None):
    content=text if text is not None else 'monado_precompile_status 1\n'+''.join(str(k)+' '+str(v)+'\n' for k,v in fields.items())
    temp=target.with_suffix('.tmp'); temp.write_text(content); temp.replace(target)
def cancelled(number,frame):
    fields.update(state='cancelled',phase='done',forced=int(mode=='cancel-forced'))
    write(); (state/'parent-cancelled').write_text('SIGTERM received\n'); raise SystemExit(130)
if mode in ('cancel','cancel-forced','parent-death'):
    signal.signal(signal.SIGTERM,cancelled); fields.update(state='running',phase='scan',files_done=1,programs_done=0,seed_done=0,variants_done=0)
    write(); (state/'fake-ready').write_text('ready\n')
    end=time.monotonic()+5
    while time.monotonic()<end: time.sleep(.005)
    sys.exit(9)
if mode=='missing': sys.exit(0)
if mode=='partial': fields.update(state='partial',built=4,failed=1,attempted=5,unsupported=3); write(); sys.exit(2)
if mode=='partial-errors': fields.update(state='partial',built=4,failed=0,attempted=4,unsupported=3,errors=2); write(); sys.exit(2)
if mode=='fatal': fields.update(state='failed',errors=1); write(); sys.exit(7)
if mode=='stale-session': fields['session']='f'*32
if mode=='wrong-pid': fields['supervisor_pid']=os.getpid()+1
if mode=='truncated-counter': fields['built']='6x'
if mode=='negative-counter': fields['built']='-1'
if mode=='unknown-state': fields['state']='success'
if mode=='bad-build': fields['build']='not-a-hash'
if mode=='done-over-total': fields['programs_done']=4
if mode=='unfinished': fields['seed_done']=1
if mode=='forced-complete': fields['forced']=1
write()
if mode=='oversize': target.write_text('x'*8193)
if mode=='duplicate': target.write_text(target.read_text()+'built 6\n')
if mode=='bad-header': target.write_text(target.read_text().replace('status 1','status 2',1))
if mode=='linked-file':
    other=state/'cache/original-status'; target.rename(other); os.link(other,target)
if mode=='symlink-file':
    other=state/'cache/original-status'; target.rename(other); target.symlink_to(other)
if mode=='no-final-record':
    end=time.monotonic()+4
    while not (state/'release').exists():
        if time.monotonic()>end: sys.exit(9)
        time.sleep(.001)
    target.unlink()
sys.exit(0)
'''


def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def sources():
    pending = [ROOT / name for name in (FIXTURE, 'scripts/launcher_precompile_tests_linux.py',
        'scripts/boot_smoke_linux.py', 'scripts/platform_tests_linux.py', 'scripts/run_linux.py',
        'runtime/platform/linux_state_guard.c')]
    found = {}
    while pending:
        path = pending.pop().resolve(); name = str(path.relative_to(ROOT))
        if name in found:
            continue
        found[name] = digest(path)
        if path.suffix in ('.c', '.h', '.cpp', '.hpp'):
            for include in re.findall(r'^\s*#\s*include\s+"([^"]+)"', path.read_text(), re.M):
                target = (path.parent / include).resolve()
                if not target.is_file():
                    raise RuntimeError('missing local include: ' + str(target))
                pending.append(target)
    return dict(sorted(found.items()))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    out = args.output.absolute()
    if out != out.resolve() or not out.is_relative_to(ROOT / 'runs') or out.exists():
        parser.error('--output must be a fresh ordinary directory under Linux runs/')
    os.umask(0o077); resource.setrlimit(resource.RLIMIT_CORE, (0, 0)); out.mkdir(parents=True)
    sysroot = Path((ROOT / '.deps/graphics/active-sysroot.txt').read_text().strip())
    cc, cxx = ROOT / '.deps/bin/clang-cc', ROOT / '.deps/bin/clang-cxx'
    env = {'PATH': '/usr/bin:/bin', 'LANG': 'C.UTF-8', 'LC_ALL': 'C.UTF-8',
           'TMPDIR': str(out), 'PYTHONDONTWRITEBYTECODE': '1'}
    receipt = {'scope': __doc__, 'status': 'running', 'argv': sys.argv, 'cwd': str(ROOT),
               'host': list(platform.uname()), 'started_utc': datetime.now(timezone.utc).isoformat(),
               'sources_before': sources(), 'dependencies': {str(path): digest(path) for path in (cc, cxx)},
               'sysroot': str(sysroot), 'commands': [], 'artifacts': {}, 'cases': list(CASES)}
    for name in receipt['sources_before']:
        target = out / 'source-snapshot' / name; target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes((ROOT / name).read_bytes())

    def save():
        (out / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')

    def run(name, argv, artifact=None):
        argv = list(map(str, argv)); row = {'name': name, 'argv': argv, 'cwd': str(ROOT), 'environment': env, 'seconds_limit': 10}
        receipt['commands'].append(row); save(); completion = {}; failure = None
        try:
            bounded_process(argv, cwd=ROOT, env=env, seconds=10, completion=completion)
        except BaseException as error:
            failure = error
        row.update(completion.get('record', {}))
        for stream in ('stdout', 'stderr'):
            path = out / (name + '.' + stream + '.log'); path.write_bytes(completion.get(stream, b'')); row[stream + '_sha256'] = digest(path)
        row['passed'] = not failure and row.get('exit') == 0 and not row.get('timed_out') and not row.get('killed_owned_pids') and not row.get('remaining_owned_pids') and not row.get('cleanup_errors')
        if artifact and row['passed']:
            receipt['artifacts'][str(artifact.relative_to(out))] = digest(artifact)
        if failure:
            row['error'] = str(failure)
        save(); print(name, 'PASS' if row['passed'] else 'FAIL', flush=True)
        if isinstance(failure, (KeyboardInterrupt, SystemExit)):
            raise failure
        if not row['passed']:
            raise RuntimeError(name + ': inspect ' + str(out / (name + '.stderr.log')))

    save(); error = None; interrupted = None
    try:
        run('compiler-version', [cxx, '--version'])
        flags = ['-O1', '-UNDEBUG', '-march=x86_64_v2', '-ffp-contract=off', '-fno-strict-aliasing',
                 '-ffunction-sections', '-fdata-sections', '-pthread', '-Wall', '-Wextra', '-I' + str(sysroot / 'usr/include')]
        guard = out / 'linux_state_guard.o'; binary = out / 'test_launcher_precompile_linux'
        run('compile-guard', [cc, *flags, '-c', ROOT / 'runtime/platform/linux_state_guard.c', '-o', guard], guard)
        run('compile-controller', [cxx, *flags, '-std=c++17', ROOT / FIXTURE, guard, '-Wl,--gc-sections', '-o', binary], binary)
        for case in CASES:
            work = out / case; project = work / 'project'; state = work / 'state'
            (project / 'scripts').mkdir(parents=True); (state / 'logs').mkdir(parents=True); (state / 'cache').mkdir()
            fake = project / 'scripts/precompile_linux.py'; fake.write_text(FAKE_SUPERVISOR)
            (project / 'scripts/run_linux.py').write_text('import pathlib,sys\np=pathlib.Path(sys.argv[sys.argv.index("--state-root")+1]);(p/"PLAY_WAS_LAUNCHED").write_text("BUG")\nraise SystemExit(99)\n')
            (project / 'mode').write_text(case)
            run(case, [binary, case, project, state])
            if (state / 'PLAY_WAS_LAUNCHED').exists():
                raise RuntimeError('unexpected authored Play sentinel in ' + case)
            receipt.setdefault('fake_supervisors', {})[str(fake.relative_to(out))] = digest(fake)
    except BaseException as failure:
        error = str(failure) or type(failure).__name__
        if isinstance(failure, (KeyboardInterrupt, SystemExit)):
            interrupted = failure
    finally:
        receipt['sources_after'] = sources()
        receipt['dependencies_after'] = {path: digest(path) for path in receipt['dependencies']}
        if receipt['sources_before'] != receipt['sources_after'] or receipt['dependencies'] != receipt['dependencies_after']:
            error = (error + '; ' if error else '') + 'source/dependency changed during test'
        receipt.update(status='failed' if error else 'passed', error=error, finished_utc=datetime.now(timezone.utc).isoformat())
        save()
    print(receipt['status'], out / 'receipt.json')
    if interrupted:
        raise interrupted
    return 1 if error else 0


if __name__ == '__main__':
    sys.exit(main())
