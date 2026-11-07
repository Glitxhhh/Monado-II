"""Tour files for the debugger (runtime/hle/party.c XCDE_TOUR) from the game's own tables.

  python tools/tour_gen.py maps <out.txt> [look frames=240] [map filter, e.g. ma0101]
      every landmark of every map (landmarklist: arrival position jump_pos*, jump_rotY), a camera turn at each
  python tools/tour_gen.py full <out.txt> [cell=150] [look frames=240] [map filter]
      every map with its own table file (bdat_maXXXX): the landmarks plus every enemy spawn (poplist), collectible
      (Litemlist), gimmick (Gimlist) and locked area (FieldLock) position, one point per cell x cell square of the
      ground (the first position met in it) - zones and their corners without landmarks too
  python tools/tour_gen.py sev <out.txt> [map filter]
      the same for every side event in EVT_sev (its script name played as the event)
  python tools/tour_gen.py events <out.txt> [map filter]
      every cutscene in EVT_cs, played on its map: the party first jumps to the map's first landmark (a safe spot;
      EVT_cs evth_pos can be out of bounds - the party falls and an Incapacitation tutorial holds the tour)
"""
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
BDAT = os.path.join(HERE, '..', 'targets', 'romfs', 'bdat')


def rows(file, table):
    return rows_file(file, table)


def rows_file(file, table):
    out = subprocess.run([sys.executable, os.path.join(HERE, 'bdat1.py'), os.path.join(BDAT, file), table, '100000'],
                         capture_output=True, encoding='utf-8', env=dict(os.environ, PYTHONIOENCODING='utf-8')).stdout
    res, cur = [], False
    import ast
    for l in out.splitlines():
        if l.startswith('== '):
            cur = l[3:].split(' (')[0] == table
            continue
        if cur and l.strip():
            i = l.index(' ')
            res.append((int(l[:i]), ast.literal_eval(l[i + 1:])))
    return res


def area_sub(name):  # 'ma0201' -> (2, 1)
    return int(name[2:4]), int(name[4:6])


def main():
    mode, out = sys.argv[1], sys.argv[2]
    maps = {rid: r['id_name'] for rid, r in rows('bdat_common.bdat', 'FLD_maplist')}
    lms = rows('bdat_common.bdat', 'landmarklist')
    first_lm = {}
    for rid, r in lms:
        first_lm.setdefault(r['mapID'], r)
    lines = []
    if mode == 'full':
        cell = float(sys.argv[3]) if len(sys.argv) > 3 else 150
        look = int(sys.argv[4]) if len(sys.argv) > 4 else 240
        filt = sys.argv[5] if len(sys.argv) > 5 else ''
        inv = {v: k for k, v in maps.items()}
        total = 0
        for fn in sorted(os.listdir(BDAT)):
            m = re.match(r'bdat_(ma[0-9]{4})[.]bdat$', fn)
            if not m or filt not in m.group(1):
                continue
            name = m.group(1)
            a, sub = area_sub(name)
            pts = []  # (x, y, z, label)
            for rid, r in lms:
                if inv.get(name) == r['mapID'] and (r['jump_posX'], r['jump_posY'], r['jump_posZ']) != (0, 0, 0):
                    pts.append((r['jump_posX'], r['jump_posY'], r['jump_posZ'], 'landmark %d' % rid))
            for rid, r in lms:
                if inv.get(name) == r['mapID'] and (r['posX'], r['posY'], r['posZ']) != (0, 0, 0):
                    pts.append((r['posX'], r['posY'] + 2, r['posZ'], 'location %d' % rid))
            # (Gimlist and FieldLock store positions in hundredths of the others' units: -65034 = -650.34, next to
            # Colony 9's landmarks; taken as they were, the tour sent the party far outside every map and ma2501 crashed)
            for table, what, scale in (('poplist', 'enemies', 1), ('Litemlist', 'collectible', 1), ('Gimlist', 'gimmick', 0.01),
                                       ('FieldLock', 'locked area', 0.01)):
                for rid, r in rows_file(fn, table + name[2:]):
                    if 'posX' in r and (r['posX'], r['posY'], r['posZ']) != (0, 0, 0):
                        pts.append((r['posX'] * scale, r['posY'] * scale + 2, r['posZ'] * scale, '%s %d' % (what, rid)))
            seen, kept = set(), []
            for x, y, z, label in pts:
                key = (int(x // cell), int(z // cell))
                if key in seen:
                    continue
                seen.add(key)
                kept.append((x, y, z, label))
            for x, y, z, label in kept:
                lines += ['# %s %s' % (name, label), 'warp %d %d %g %g %g 0' % (a, sub, x, y, z), 'look %d' % look]
            total += len(kept)
            print('%s: %d positions -> %d points' % (name, len(pts), len(kept)))
        print('%d points' % total)
    elif mode == 'maps':
        look = int(sys.argv[3]) if len(sys.argv) > 3 else 240
        filt = sys.argv[4] if len(sys.argv) > 4 else ''
        for rid, r in lms:
            name = maps.get(r['mapID'])
            if not name or filt not in name or (r['jump_posX'], r['jump_posY'], r['jump_posZ']) == (0, 0, 0):
                continue
            a, s = area_sub(name)
            lines += ['# %s landmark %d' % (name, rid),
                      'warp %d %d %g %g %g %g' % (a, s, r['jump_posX'], r['jump_posY'], r['jump_posZ'], r['jump_rotY']),
                      'look %d' % look]
    else:
        filt = sys.argv[3] if len(sys.argv) > 3 else ''
        inv = {v: k for k, v in maps.items()}
        table = 'EVT_sev' if mode == 'sev' else 'EVT_cs'  # (EVT_sev: the side events, a script name instead of an event)
        for rid, r in rows('bdat_evt.bdat', table):
            if 'script' in r:
                r = dict(r, event=r['script'])
            m = r['map']
            if not m.startswith('ma') or filt not in m:
                continue
            x, y, z = r['evth_pos_x'], r['evth_pos_y'], r['evth_pos_z']
            # a scene with a place of its own (evth_pos) starts there: the ones of ma0402 never started from the map's
            # first landmark, ~250 units away. Without one (Colony 9's): the first landmark, a safe spot.
            if (x, y, z) != (0, 0, 0):
                y += 2
            elif inv.get(m) in first_lm:
                lm = first_lm[inv[m]]
                x, y, z = lm['jump_posX'], lm['jump_posY'], lm['jump_posZ']
            a, s = area_sub(m)
            secs = 0  # (its length: the cutscene header's frame count at 30 fps, cs/<event>.pcbeh +0x4c)
            hp = os.path.join(HERE, '..', 'targets', 'romfs', 'cs', r['event'] + '.pcbeh')
            if os.path.exists(hp):
                import struct
                secs = struct.unpack_from('<I', open(hp, 'rb').read(), 0x4c)[0] / 30.0
            # the story progress set to the scene's own scenario flag first: with the save's progress elsewhere the
            # game does not start it (2026-10-25, 7 Heart-to-Hearts of ma0401: 7/7 played with it, 0/6 without)
            lines += ['# %s event %s (%s %d, %.0f s)' % (m, r['event'], table, rid, secs),
                      'story %d' % r['scenario_flag'],
                      'warp %d %d %g %g %g 0' % (a, s, x, y, z), 'event %s %.0f' % (r['event'], secs)]
    open(out, 'w').write('\n'.join(lines) + '\n')
    print('%d steps -> %s' % (len(lines), out))


main()
