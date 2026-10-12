# draws.py FILE [OUT.png] [first] [count]: the screenshot key's per draw pictures as a contact sheet
import sys, struct
import numpy as np
from PIL import Image, ImageDraw
path = sys.argv[1]
out = sys.argv[2] if len(sys.argv) > 2 else path + '.png'
first = int(sys.argv[3]) if len(sys.argv) > 3 else 0
count = int(sys.argv[4]) if len(sys.argv) > 4 else 10**9
data = open(path, 'rb').read()
at = 0; pics = []
while at + 16 <= len(data):
    magic, number, w, h = struct.unpack_from('<4I', data, at); at += 16
    if magic != 0x57415244: break
    px = np.frombuffer(data, np.uint8, w * h * 4, at).reshape(h, w, 4); at += w * h * 4
    pics.append((number, px))
print(len(pics), 'pictures, draws', pics[0][0] if pics else None, '..', pics[-1][0] if pics else None)
sel = [p for p in pics if p[0] >= first][:count]
if sel:
    h, w = sel[0][1].shape[:2]
    hv = int(w * 0.6)  # the frame's own rows (the target is taller than the picture)
    cols = 12; rows = (len(sel) + cols - 1) // cols
    sheet = Image.new('RGB', (cols * w, rows * (hv + 12)), (40, 40, 40))
    d = ImageDraw.Draw(sheet)
    for i, (n, px) in enumerate(sel):
        x, y = (i % cols) * w, (i // cols) * (hv + 12)
        sheet.paste(Image.fromarray(px[:hv, :, :3].copy()), (x, y + 12))
        d.text((x + 2, y), str(n), fill=(255, 255, 0))
    sheet.save(out); print('sheet', out, sheet.size)
