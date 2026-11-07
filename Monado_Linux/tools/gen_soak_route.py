"""Long soak route (T185): load the save as load_walk.txt, then repeat for ~2 h (real time, 30 fps):
walk in circles turning the camera, run straight out in one of four directions (streams other parts of the area),
fight (L = target nearest, A = attack) and run back.  Writes an input route (tests/routes/README.md format)."""
import math, sys

out = sys.argv[1] if len(sys.argv) > 1 else "soak_route.txt"
end = int(sys.argv[2]) if len(sys.argv) > 2 else 240000
ev = [(1, 0, 0, 0, 0, 0)]
for f in (600, 1000, 1400, 1800):          # load the newest save (as load_a / load_walk)
    ev += [(f, 1, 0, 0, 0, 0), (f + 6, 0, 0, 0, 0, 0)]
f, cyc = 2500, 0
while f < end:
    # 1) circles: one turn every 600 frames, 3 turns, camera turning
    for i in range(0, 1800, 20):
        a = 2 * math.pi * i / 600
        ev.append((f + i, 0, int(26000 * math.cos(a)), int(26000 * math.sin(a)), int(12000 * math.sin(a / 3)), 0))
    f += 1800
    # 2) run straight out for 20 s in a direction that rotates each cycle, then fight there, then run back
    d = (cyc % 4) * math.pi / 2 + (cyc // 4) * 0.4
    lx, ly = int(30000 * math.cos(d)), int(30000 * math.sin(d))
    ev.append((f, 0, lx, ly, 0, 0)); f += 600
    for k in range(10):                     # ~30 s of fighting: L every 15 s, A every 3 s
        ev += [(f, 0x40 if k % 5 == 0 else 1, 0, 0, 0, 0), (f + 6, 0, 0, 0, 0, 0)]; f += 90
    ev.append((f, 0, -lx, -ly, 0, 0)); f += 600
    ev.append((f, 0, 0, 0, 0, 0)); f += 60
    cyc += 1
with open(out, "w") as o:
    o.write("# xcde input v1\n# T185 soak route (tools/gen_soak_route.py): load the save, then circles / run out / fight / run back, repeated\n")
    for e in ev:
        o.write("%d %x %d %d %d %d\n" % e)
print(out, len(ev), "events, last frame", ev[-1][0])
