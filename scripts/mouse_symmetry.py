# Is the look symmetric?
#
# The title loads the stick with `lhz`, which zero-extends: a value of -1000
# arrives as 64536, not -1000. If it then treats that as signed the look is
# fine, and if it does not, moving left and moving right are not the same
# movement and the right one is a very large one.
#
# So: move the same number of counts each way and compare how far the picture
# went. A straight look gives two similar numbers.
import sys

import mouse_measure as mm   # reuses the capture, the input and the correlation


def shift_for(counts):
    before = mm.grab(mm.BOX)
    mm.time.sleep(0.4)
    mm.move(counts, 0)
    mm.time.sleep(0.6)
    after = mm.grab(mm.BOX)
    mm.time.sleep(0.3)
    mm.move(-counts, 0)
    mm.time.sleep(0.4)

    h, w = before.shape
    ax, aw = int(w * 0.56), int(w * 0.36)
    ay, ah = int(h * 0.20), int(h * 0.42)
    a = before[ay:ay + ah, ax:ax + aw]
    b = after[ay:ay + ah, ax:ax + aw]
    err, shift = mm.best_shift(a, b, max(8, int(aw * 0.35)))
    return shift, err


def main():
    pid = int(sys.argv[1]) if len(sys.argv) > 1 else 0
    mm.TARGET_PID = pid
    x0, y0, gw, gh = mm.game_rect()
    mm.BOX = (x0, y0, x0 + gw, y0 + gh)
    print("game window at %d,%d %dx%d" % (x0, y0, gw, gh))

    n = 400
    for _ in range(2):                       # twice each way, take the better match
        r_err, r = shift_for(n)
        l_err, l = shift_for(-n)
        print("%+d counts -> %+4d px   (%d counts -> %+d px)   errors %.1f / %.1f"
              % (n, r, -n, l, r_err, l_err))
        if r != 0 and l != 0 and abs(r + l) <= max(3, int(0.25 * abs(r))):
            print("  symmetric: the two are the same size and opposite in sign")
        else:
            print("  ASYMMETRIC, or one of them did not register")


main()
