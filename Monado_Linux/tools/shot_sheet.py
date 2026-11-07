"""Sheet of our own capture PNGs: python tools/shot_sheet.py <out.png> <cols> <file>... (each cell 640x360, labelled with the file name)."""
import sys
from PIL import Image, ImageDraw
out, cols, fs = sys.argv[1], int(sys.argv[2]), sys.argv[3:]
w, h = 640, 360
rows = (len(fs) + cols - 1) // cols
sheet = Image.new("RGB", (cols * w, rows * h))
d = ImageDraw.Draw(sheet)
for k, f in enumerate(fs):
    x, y = (k % cols) * w, (k // cols) * h
    sheet.paste(Image.open(f).convert("RGB").resize((w, h)), (x, y))
    d.text((x + 6, y + 6), f.replace("\\", "/").split("/")[-1], fill=(255, 255, 0))
sheet.save(out)
