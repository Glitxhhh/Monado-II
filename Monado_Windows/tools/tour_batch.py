"""Run a debugger tour (tests/tours/*.txt, runtime/hle/party.c XCDE_TOUR) as many short game runs, in parallel.

  python tools/tour_batch.py <tour.txt> <work dir> [parallel=3] [--by-map] [--only=<substr>] [--exe=<path>]

One job per cutscene (a "# ..." label and the steps under it), or with --by-map one job per map (all its landmarks):
each job boots the game, loads the save, follows its steps and exits (XCDE_TOUR_EXIT). A scene that holds the game
(a text box, an event that never ends) only costs its own job. Real time at 60 fps with audio on at volume 0 (the
cutscene clock follows the audio: without audio a shot can step back a frame), the in-game flicker scanner on.

Each parallel slot has its own folder (<work dir>/slotN: targets, data and cache/spirv linked to the repo's) and keeps
its cache/variants.bin from job to job: every pipeline the tours drew is in the slots' variants.bin files at the end
(tools/merge_variants.py merges them). Logs: <work dir>/logs/<job>.log; scanner hits: <work dir>/hits/<job>/.
A job whose log says "[tour] done" is skipped when the batch runs again (resume after an interruption).
"""
import os
import re
import shutil
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SAVE = os.environ.get('XCDE_BATCH_SAVE', 'saves/colony9')  # (read-only: copied into each slot)


def jobs_of(tour, by_map):
    jobs, cur, label = [], [], None
    for line in open(tour):
        line = line.rstrip('\n')
        if not line.strip():
            continue
        if line.startswith('#'):
            key = re.match(r'# (ma\d{4})', line)
            new = not by_map or not label or not key or key.group(1) != label
            if new and cur:
                jobs.append(cur)
                cur = []
            if key:
                label = key.group(1)
        cur.append(line)
    if cur:
        jobs.append(cur)
    out = []
    for j in jobs:
        m = re.match(r'# (ma\d{4})(?: event (\w+)| (?:landmark|enemies|collectible|location|gimmick|locked) (\d+))?', j[0])
        kind = re.match(r'# ma\d{4} (enemies|collectible|location|gimmick|locked) ', j[0])
        tail = (kind.group(1)[0] if kind else '') + (m.group(3) or '') if m else ''
        name = (m.group(1) + ('_' + m.group(2) if m.group(2) else '' if by_map else '_' + tail)) if m else 'job%d' % len(out)
        out.append((name, j))
    return out


def link(dst, src):
    if not os.path.exists(dst):
        subprocess.run(['cmd', '/c', 'mklink', '/J', dst.replace('/', '\\'), src.replace('/', '\\')], capture_output=True)


def slot_dir(work, k):
    d = os.path.join(work, 'slot%d' % k)
    os.makedirs(os.path.join(d, 'cache'), exist_ok=True)
    link(os.path.join(d, 'targets'), os.path.join(REPO, 'targets'))
    link(os.path.join(d, 'data'), os.path.join(REPO, 'data'))
    link(os.path.join(d, 'cache', 'spirv'), os.path.join(REPO, 'cache', 'spirv'))
    return d


def start(work, k, name, steps, exe):
    d = slot_dir(work, k)
    tour = os.path.join(d, 'tour.txt')
    open(tour, 'w').write('\n'.join(steps) + '\n')
    save = os.path.join(d, 'save')
    shutil.rmtree(save, ignore_errors=True)
    shutil.copytree(SAVE, save)
    hits = os.path.join(work, 'hits', name)
    shutil.rmtree(hits, ignore_errors=True)
    os.makedirs(hits)
    log = open(os.path.join(work, 'logs', name + '.log'), 'w')
    n_look = sum(1 for s in steps if s.startswith('look'))
    n_event = sum(1 for s in steps if s.startswith('event'))
    ev_s = 0  # (each scene's own length when the tour gives it: "event <name> <seconds>", tools/tour_gen.py)
    for st in steps:
        f = st.split()
        if f[0] == 'battle':  # (its frames, a jump next to an enemy)
            ev_s += int(f[1]) / 60 + 30
        if f[0] in ('event', 'theater', 'await'):
            ev_s += float(f[2]) * 1.2 + 60 if len(f) > 2 and float(f[2]) > 0 else int(os.environ.get("XCDE_BATCH_EVENT_S", "420"))
    timeout = 180 + 20 * n_look + ev_s  # (boot + load ~50 s, a scene up to 60 s to start; a landmark; the scenes)
    env = dict(os.environ, XCDE_TOUR=tour, XCDE_TOUR_EXIT='1', LARECOMP_AUDIO_GAIN='0', LARECOMP_HIDDEN='1',
               XCDE_INI=os.environ.get('XCDE_INI', os.path.join(REPO, 'build', 'fps60.ini')), XCDE_INPUT_REPLAY=os.path.join(REPO, 'tests', 'routes', 'load_a.txt'),
               LARECOMP_SAVE_DIR=save, XCDE_REC_SIZE=os.environ.get('XCDE_REC_SIZE', '1280x720'), XCDE_GLITCH_WATCH='1')
    if not os.environ.get('XCDE_BATCH_NO_SCAN'):  # (XCDE_BATCH_NO_SCAN=1: the recorder as XCDE_REC / XCDE_REC_OUT set it)
        env['XCDE_REC_SCAN'] = hits.replace('\\', '/')
    env['PATH'] = 'C:\\msys64\\ucrt64\\bin;' + env.get('PATH', '')
    p = subprocess.Popen([exe, 'targets'], cwd=d, env=env, stdout=log, stderr=subprocess.STDOUT)
    return p, time.time() + timeout, log


def main():
    tour, work = sys.argv[1], sys.argv[2]
    par = int(sys.argv[3]) if len(sys.argv) > 3 and sys.argv[3].isdigit() else 3
    by_map = '--by-map' in sys.argv
    only = next((a[7:] for a in sys.argv if a.startswith('--only=')), '')
    exe = next((a[6:] for a in sys.argv if a.startswith('--exe=')), os.path.join(REPO, 'build', 'xcde.exe'))
    os.makedirs(os.path.join(work, 'logs'), exist_ok=True)
    todo = []
    for name, steps in jobs_of(tour, by_map):
        if only and only not in name:
            continue
        lp = os.path.join(work, 'logs', name + '.log')
        if os.path.exists(lp) and '[tour] done' in open(lp, errors='replace').read():
            continue
        todo.append((name, steps))
    print('%d jobs to run, %d at a time' % (len(todo), par), flush=True)
    running = {}
    t0 = time.time()
    done = 0
    while todo or running:
        for k in range(par):
            if k not in running and todo:
                name, steps = todo.pop(0)
                running[k] = (name,) + start(work, k, name, steps, exe)
        time.sleep(2)
        for k, (name, p, deadline, log) in list(running.items()):
            if p.poll() is None and time.time() < deadline:
                continue
            if p.poll() is None:
                p.kill()
                p.wait()
            log.close()
            done += 1
            txt = open(os.path.join(work, 'logs', name + '.log'), errors='replace').read()
            state = 'done' if '[tour] done' in txt else 'timeout' if p.returncode in (1, -9, 9) or time.time() >= deadline else 'exit %s' % p.returncode
            hits = len(os.listdir(os.path.join(work, 'hits', name)))
            print('[%5.0f min] %d/%d %s: %s, %d saved hits%s' % ((time.time() - t0) / 60, done, done + len(todo) + len(running) - 1, name, state, hits,
                  ', never started' if 'never started' in txt else ''), flush=True)
            del running[k]


main()
