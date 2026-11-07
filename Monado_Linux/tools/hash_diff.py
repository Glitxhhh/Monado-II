#!/usr/bin/env python3
"""Compare two per-frame hash files written with XCDE_FRAME_HASH (deterministic-mode runs).

  python tools/hash_diff.py a.txt b.txt [--max-shift 2] [--min-match 0.0]

Reports the identical prefix, how many frames match exactly, and how many match once one run is shifted by up to
--max-shift frames (the presenting thread of two runs can drift by a frame or two because host threads are not
scheduled identically). Exit code 1 if the matched fraction (with shifts) is below --min-match.
"""
import argparse, sys


def load(path):
    d = {}
    with open(path, encoding='utf8') as f:
        for line in f:
            if line.startswith('#') or not line.strip():
                continue
            fr, h = line.split()[:2]
            d[int(fr)] = h
    return d


def compare(a, b, max_shift=2):
    common = sorted(set(a) & set(b))
    exact = sum(1 for f in common if a[f] == b[f])
    prefix = 0
    for f in common:
        if a[f] != b[f]:
            break
        prefix += 1
    shifted = 0
    for f in common:
        if any(a[f] == b.get(f + s) for s in range(-max_shift, max_shift + 1)):
            shifted += 1
    return {'frames': len(common), 'exact': exact, 'prefix': prefix, 'shifted': shifted}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('a')
    ap.add_argument('b')
    ap.add_argument('--max-shift', type=int, default=2)
    ap.add_argument('--min-match', type=float, default=0.0)
    o = ap.parse_args()
    r = compare(load(o.a), load(o.b), o.max_shift)
    n = max(1, r['frames'])
    print(f"frames compared {r['frames']}; identical prefix {r['prefix']}; exact {r['exact']} ({r['exact'] / n:.1%}); "
          f"within +-{o.max_shift} frames {r['shifted']} ({r['shifted'] / n:.1%})")
    sys.exit(0 if r['shifted'] / n >= o.min_match else 1)


if __name__ == '__main__':
    main()
