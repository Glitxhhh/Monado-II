#!/usr/bin/env python3
"""Authored native run records; never opens a game/input/human profile."""
import hashlib,json,os
from pathlib import Path
import subprocess,sys,time,uuid
PORT=Path(__file__).resolve().parents[1]
def main():
    if os.name!='nt':raise SystemExit('Native Windows required')
    root=PORT/'runs'/('run-diagnostics-'+str(time.time_ns())+'-'+uuid.uuid4().hex[:8]);root.mkdir()
    sources=[PORT/'runtime/host/run_diagnostics_windows.c',PORT/'runtime/host/settings_ini.c',PORT/'tests/native/test_run_diagnostics_windows.c']
    env=dict(os.environ,ZIG_GLOBAL_CACHE_DIR=str(PORT/'.deps/zig-global-cache'),ZIG_LOCAL_CACHE_DIR=str(root/'zig-cache'))
    exe=root/'authored-run-diagnostics.exe'
    command=[sys.executable,'-m','ziglang','cc','-O1','-UNDEBUG','-march=x86_64_v2','-ffp-contract=off','-fno-fast-math','-DMONADO_RUN_DIAGNOSTICS_TEST=1','-municode',*map(str,sources),'-lbcrypt','-ladvapi32','-o',str(exe)]
    result=subprocess.run(command,env=env,capture_output=True,timeout=180);(root/'compile.out').write_bytes(result.stdout);(root/'compile.err').write_bytes(result.stderr)
    if result.returncode:raise RuntimeError(result.stderr.decode(errors='replace'))
    cases=[]
    for name in ('precompile','normal','bounded','retention','modified','locked','hardlink','unknown','reparse'):
        profile=root/(name+' authored 測試');profile.mkdir()
        if name=='reparse':
            import _winapi
            target=root/'authored-junction-target';target.mkdir();(target/'sentinel').write_bytes(b'preserve')
            _winapi.CreateJunction(str(target),str(profile/'logs'))
        result=subprocess.run([str(exe),str(profile),name],capture_output=True,timeout=30,env=env)
        (root/(name+'.out')).write_bytes(result.stdout);(root/(name+'.err')).write_bytes(result.stderr)
        if result.returncode:raise RuntimeError(name+' failed '+str(result.returncode)+': '+result.stderr.decode(errors='replace'))
        records=list((profile/'logs/game-runs').glob('run-*.jsonl'))
        if name=='precompile':assert not (profile/'logs').exists()
        if name=='normal':
            raw=records[0].read_bytes();assert b'TOP_SECRET' not in raw and b'bind_key' not in raw and b'private_key' not in raw
            rows=[json.loads(r) for r in raw.splitlines()]
            assert sum(r.get('event')=='measured-output' for r in rows)==2
            assert any(r.get('event')=='settings-loaded' and r['configured_fps']==120 and r['fps_overlay']==0 for r in rows)
            assert any(r.get('event')=='applied-limiter' and r['interval']==0 and r['effective_interval']==2 and r['nominal_fps']==30 and r['measured'] is False for r in rows)
            assert all(r.get('generated')=='unavailable' for r in rows if r.get('event')=='measured-output')
        if name=='bounded':assert records[0].stat().st_size<=32768
        if name=='reparse':assert (target/'sentinel').read_bytes()==b'preserve'
        cases.append({'scenario':name,'status':'pass','records':len(records)})
    (root/'receipt.json').write_text(json.dumps({'scope':'authored native components; no gameplay/profile/input','command':command,'sources':{str(p.relative_to(PORT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in sources},'cases':cases},indent=2)+'\n')
    print('Native run diagnostics9 PASS: '+str(root))
if __name__=='__main__':main()
