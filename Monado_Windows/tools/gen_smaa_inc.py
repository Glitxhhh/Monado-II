"""third_party/smaa/SMAA.hlsl -> a C string literal (build/inc/smaa_hlsl.inc) for runtime/nvn/smaa_data.c.
Same output as LASRecomp's Makefile rule: CRs dropped, non-ASCII bytes as '?', backslashes and quotes escaped."""
import sys

src, dst = sys.argv[1], sys.argv[2]
data = open(src, "rb").read().replace(b"\r", b"")
text = bytes(b if b < 0x80 else ord("?") for b in data).decode("ascii")
lines = text.split("\n")
if lines and lines[-1] == "":
    lines.pop()
with open(dst, "w", newline="\n") as f:
    for line in lines:
        f.write('"' + line.replace("\\", "\\\\").replace('"', '\\"') + '\\n"\n')
