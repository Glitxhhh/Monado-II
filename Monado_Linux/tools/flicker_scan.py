"""Find short graphics glitches in a frame recording (XCDE_REC: <out>.raw RGBA 480x270 + <out>.txt frame numbers).

A glitch here is a screen area that changes for 1..N frames and then returns to what it was (Shulk's hair black for one
frame, the sky flat blue for one frame): per 30x30 block, the frame differs from both the frame `k` before and `k`
after much more than those two differ from each other. Camera cuts don't count (the before and after differ too).

  python tools/flicker_scan.py <rec base path> [threshold=40] [out dir for crops] [WxH of the recording, 480x270]
Prints one line per glitch (frame, block, strength) and, with an out dir, writes before / glitch / after images.
"""
import os
import sys

import cv2
import numpy as np

W, H, B = 480, 270, 30  # analysis grid (recordings of another size are scaled to it)


def main():
    base = sys.argv[1]
    thr = float(sys.argv[2]) if len(sys.argv) > 2 else 40.0
    out = sys.argv[3] if len(sys.argv) > 3 and sys.argv[3] != "-" else None
    rw, rh = (int(v) for v in sys.argv[4].split("x")) if len(sys.argv) > 4 else (W, H)
    raw = np.memmap(base + ".raw", dtype=np.uint8, mode="r")
    n = raw.size // (rw * rh * 4)
    frames = raw[: n * rw * rh * 4].reshape(n, rh, rw, 4)
    nums = [int(l.split()[0]) for l in open(base + ".txt")] if os.path.exists(base + ".txt") else list(range(n))
    # luminance per block, all frames (n x 9 x 16)
    lum = np.empty((n, H // B, W // B), np.float32)
    for i in range(n):
        f = frames[i, :, :, :3]
        if (rw, rh) != (W, H):
            f = cv2.resize(np.ascontiguousarray(f), (W, H), interpolation=cv2.INTER_AREA)
        f = f.astype(np.float32)
        y = f[..., 0] * 0.299 + f[..., 1] * 0.587 + f[..., 2] * 0.114
        lum[i] = y.reshape(H // B, B, W // B, B).mean(axis=(1, 3))
    hits = []
    for k in (1, 2, 3, 4, 6, 8):
        a, m, b = lum[: n - 2 * k], lum[k: n - k], lum[2 * k:]
        score = np.minimum(np.abs(m - a), np.abs(m - b)) - np.abs(a - b) * 2
        for i in range(score.shape[0]):
            j = np.unravel_index(score[i].argmax(), score[i].shape)
            if score[i][j] > thr:
                hits.append((float(score[i][j]), i + k, k, j))
    hits.sort(reverse=True)
    taken = []
    for s, i, k, j in hits:
        if any(abs(i - t) <= 8 for t in taken):
            continue
        taken.append(i)
        print("frame %d (rec #%d): block row %d col %d, %d-frame window, strength %.0f" % (nums[i], i, j[0], j[1], k, s))
        if out:
            from PIL import Image
            os.makedirs(out, exist_ok=True)
            row = np.concatenate([frames[max(i - k, 0), :, :, :3], frames[i, :, :, :3], frames[min(i + k, n - 1), :, :, :3]], axis=1)
            Image.fromarray(np.ascontiguousarray(row)).save(os.path.join(out, "glitch_%06d.png" % nums[i]))
    print("%d frames, %d glitches" % (n, len(taken)))


main()
