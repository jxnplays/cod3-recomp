# Measures what a known amount of mouse movement actually does to the picture.
#
# Every attempt to work the gain out from a model has been wrong, so this
# measures it. Grab the screen, move the mouse a known number of counts, grab
# it again, and find how far the second is shifted from the first.
#
# The screen is grabbed directly rather than through the game's screenshot
# key, because the game is full screen and takes the keyboard with it: a
# synthetic key press goes to whatever has the focus, which while this runs is
# not the game. Raw mouse movement is registered focus independently, so it
# arrives either way.
#
# A yaw of a few degrees is, in the middle of the picture, very nearly a
# horizontal shift, so the best matching shift of a centre crop is the
# rotation the hand asked for. That gives pixels per count: the number the
# gain has to produce, and the one no arithmetic about the title's internals
# has managed to predict.
import ctypes
import sys
import time

import numpy as np
from PIL import ImageGrab

user32 = ctypes.windll.user32
user32.SetProcessDPIAware()

ULONG_PTR = ctypes.c_ulonglong if ctypes.sizeof(ctypes.c_void_p) == 8 else ctypes.c_ulong


def game_rect():
    """Where the game actually is.

    Not the whole screen: the game may be in a window, and in borderless mode
    beside something else, and a crop of the whole screen that is mostly
    something else correlates against nothing and reports no movement at all.
    """
    found = ctypes.wintypes.RECT() if hasattr(ctypes, "wintypes") else None
    import ctypes.wintypes as wt
    rect = wt.RECT()

    handles = []
    EnumWindowsProc = ctypes.WINFUNCTYPE(ctypes.c_bool, wt.HWND, wt.LPARAM)

    def cb(hwnd, _):
        pid = wt.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        if pid.value == TARGET_PID and user32.IsWindowVisible(hwnd):
            handles.append(hwnd)
        return True

    user32.EnumWindows(EnumWindowsProc(cb), 0)
    for hwnd in handles:
        if user32.GetWindowRect(hwnd, ctypes.byref(rect)):
            w = rect.right - rect.left
            h = rect.bottom - rect.top
            if w > 400 and h > 300:
                return rect.left, rect.top, w, h
    return 0, 0, user32.GetSystemMetrics(0), user32.GetSystemMetrics(1)

ULONG_PTR = ctypes.c_ulonglong if ctypes.sizeof(ctypes.c_void_p) == 8 else ctypes.c_ulong


class MOUSEINPUT(ctypes.Structure):
    _fields_ = [("dx", ctypes.c_long), ("dy", ctypes.c_long),
                ("mouseData", ctypes.c_ulong), ("dwFlags", ctypes.c_ulong),
                ("time", ctypes.c_ulong), ("dwExtraInfo", ULONG_PTR)]


class KEYBDINPUT(ctypes.Structure):
    _fields_ = [("wVk", ctypes.c_ushort), ("wScan", ctypes.c_ushort),
                ("dwFlags", ctypes.c_ulong), ("time", ctypes.c_ulong),
                ("dwExtraInfo", ULONG_PTR)]


class HARDWAREINPUT(ctypes.Structure):
    _fields_ = [("uMsg", ctypes.c_ulong), ("wParamL", ctypes.c_short),
                ("wParamH", ctypes.c_ushort)]


class _U(ctypes.Union):
    _fields_ = [("mi", MOUSEINPUT), ("ki", KEYBDINPUT), ("hi", HARDWAREINPUT)]


class INPUT(ctypes.Structure):
    _anonymous_ = ("u",)
    _fields_ = [("type", ctypes.c_ulong), ("u", _U)]


MOVE, ABSOLUTE = 0x0001, 0x8000

TARGET_PID = 0


def move(dx, dy):
    inp = INPUT(type=0)
    inp.mi = MOUSEINPUT(dx, dy, 0, MOVE | ABSOLUTE, 0, 0)
    user32.SendInput(1, ctypes.byref(inp), ctypes.sizeof(INPUT))


def grab(rect):
    return np.asarray(ImageGrab.grab(bbox=rect).convert("L"), dtype=np.float32)


def best_shift(a, b, limit):
    """The horizontal shift of b that best matches a, by direct comparison."""
    scores = []
    for s in range(-limit, limit + 1):
        if s >= 0:
            left, right = a[:, s:], b[:, :a.shape[1] - s]
        else:
            left, right = a[:, :s], b[:, -s:]
        if left.shape[1] < 64:
            continue
        scores.append((float(np.mean(np.abs(left - right))), s))
    scores.sort()
    return scores[0]


def main():
    global TARGET_PID
    counts = int(sys.argv[1]) if len(sys.argv) > 1 else 200
    TARGET_PID = int(sys.argv[2]) if len(sys.argv) > 2 else 0
    x0, y0, gw, gh = game_rect()
    print("game window at %d,%d %dx%d" % (x0, y0, gw, gh))
    box = (x0, y0, x0 + gw, y0 + gh)

    time.sleep(0.4)
    before = grab(box)
    time.sleep(0.4)

    move(counts, 0)
    time.sleep(0.6)
    after = grab(box)
    time.sleep(0.3)
    move(-counts, 0)          # leave the game where it was found
    time.sleep(0.3)

    # Only the part of the picture that is actually the game, and not the
    # weapon. Anything in front of the window is not moving with the view, and
    # the weapon is fixed to it, so both correlate against nothing. What is
    # left is the middle height of the right hand side: sky, tents and
    # buildings, which is where a few degrees of yaw is nearest to a plain
    # sideways shift anyway.
    h, w = before.shape
    ax, aw = int(w * 0.56), int(w * 0.36)
    ay, ah = int(h * 0.20), int(h * 0.42)
    a = before[ay:ay + ah, ax:ax + aw]
    b = after[ay:ay + ah, ax:ax + aw]
    print("crop %dx%d at %d,%d of the picture" % (aw, ah, ax, ay))

    limit = max(8, int(aw * 0.35))
    err, shift = best_shift(a, b, limit)
    err0, _ = best_shift(a, a, limit)   # the floor: a still picture

    print("%d counts moved the picture %d pixels sideways" % (counts, -shift))
    print("  mean error at that shift : %.3f" % err)
    print("  mean error against itself: %.3f  (the floor)" % err0)
    if shift:
        print("  %.3f pixels per count" % (abs(shift) / float(counts)))
    if err > err0 * 1.5 + 1.0:
        print("  WARNING: the match is poor, so this shift is probably not the real one")


main()
