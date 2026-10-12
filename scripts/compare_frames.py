"""Compares two runs' frame dumps, for a visual regression check of the renderer.

    python scripts/compare_frames.py shots/baseline shots/new [--window 3] [--sheet out.png]

Both directories hold f-NNN.bmp files from COD3_FRAMEDUMP (the same
COD3_FRAMEDUMP_EVERY in both runs). For every frame of the second run the
nearest frame of the first within the window is found, by mean absolute
difference of the pixels, and the difference and the PSNR are printed; a
frame whose best match is far off is one worth looking at. The runs are
never in step exactly (the level's timing is the title's own), so the
window absorbs a little drift, and the numbers are a guide, not a verdict.
"""
import argparse
import glob
import math
import os
import sys

from PIL import Image, ImageChops, ImageStat


def load(path, size):
    image = Image.open(path).convert("RGB")
    if image.size != size:
        image = image.resize(size)
    return image


def difference(a, b):
    diff = ImageChops.difference(a, b)
    stat = ImageStat.Stat(diff)
    mad = sum(stat.mean) / 3.0
    mse = sum(s * s for s in stat.rms) / 3.0
    psnr = 99.0 if mse < 1e-9 else 20.0 * math.log10(255.0 / math.sqrt(mse))
    return mad, psnr


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("before")
    parser.add_argument("after")
    parser.add_argument("--window", type=int, default=3, help="frames either side to try for the best match")
    parser.add_argument("--size", default="320x180", help="the size to compare at")
    parser.add_argument("--sheet", help="a PNG of the worst matches, side by side")
    args = parser.parse_args()
    width, height = (int(v) for v in args.size.split("x"))
    size = (width, height)

    def frames(directory):
        out = {}
        for path in glob.glob(os.path.join(directory, "f-*.bmp")):
            number = int(os.path.basename(path)[2:5])
            out[number] = path
        return out

    before, after = frames(args.before), frames(args.after)
    if not before or not after:
        print("no frames found")
        return 1
    cache = {}

    def image(path):
        if path not in cache:
            cache[path] = load(path, size)
        return cache[path]

    results = []
    for number in sorted(after):
        best = None
        for offset in range(-args.window, args.window + 1):
            other = number + offset
            if other not in before:
                continue
            mad, psnr = difference(image(before[other]), image(after[number]))
            if best is None or mad < best[0]:
                best = (mad, psnr, other)
        if best is None:
            continue
        results.append((number, best))
        print(f"frame {number:3d}: best match {best[2]:3d}, mean difference {best[0]:6.2f} / 255, psnr {best[1]:5.1f} dB")
    if not results:
        print("nothing to compare")
        return 1
    mads = [r[1][0] for r in results]
    print(f"{len(results)} frames: mean difference {sum(mads) / len(mads):.2f}, worst {max(mads):.2f}, "
          f"{sum(1 for m in mads if m < 8.0)} under 8, {sum(1 for m in mads if m >= 16.0)} at or over 16")

    if args.sheet:
        worst = sorted(results, key=lambda r: -r[1][0])[:6]
        sheet = Image.new("RGB", (width * 2 + 8, (height + 8) * len(worst)), (40, 40, 40))
        for row, (number, (mad, psnr, other)) in enumerate(worst):
            sheet.paste(image(before[other]), (0, row * (height + 8)))
            sheet.paste(image(after[number]), (width + 8, row * (height + 8)))
        sheet.save(args.sheet)
        print(f"the worst {len(worst)} pairs are in {args.sheet}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
