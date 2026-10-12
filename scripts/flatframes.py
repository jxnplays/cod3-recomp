"""flatframes.py files...: scores frame dumps for flat single-coloured area, flagging the ones a giant triangle covers.

Use with COD3_FRAMEDUMP=path COD3_FRAMEDUMP_EVERY=3 COD3_FRAMEDUMP_KEEP=999: a frame of the level scores
about 0.05 to 0.2, a fade from black or a sky shot up to 0.5, a triangle across the picture 0.5 to 0.95."""
import sys, glob
from PIL import Image
import numpy as np
def score(path):
    im = Image.open(path).convert('RGB')
    a = np.asarray(im.resize((im.width // 4, im.height // 4)), dtype=np.int16)
    # crop the letterbox bars: rows whose mean brightness is nearly black
    rows = a.mean(axis=(1, 2)) > 8
    a = a[rows]
    if a.shape[0] < 10: return 0.0
    dx = np.abs(np.diff(a, axis=1)).sum(axis=2)
    dy = np.abs(np.diff(a, axis=0)).sum(axis=2)
    flat = (dx[:-1, :] < 3) & (dy[:, :-1] < 3)
    return float(flat.mean())
files = []
for pat in sys.argv[1:]: files += glob.glob(pat)
files.sort()
bad = 0
for f in files:
    s = score(f)
    if s > 0.35: bad += 1
    print(f"{f} {s:.3f}{'  <-- FLAT' if s > 0.35 else ''}")
print(f"{bad} flat of {len(files)}")
