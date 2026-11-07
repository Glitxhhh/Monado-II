"""Story-boss tour (tests/tours/battle_story.txt): one fight per enemy species that only appears in story-gated spawns
(S_FLG_MAX below the table's maximum: bosses, story fights). Each job jumps to the map, sets the story progress to the
spawn's S_FLG_MIN (party.c "story" step) and jumps again onto the spawn so the map's spawns are evaluated at that point
of the story, then fights (a finished save: XCDE_BATCH_SAVE).
  python tools/boss_tour_gen.py
"""
import os, re, sys
sys.argv = ['x']
exec(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'battle_tour_gen.py'), encoding='utf-8').read().split('tot=0')[0])
covered, out, n = set(), [], 0
story_rows = []
for fn in sorted(os.listdir(BD)):
    m = re.match(r'bdat_(ma\d{4})\.bdat$', fn)
    if not m:
        continue
    name = m.group(1)
    r = rows(fn, 'poplist' + name[2:])
    if not r:
        continue
    el = {rid: x for rid, x in rows(fn, 'BTL_enelist' + name[2:])}
    smax = max(x['S_FLG_MAX'] for _, x in r)
    for rid, x in r:
        keys = set()
        for k in range(1, 6):
            e = el.get(x['ene%dID' % k])
            if e:
                keys.add((e['family'], e['atk1'], e['arts1'], e['arts2'], e['arts3'], e['named']))
        if x['S_FLG_MAX'] >= smax:
            covered |= keys
        elif (x['posX'], x['posY'], x['posZ']) != (0, 0, 0):
            story_rows.append((name, rid, x, keys))
for name, rid, x, keys in story_rows:
    new = keys - covered
    if not new:
        continue
    covered |= keys
    n += 1
    a, s = int(name[2:4]), int(name[4:6])
    pos = '%g %g %g' % (x['posX'], x['posY'] + 2, x['posZ'])
    out += ['# %s enemies %d (story %d-%d)' % (name, rid, x['S_FLG_MIN'], x['S_FLG_MAX']), 'warp %d %d %s 0' % (a, s, pos),
            'story %d' % x['S_FLG_MIN'], 'warp %d %d %s 0' % (a, s, pos), 'battle 2400']
open(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'tests', 'tours', 'battle_story.txt'), 'w').write('\n'.join(out) + '\n')
print('%d story fights' % n)
