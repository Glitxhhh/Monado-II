#!/usr/bin/env python3
"""Coarse musical comparison of a game capture against a reference recording (both PCM16 WAV; decode an MP3 first with
ffmpeg -i ref.mp3 -ac 2 -ar 48000 ref.wav). Prints band-energy shares, dominant pitch classes, per-second chroma similarity
at the best time alignment and the level difference. Statistics only: reference audio is never stored in the repo.

  python tools/wav_compare.py build/audout.wav ref.wav
"""
import array, cmath, math, sys, wave

N = 16384

def fft(x):
    n = len(x)
    if n == 1:
        return x
    e, o = fft(x[0::2]), fft(x[1::2])
    t = [cmath.exp(-2j * math.pi * k / n) * o[k] for k in range(n // 2)]
    return [e[k] + t[k] for k in range(n // 2)] + [e[k] - t[k] for k in range(n // 2)]

WIN = [0.5 - 0.5 * math.cos(2 * math.pi * i / N) for i in range(N)]
BANDS = [(20, 100), (100, 300), (300, 1000), (1000, 3000), (3000, 8000), (8000, 16000)]

def load(path, maxsec):
    w = wave.open(path, 'rb')
    rate = w.getframerate()
    raw = w.readframes(min(w.getnframes(), int(maxsec * rate)))
    s = array.array('h'); s.frombytes(raw)
    return [(s[i] + s[i + 1]) / 2 for i in range(0, len(s) - 1, 2)], rate

def analyse(mono, rate):
    chroma, bands, rms = [], [], []
    for sec in range(len(mono) // rate - 1):
        seg = mono[sec * rate: sec * rate + N]
        r = math.sqrt(sum(x * x for x in seg) / N)
        X = fft([a * b for a, b in zip(seg, WIN)])
        mag = [abs(c) ** 2 for c in X[:N // 2]]
        bw = rate / N
        ch = [0.0] * 12
        for k in range(int(80 / bw), int(2000 / bw)):
            f = k * bw
            pc = int(round(12 * math.log2(f / 440.0) + 69)) % 12
            ch[pc] += mag[k]
        t = sum(ch) or 1
        chroma.append([c / t for c in ch])
        tot = sum(mag) or 1
        bands.append([sum(mag[int(lo / bw):int(hi / bw)]) / tot for lo, hi in BANDS])
        rms.append(r)
    return chroma, bands, rms

def cos(a, b):
    na = math.sqrt(sum(x * x for x in a)); nb = math.sqrt(sum(x * x for x in b))
    return sum(x * y for x, y in zip(a, b)) / (na * nb + 1e-12)

NAMES = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B']
mine, r1 = load(sys.argv[1], 70)
ref, r2 = load(sys.argv[2], 125)
cm, bm, rm = analyse(mine, r1)
cr, br, rr = analyse(ref, r2)

def avg(v):
    return [sum(x[i] for x in v) / len(v) for i in range(len(v[0]))]

# only active seconds of mine (skip the silent loading)
act = [i for i, r in enumerate(rm) if r > 5]
print('mine: active seconds', act[0], '..', act[-1], 'of', len(rm))
cm_a = [cm[i] for i in act]; bm_a = [bm[i] for i in act]
print('band energy % (20-100,100-300,300-1k,1-3k,3-8k,8-16k)')
print(' mine', [round(100 * x, 1) for x in avg(bm_a)])
print(' ref ', [round(100 * x, 1) for x in avg(br)])
top = lambda c: ', '.join(NAMES[i] for i in sorted(range(12), key=lambda i: -c[i])[:4])
print('top pitch classes mine:', top(avg(cm_a)), '| ref:', top(avg(cr)))
print('mean-chroma cosine', round(cos(avg(cm_a), avg(cr)), 3))

# best alignment: slide mine's active window over the reference
L = len(cm_a)
best = (-1, 0)
scores = []
for off in range(0, len(cr) - L + 1):
    sc = sum(cos(cm_a[i], cr[off + i]) for i in range(L)) / L
    scores.append(sc)
    if sc > best[0]:
        best = (sc, off)
print('best alignment: reference offset %d s, mean per-second chroma cosine %.3f (random chroma ~0.5-0.6)' % (best[1], best[0]))
for lo in (0, 10, 20, 30, 40):
    seg = cm_a[lo:lo + 10]
    if len(seg) == 10:
        print('  mine sec', act[0] + lo, '-> ref', best[1] + lo, 'cos', round(sum(cos(seg[i], cr[best[1] + lo + i]) for i in range(10)) / 10, 3))
print('rms ratio (mine/ref at best offset, dB):', round(20 * math.log10((sum(rm[i] for i in act) / L) / (sum(rr[best[1]:best[1] + L]) / L)), 1))
