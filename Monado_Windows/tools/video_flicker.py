"""One-frame dark flashes in a video (a frame with clearly more near-black pixels than both neighbours).
usage: python tools/video_flicker.py <video> <out.txt> [--crop-sides <fraction>] [--thresh <luma>]
Streams frames through ffmpeg at 160x90 grey; prints/writes (frame, time, count, neighbours)."""
import subprocess, sys
import numpy as np
video, out = sys.argv[1], sys.argv[2]
args = sys.argv[3:]
crop = float(args[args.index('--crop-sides') + 1]) if '--crop-sides' in args else 0.0
thr = int(args[args.index('--thresh') + 1]) if '--thresh' in args else 35
W, H = 160, 90
p = subprocess.Popen([r'C:\msys64\ucrt64\bin\ffmpeg.exe', '-v', 'error', '-i', video, '-vf', f'scale={W}:{H},format=gray',
                      '-f', 'rawvideo', '-'], stdout=subprocess.PIPE)
c0 = int(W * crop)
cnt = []
while True:
    b = p.stdout.read(W * H)
    if len(b) < W * H: break
    a = np.frombuffer(b, np.uint8).reshape(H, W)[:, c0:W - c0]
    cnt.append(int((a < thr).sum()))
cnt = np.array(cnt)
n = cnt.size
spikes = []
for i in range(1, n - 1):
    nb = max(cnt[i - 1], cnt[i + 1])
    if cnt[i] > nb + 120 and cnt[i] > 1.5 * nb:
        spikes.append((i, i / 29.97, int(cnt[i]), int(cnt[i - 1]), int(cnt[i + 1])))
with open(out, 'w') as f:
    for s in spikes: f.write(f'{s[0]} {s[1]:.2f}s {s[2]} ({s[3]} / {s[4]})\n')
print(f'{video}: {n} frames, {len(spikes)} one-frame dark spikes ({len(spikes) / max(n, 1) * 1000:.2f} per 1000 frames)')
