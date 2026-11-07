"""Contact sheet of reference-video frames: python tools/contact_sheet.py <from> <to> <step> <out.png> [cols] (frames from ref/idx)."""
import sys
from PIL import Image, ImageDraw
a, b, s, out = int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
cols = int(sys.argv[5]) if len(sys.argv) > 5 else 5
ids = list(range(a, b + 1, s))
rows = (len(ids) + cols - 1) // cols
sheet = Image.new("RGB", (cols * 320, rows * 180))
d = ImageDraw.Draw(sheet)
for k, i in enumerate(ids):
    try:
        im = Image.open(f"ref/idx/f_{i:05d}.jpg")
    except OSError:
        continue
    x, y = (k % cols) * 320, (k // cols) * 180
    sheet.paste(im, (x, y)); d.text((x + 4, y + 4), str(i), fill=(255, 255, 0))
sheet.save(out)
