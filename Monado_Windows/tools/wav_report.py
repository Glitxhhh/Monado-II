#!/usr/bin/env python3
"""Summarise a PCM16 WAV (e.g. LARECOMP_AUDOUT_WAV dump): duration, per-second RMS/peak, silence ratio, clipping, DC.

  python tools/wav_report.py build/audout.wav [--min-active 0.5] [--max-clip 0.001]

Exit code 0 if the audio looks alive (>= --min-active of the seconds above -60 dBFS, clipping below --max-clip),
1 otherwise. No game data is read or stored: the report only holds statistics.
"""
import argparse, array, math, sys, wave


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('wav')
    ap.add_argument('--min-active', type=float, default=0.5)
    ap.add_argument('--max-clip', type=float, default=0.001)
    a = ap.parse_args()
    with wave.open(a.wav, 'rb') as w:
        ch, rate, width, n = w.getnchannels(), w.getframerate(), w.getsampwidth(), w.getnframes()
        if width != 2:
            sys.exit('only PCM16 is supported')
        raw = w.readframes(n)
    s = array.array('h')
    s.frombytes(raw[:len(raw) // 2 * 2])
    if sys.byteorder == 'big':
        s.byteswap()
    n = len(s) // ch
    print(f'{a.wav}: {ch} ch, {rate} Hz, {n / rate:.2f} s ({n} frames)')
    active = clip = 0
    secs = max(1, n // rate)
    for i in range(secs):
        seg = s[i * rate * ch:(i + 1) * rate * ch]
        if not seg:
            break
        pk = max(max(seg), -min(seg))
        rms = math.sqrt(sum(x * x for x in seg) / len(seg))
        db = 20 * math.log10(rms / 32768) if rms else -120.0
        c = sum(1 for x in seg if x >= 32767 or x <= -32768)
        clip += c
        active += db > -60
        if i < 60:
            print(f'  {i:3d}s  rms {db:6.1f} dBFS  peak {pk:5d}  clip {c}')
    total = secs * rate * ch
    ratio = active / secs
    dc = sum(s) / len(s) if len(s) else 0
    print(f'active seconds (> -60 dBFS): {active}/{secs} = {ratio:.0%}; clipped samples {clip / max(1, total):.4%}; DC offset {dc:.1f}')
    ok = ratio >= a.min_active and clip / max(1, total) <= a.max_clip
    print('RESULT:', 'OK' if ok else 'FAIL')
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
