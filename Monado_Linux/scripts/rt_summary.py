"""Summary of a real-time run log (scripts/rt_run.sh): build waits and frame periods by part of the run.
Parts: title + load (frames < 2500 with the save routes) and field (>= 2500).  Frame periods come from the [pace]
lines (LARECOMP_LOG_PACE=1; args: log [field start frame] [last frame]; periods over 100 ms are not logged there - the fps lines' 'max' shows them)."""
import re, sys
log = open(sys.argv[1], errors="replace").read().splitlines()
split = int(sys.argv[2]) if len(sys.argv) > 2 else 2500
upto = int(sys.argv[3]) if len(sys.argv) > 3 else 1 << 60  # (compare runs up to the same frame)
parts = {"title+load": dict(waits=0, wait_ms=0.0, over40=0, over100=0, frames=0, periods=[]),
         "field": dict(waits=0, wait_ms=0.0, over40=0, over100=0, frames=0, periods=[])}
frame = 0
for l in log:
    m = re.search(r"\[vk\] t=[\d.]+ frame (\d+) [\d.]+ fps max (\d+)ms", l)
    if m:
        frame = int(m.group(1))
        if frame > upto: break
        p = parts["field" if frame >= split else "title+load"]
        w = re.search(r"build waits (\d+) \(([\d.]+)ms\)", l)
        if w: p["waits"] += int(w.group(1)); p["wait_ms"] += float(w.group(2))
        if int(m.group(2)) > 100: p["over100"] += 1
        continue
    m = re.search(r"\[pace\] period ([\d.]+)", l)
    if m:
        p = parts["field" if frame >= split else "title+load"]
        v = float(m.group(1)); p["frames"] += 1; p["periods"].append(v)
        if v > 40: p["over40"] += 1
for k, p in parts.items():
    ps = sorted(p["periods"]); n = len(ps)
    med = ps[n // 2] if n else 0; p99 = ps[int(n * 0.99)] if n else 0
    print("%-10s build waits %5d (%8.1f ms)  frames %6d  over 40 ms %4d  windows with a frame over 100 ms %3d  median %.2f p99 %.2f"
          % (k, p["waits"], p["wait_ms"], p["frames"], p["over40"], p["over100"], med, p99))
print("last frame", frame)
