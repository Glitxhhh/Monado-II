"""Compare our captures with a reference recording of the Switch version (T144).

The reference is a gameplay video the user supplied, extracted outside the repo (never commit game footage):
    ffmpeg -i video.mp4 -filter_complex "fps=1,split=2[a][b];[b]scale=320:180[s]" \
           -map "[a]" -q:v 2 ref/full/f_%05d.jpg -map "[s]" -q:v 4 ref/idx/f_%05d.jpg

For each capture: the most similar reference second (normalized correlation of 64x36 grayscale thumbnails, optionally
within --window "from-to" seconds), then a region-by-region comparison at 1280x720 (16x9 cells): mean colour difference
and edge-structure difference per cell. Writes <out>/<capture>_cmp.png (ours | reference | heat map of the cells that
differ most) and prints a table. Matches below --min-corr are reported as "no match" (scene not in the video).

The video has its own timing, camera and compression: small differences everywhere are normal. What matters are
cells far above the frame's median (a missing object, a wrong texture, a black area).
"""
import argparse
import glob
import os
import sys

import numpy as np
from PIL import Image, ImageDraw

THUMB = (64, 36)
CMP = (1280, 720)
GRID = (16, 9)


def thumb(img):
    a = np.asarray(img.convert("L").resize(THUMB, Image.BILINEAR), dtype=np.float32).ravel()
    a -= a.mean()
    n = np.linalg.norm(a)
    return a / n if n > 0 else a


def load_index(ref_dir):
    cache = os.path.join(ref_dir, "thumbs.npy")
    files = sorted(glob.glob(os.path.join(ref_dir, "idx", "f_*.jpg")))
    if os.path.exists(cache):
        t = np.load(cache)
        if len(t) == len(files):
            return files, t
    t = np.stack([thumb(Image.open(f)) for f in files])
    np.save(cache, t)
    return files, t


def edges(a):
    g = a.mean(2)
    return np.abs(np.diff(g, axis=0))[:, :-1] + np.abs(np.diff(g, axis=1))[:-1, :]


def compare(ours_path, ref_dir, files, thumbs, window, min_corr, out_dir):
    ours = Image.open(ours_path).convert("RGB").resize(CMP, Image.BILINEAR)
    corr = thumbs @ thumb(ours)
    lo, hi = window
    idx = np.arange(len(files))
    mask = (idx + 1 >= lo) & (idx + 1 <= hi)
    corr = np.where(mask, corr, -1)
    best = int(np.argmax(corr))
    c = float(corr[best])
    name = os.path.splitext(os.path.basename(ours_path))[0]
    if c < min_corr:
        print(f"{name}: no match (best {c:.2f} at {best + 1}s)")
        return
    full = os.path.join(ref_dir, "full", os.path.basename(files[best]))
    ref = Image.open(full).convert("RGB").resize(CMP, Image.BILINEAR)
    a = np.asarray(ours, dtype=np.float32)
    b = np.asarray(ref, dtype=np.float32)
    # the video lifts blacks and shifts colours (measured: console black = 29 in it, dark R/G lifted more than B):
    # fit the reference's levels to ours per channel (least squares, gain + offset) so only real differences remain
    for ch in range(3):
        x, y = b[..., ch].ravel(), a[..., ch].ravel()
        g, o = np.polyfit(x, y, 1)
        b[..., ch] = np.clip(b[..., ch] * g + o, 0, 255)
    ref = Image.fromarray(b.astype(np.uint8))
    ea, eb = edges(a), edges(b)
    cw, ch = CMP[0] // GRID[0], CMP[1] // GRID[1]
    score = np.zeros((GRID[1], GRID[0]))
    for gy in range(GRID[1]):
        for gx in range(GRID[0]):
            s = (slice(gy * ch, (gy + 1) * ch), slice(gx * cw, (gx + 1) * cw))
            se = (slice(gy * ch, min((gy + 1) * ch, ea.shape[0])), slice(gx * cw, min((gx + 1) * cw, ea.shape[1])))
            col = np.abs(a[s].mean((0, 1)) - b[s].mean((0, 1))).mean()
            st = np.abs(ea[se] - eb[se]).mean()
            score[gy, gx] = col + st
    med = np.median(score) + 1e-6
    rel = score / med
    top = np.dstack(np.unravel_index(np.argsort(-rel, axis=None)[:5], rel.shape))[0]
    canvas = Image.new("RGB", (CMP[0] * 3, CMP[1]))
    canvas.paste(ours, (0, 0))
    canvas.paste(ref, (CMP[0], 0))
    heat = ours.copy()
    d = ImageDraw.Draw(heat, "RGBA")
    for gy in range(GRID[1]):
        for gx in range(GRID[0]):
            r = rel[gy, gx]
            if r > 1.5:
                alpha = int(min(200, (r - 1.5) * 80))
                d.rectangle([gx * cw, gy * ch, (gx + 1) * cw - 1, (gy + 1) * ch - 1], fill=(255, 0, 0, alpha))
    canvas.paste(heat, (CMP[0] * 2, 0))
    os.makedirs(out_dir, exist_ok=True)
    out = os.path.join(out_dir, f"{name}_cmp.png")
    canvas.save(out)
    cells = ", ".join(f"({gx},{gy}) x{rel[gy, gx]:.1f}" for gy, gx in top)
    print(f"{name}: reference {best + 1}s (corr {c:.2f}); most different cells {cells} -> {out}")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("captures", nargs="+", help="our PNG captures")
    p.add_argument("--ref", default="ref", help="folder with idx/ and full/ frames")
    p.add_argument("--window", default="0-99999", help="reference seconds to search, from-to")
    p.add_argument("--min-corr", type=float, default=0.6)
    p.add_argument("--out", default="ref/cmp")
    a = p.parse_args()
    files, thumbs = load_index(a.ref)
    if not files:
        sys.exit(f"no reference frames in {a.ref}/idx")
    lo, hi = (int(x) for x in a.window.split("-"))
    for cap in a.captures:
        compare(cap, a.ref, files, thumbs, (lo, hi), a.min_corr, a.out)


if __name__ == "__main__":
    main()
