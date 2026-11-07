"""Contact sheets of the in-game flicker scanner's hits (XCDE_REC_SCAN=<dir>, runtime/nvn/vulkan/device.c).

  python tools/flicker_view.py <log> <hit dir> <out dir> [per sheet=12] [--strongest] [--kind=dark|flat|light ...] [--back] [--calm] [--maxk=n] [--delete]
Each hit (flicker_<frame>.ppm: before | frame | after) becomes one row: the three images cropped around the block the
log names, plus the whole frame small, labelled with the frame, strength and the tour step playing (the log's [tour]
lines). --delete removes the .ppm files once the sheets are written (recordings are not kept).
"""
import os
import re
import sys

from PIL import Image, ImageChops, ImageDraw, ImageStat


def main():
    log, hits, out = sys.argv[1], sys.argv[2], sys.argv[3]
    per = int(sys.argv[4]) if len(sys.argv) > 4 and sys.argv[4].isdigit() else 12
    delete = '--delete' in sys.argv
    info, steps = {}, []
    for l in open(log, errors='replace'):
        m = re.match(r'\[flicker\] frame (\d+): block \d+,\d+ \(px (\d+),(\d+)\), (\d+)-frame window, strength (\d+)', l)
        if m:
            info[int(m[1])] = tuple(int(v) for v in m.groups()[1:])
        m = re.match(r'\[tour\] frame (\d+): (# .*)', l)
        if m:
            steps.append((int(m[1]), m[2][2:].strip()))
    files = sorted(f for f in os.listdir(hits) if f.startswith('flicker_') and f.endswith('.ppm'))
    if '--strongest' in sys.argv:  # (strongest first)
        files.sort(key=lambda f: -info.get(int(f[8:-4]), (0, 0, 0, 0))[3])
    os.makedirs(out, exist_ok=True)
    kinds = [a[7:] for a in sys.argv if a.startswith('--kind=')]  # --kind=dark --kind=flat: only those
    rows = []
    for f in files:
        fr = int(f[8:-4])
        px, py, k, s = info.get(fr, (0, 0, 0, 0))
        label = next((t for at, t in reversed(steps) if at <= fr), 'before the tour')
        im = Image.open(os.path.join(hits, f)).convert('RGB')
        w, h = im.width // 3, im.height
        # the block in the three images: darker / flatter than both neighbours = a rendering fault's look (black hair,
        # flat sky); brighter and as detailed = usually an effect (ether shots, sparks, flashes)
        st = [ImageStat.Stat(im.crop((j * w + px, py, j * w + px + 40, py + 40)).convert('L')) for j in range(3)]
        mean, sd = [t.mean[0] for t in st], [t.stddev[0] for t in st]
        kind = 'dark' if mean[1] < min(mean[0], mean[2]) - 20 else 'flat' if sd[1] < 0.5 * min(sd[0], sd[2]) else 'light'
        # the area comes back as it was: before and after alike pixel by pixel, the frame unlike both (fast action and
        # camera cuts only match in mean brightness)
        g = [im.crop((j * w + px, py, j * w + px + 40, py + 40)).convert('L') for j in range(3)]
        mad = lambda x, y: ImageStat.Stat(ImageChops.difference(x, y)).mean[0]
        ab, ma, mb = mad(g[0], g[2]), mad(g[1], g[0]), mad(g[1], g[2])
        back = ab < 0.35 * min(ma, mb)
        # a calm shot: the whole before and after images nearly the same (in fast action and flashes before / after
        # match by chance; a fault there can't be told from an effect)
        sm = [im.crop((j * w, 0, j * w + w, h)).convert('L').resize((160, 90)) for j in range(3)]
        calm = mad(sm[0], sm[2]) < 6
        if '--calm' in sys.argv and not calm:
            continue
        if '--back' in sys.argv and not back:
            continue
        maxk = [int(a[7:]) for a in sys.argv if a.startswith('--maxk=')]  # --maxk=4: windows up to 4 frames
        if maxk and k > maxk[0]:
            continue
        if kinds and kind not in kinds:
            continue
        cw, ch = 320, 180
        x0 = min(max(px + 20 - cw // 2, 0), w - cw)
        y0 = min(max(py + 20 - ch // 2, 0), h - ch)
        row = Image.new('RGB', (cw * 3 + 320 + 30, ch + 18), (40, 40, 40))
        for j in range(3):
            row.paste(im.crop((j * w + x0, y0, j * w + x0 + cw, y0 + ch)), (j * (cw + 5), 18))
        small = im.crop((w, 0, 2 * w, h)).resize((320, 180))
        ImageDraw.Draw(small).rectangle([px * 320 // w, py * 180 // h, (px + 40) * 320 // w, (py + 40) * 180 // h], outline=(255, 0, 0))
        row.paste(small, (cw * 3 + 20, 18))
        ImageDraw.Draw(row).text((4, 3), 'frame %d  %s%s  strength %d  window %d  |  %s' % (fr, kind, (' back' if back else '') + (' calm' if calm else ''), s, k, label), fill=(255, 255, 0))
        rows.append(row)
    for i in range(0, len(rows), per):
        part = rows[i:i + per]
        sheet = Image.new('RGB', (part[0].width, sum(r.height + 4 for r in part)), (0, 0, 0))
        y = 0
        for r in part:
            sheet.paste(r, (0, y))
            y += r.height + 4
        sheet.save(os.path.join(out, 'sheet_%03d.png' % (i // per)))
    print('%d hits -> %d sheets in %s' % (len(rows), (len(rows) + per - 1) // per, out))
    if delete:
        for f in files:
            os.remove(os.path.join(hits, f))


main()
