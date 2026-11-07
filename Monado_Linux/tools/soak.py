"""2-3 h soak: one game process running a long tour (battles across maps), memory sampled every 30 s.
  python soak.py <exe> <work dir> [every=2]
"""
import os, re, shutil, subprocess, sys, time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SAVE = os.environ.get('SOAK_SAVE', 'saves/dl_100')  # (a folder holding save_0/: read only)
exe, work = os.path.abspath(sys.argv[1]), sys.argv[2]  # (absolute: the game runs with cwd = work)
every = int(sys.argv[3]) if len(sys.argv) > 3 else 2

# tour: every <every>-th battle job of battle_types.txt (not the out-of-map warp ma0601 e15)
jobs, cur = [], []
for line in open(os.path.join(REPO, 'tests', 'tours', 'battle_types.txt')):
    line = line.rstrip('\n')
    if line.startswith('#'):
        if cur: jobs.append(cur)
        cur = [line]
    elif line.strip() and cur:
        cur.append(line)
if cur: jobs.append(cur)
jobs = [j for j in jobs if not j[0].startswith('# ma0601 enemies 15')]
pick = jobs[::every]
os.makedirs(work, exist_ok=True)
tour = os.path.join(work, 'tour.txt')
open(tour, 'w').write('\n'.join(l for j in pick for l in j) + '\n')

def link(dst, src):
    if not os.path.exists(dst):
        subprocess.run(['cmd', '/c', 'mklink', '/J', dst.replace('/', '\\'), src.replace('/', '\\')], capture_output=True)

os.makedirs(os.path.join(work, 'cache'), exist_ok=True)
link(os.path.join(work, 'targets'), os.path.join(REPO, 'targets'))
link(os.path.join(work, 'data'), os.path.join(REPO, 'data'))
link(os.path.join(work, 'cache', 'spirv'), os.path.join(REPO, 'cache', 'spirv'))
save = os.path.join(work, 'save')
shutil.rmtree(save, ignore_errors=True)
shutil.copytree(SAVE, save)
env = dict(os.environ, XCDE_TOUR=tour, XCDE_TOUR_EXIT='1', LARECOMP_AUDIO_GAIN='0', LARECOMP_HIDDEN='1',
           XCDE_INI=os.path.join(REPO, 'build', 'fps60.ini'), XCDE_INPUT_REPLAY=os.path.join(REPO, 'tests', 'routes', 'load_a.txt'),
           LARECOMP_SAVE_DIR=save, XCDE_LOAD_HOLD_MS='0', XCDE_REG_HOLD='0')
env['PATH'] = 'C:\\msys64\\ucrt64\\bin;' + env.get('PATH', '')
log = open(os.path.join(work, 'game.log'), 'w')
mem = open(os.path.join(work, 'mem.tsv'), 'w')
mem.write('sec\tprivate_mb\tworking_mb\thandles\tthreads\n')
p = subprocess.Popen([exe, 'targets'], cwd=work, env=env, stdout=log, stderr=subprocess.STDOUT)
print('soak: %d battles, pid %d' % (len(pick), p.pid), flush=True)
t0 = time.time()
while p.poll() is None and time.time() - t0 < 4 * 3600:
    time.sleep(30)
    q = subprocess.run(['powershell', '-NoProfile', '-Command',
                        '$x=Get-Process -Id %d -ErrorAction SilentlyContinue; if($x){"{0} {1} {2} {3}" -f [int]($x.PrivateMemorySize64/1MB),[int]($x.WorkingSet64/1MB),$x.HandleCount,$x.Threads.Count}' % p.pid],
                       capture_output=True, text=True).stdout.split()
    if len(q) == 4:
        mem.write('%d\t%s\t%s\t%s\t%s\n' % (time.time() - t0, *q))
        mem.flush()
if p.poll() is None:
    p.kill()
print('soak ended after %.0f min, exit %s' % ((time.time() - t0) / 60, p.returncode), flush=True)
