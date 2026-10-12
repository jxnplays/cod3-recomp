# Measures what a known amount of mouse movement actually does to the picture.
#
# Every attempt to work the gain out from a model has been wrong, so this
# measures it: take the picture, move the mouse a known number of counts, take
# it again, and find how far the second is shifted from the first.
#
# A yaw of a few degrees is, in the middle of the picture, very nearly a
# horizontal shift, so the best-matching shift of a centre crop is the
# rotation the hand asked for. That gives pixels per count directly, which is
# the number the gain has to produce and the one no amount of arithmetic about
# the title's internals has managed to predict.
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class M {
    [StructLayout(LayoutKind.Sequential)] public struct MOUSEINPUT { public int dx; public int dy; public uint mouseData; public uint dwFlags; public uint time; public IntPtr dwExtraInfo; }
    [StructLayout(LayoutKind.Sequential)] public struct KEYBDINPUT  { public ushort wVk; public ushort wScan; public uint dwFlags; public uint time; public IntPtr dwExtraInfo; }
    [StructLayout(LayoutKind.Explicit)] public struct UNION {
        [FieldOffset(0)] public MOUSEINPUT mi;
        [FieldOffset(0)] public KEYBDINPUT ki;
    }
    [StructLayout(LayoutKind.Sequential)] public struct INPUT { public uint type; public UNION u; }
    [DllImport("user32.dll", SetLastError = true)] static extern uint SendInput(uint n, INPUT[] p, int size);
    const uint MOVE = 0x0001, KEY = 0x0002, ABSOLUTE = 0x8000;

    public static void Raw(int dx, int dy) {
        INPUT[] i = new INPUT[1];
        i[0].type = 0; i[0].u.mi.dx = dx; i[0].u.mi.dy = dy; i[0].u.mi.dwFlags = MOVE | ABSOLUTE;
        SendInput(1, i, Marshal.SizeOf(typeof(INPUT)));
    }
    public static void Tap(int vk) {
        INPUT[] i = new INPUT[2];
        i[0].type = 1; i[0].u.ki.wVk = (ushort)vk;
        i[1].type = 1; i[1].u.ki.wVk = (ushort)vk; i[1].u.ki.dwFlags = 0x0001;   // KEYUP
        SendInput(2, i, Marshal.SizeOf(typeof(INPUT)));
    }
}
"@ -ErrorAction SilentlyContinue

$shots = Join-Path $PSScriptRoot '..\build\CoD3Host\screenshots'
function Shot([string]$tag) {
    [M]::Tap(0x7B)                       # F12
    Start-Sleep -Milliseconds 900
    $f = Get-ChildItem $shots -Filter "*.png" -ErrorAction SilentlyContinue |
         Where-Object { $_.Name -notlike "meas_*" } |
         Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($f) { Copy-Item $f.FullName (Join-Path $shots "$tag.png") -Force; "  $tag -> $($f.Name)" }
    else { "  $tag -> NO SCREENSHOT (is the game running and focused?)" }
}

Write-Host "warming up"
Start-Sleep -Seconds 2
Shot "meas_before" | Write-Host

# A known movement. 200 counts is a couple of centimetres of hand on a
# 400 DPI mouse, enough to measure without the view running off the scenery.
[M]::Raw(200, 0)
Start-Sleep -Milliseconds 500
Shot "meas_after" | Write-Host

[M]::Raw(-200, 0)     # back, so the game is left as it was found
Start-Sleep -Milliseconds 300
Write-Host "done"
