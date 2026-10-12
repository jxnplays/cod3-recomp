"""Posts keys to the game's window without taking the focus, for scripted runs.

    python scripts/sendkeys.py [--delay S] KEY...

A KEY is a single character (typed as WM_CHAR), or one of: enter, escape,
console (the key left of 1, by scan code), f11, f12, space, up, down. A
number on its own like 2.5 waits that many seconds. The window is found
by its title, so the game must already be up.
"""
import ctypes
import ctypes.wintypes as wt
import sys
import time

user32 = ctypes.windll.user32
WM_KEYDOWN, WM_KEYUP, WM_CHAR = 0x100, 0x101, 0x102
VK = {"enter": 0x0D, "escape": 0x1B, "space": 0x20, "f11": 0x7A, "f12": 0x7B, "up": 0x26, "down": 0x28, "console": 0xC0}
SCAN = {"console": 0x29, "enter": 0x1C, "escape": 0x01, "space": 0x39, "f11": 0x57, "f12": 0x58, "up": 0x48, "down": 0x50}


def find_window():
    return user32.FindWindowW(None, "Call of Duty 3 - recompiled")


def press(hwnd, name):
    vk = VK[name]
    scan = SCAN.get(name, 0)
    user32.PostMessageW(hwnd, WM_KEYDOWN, vk, (scan << 16) | 1)
    if name == "enter":
        user32.PostMessageW(hwnd, WM_CHAR, 0x0D, (scan << 16) | 1)
    user32.PostMessageW(hwnd, WM_KEYUP, vk, (scan << 16) | 0xC0000001)


def type_text(hwnd, text):
    for c in text:
        user32.PostMessageW(hwnd, WM_CHAR, ord(c), 1)
        time.sleep(0.01)


def main():
    args = sys.argv[1:]
    hwnd = find_window()
    if not hwnd:
        print("the game's window is not open")
        return 1
    for arg in args:
        try:
            time.sleep(float(arg))
            continue
        except ValueError:
            pass
        if arg in VK:
            press(hwnd, arg)
        else:
            type_text(hwnd, arg)
        time.sleep(0.05)
    return 0


if __name__ == "__main__":
    sys.exit(main())
