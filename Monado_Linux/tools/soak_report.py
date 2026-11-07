"""T185 soak report: the [mem] lines of a game log (LARECOMP_VK_PROF=1, every ~10 s) and the proc.csv of scripts/soak.sh
(handles / private / working set every minute, from outside). Prints one row per <step> minutes and the growth per hour
after the warm-up (default: from minute 15).   python tools/soak_report.py <outdir> [step min] [warm-up min]"""
import re, sys, csv
out = sys.argv[1]; step = float(sys.argv[2]) if len(sys.argv) > 2 else 10; warm = float(sys.argv[3]) if len(sys.argv) > 3 else 15
mem = []  # (t s, private, ws, gpu process heap0, ours heap0, vcache)
t = 0.0
for l in open(out + "/game.log", errors="replace"):
    m = re.search(r"\[vk\] t=([\d.]+)", l)
    if m: t = float(m.group(1)); continue
    m = re.search(r"\[mem\] private (\d+) MB, working set (\d+) MB.*?vertex cache (\d+) MB.*?heap0\(device\) ours (\d+) MB of \d+, process (\d+) MB", l)
    if m: mem.append((t, int(m.group(1)), int(m.group(2)), int(m.group(5)), int(m.group(4)), int(m.group(3))))
proc = []
for r in csv.reader(open(out + "/proc.csv")):
    if len(r) == 7 and r[0].isdigit(): proc.append(tuple(int(x) for x in r))
print("min   private  wset  gpu-proc gpu-ours vcache | handles threads cpu-s")
def near(rows, ts):
    return min(rows, key=lambda r: abs(r[0] - ts)) if rows else None
end = max(mem[-1][0] if mem else 0, proc[-1][0] if proc else 0)
k = 0.0
while k * 60 <= end + 1:
    a = near(mem, k * 60); b = near(proc, k * 60)
    print("%4.0f  %7s %5s %8s %8s %6s | %7s %7s %5s" % (k, *(a[1:] if a else ("",) * 5), *((b[2], b[5], b[6]) if b else ("",) * 3)))
    k += step
def slope(rows, i):
    rows = [r for r in rows if r[0] >= warm * 60]
    if len(rows) < 3: return float("nan")
    n = len(rows); mx = sum(r[0] for r in rows) / n; my = sum(r[i] for r in rows) / n
    sxx = sum((r[0] - mx) ** 2 for r in rows)
    return sum((r[0] - mx) * (r[i] - my) for r in rows) / sxx * 3600 if sxx else float("nan")
print("growth per hour after minute %.0f (least squares): private %+.0f MB, working set %+.0f MB, GPU process heap %+.0f MB, "
      "GPU ours %+.0f MB, handles %+.0f, threads %+.1f" % (warm, slope(mem, 1), slope(mem, 2), slope(mem, 3), slope(mem, 4),
                                                           slope(proc, 2), slope(proc, 5)))
