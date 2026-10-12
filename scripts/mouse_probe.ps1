# Drives the mouse with a known pattern so the game's mouse look can be
# measured without a hand on it.
#
# SendInput's absolute moves are what the game's raw input sees, so the counts
# it reports are the counts sent here. The sweep is a slow drag, the flick is a
# fast one, and both are repeated, so the log can show how far the stick moved
# for the same distance at two speeds.
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class Probe {
    [StructLayout(LayoutKind.Sequential)] public struct MOUSEINPUT { public int dx; public int dy; public uint mouseData; public uint dwFlags; public uint time; public IntPtr dwExtraInfo; }
    [StructLayout(LayoutKind.Explicit)] public struct INPUTUNION { [FieldOffset(0)] public MOUSEINPUT mi; }
    [StructLayout(LayoutKind.Sequential)] public struct INPUT { public uint type; public INPUTUNION u; }
    [DllImport("user32.dll", SetLastError = true)] static extern uint SendInput(uint n, INPUT[] p, int size);
    const uint MOVE = 0x0001, ABSOLUTE = 0x8000;
    public static void Move(int dx, int dy) {
        INPUT[] i = new INPUT[1];
        i[0].type = 0; i[0].u.mi.dx = dx; i[0].u.mi.dy = dy; i[0].u.mi.dwFlags = MOVE | ABSOLUTE;
        SendInput(1, i, Marshal.SizeOf(typeof(INPUT)));
    }
}
"@ -ErrorAction SilentlyContinue

# The window's own position is where the pointer is pinned, so the moves are
# relative to the middle of the screen: the game keeps re-centring anyway.
$cx = [int]([System.Windows.Forms.Screen]::PrimaryScreen.Bounds.Width / 2)
$cy = [int]([System.Windows.Forms.Screen]::PrimaryScreen.Bounds.Height / 2)
Add-Type -AssemblyName System.Windows.Forms
$cx = [int]([System.Windows.Forms.Screen]::PrimaryScreen.Bounds.Width / 2)
$cy = [int]([System.Windows.Forms.Screen]::PrimaryScreen.Bounds.Height / 2)

Write-Host "screen centre $cx,$cy"

# Slow sweep: 200 counts a step, 40 steps. A hand dragging, roughly.
Write-Host "slow sweep"
[Probe]::Move($cx, $cy)
for ($i = 0; $i -lt 40; $i++) { [Probe]::Move($cx + 200, $cy); Start-Sleep -Milliseconds 16 }

# Fast sweep: the same 8000 counts in far fewer steps.
Write-Host "fast sweep"
[Probe]::Move($cx, $cy)
for ($i = 0; $i -lt 8; $i++) { [Probe]::Move($cx + 1000, $cy); Start-Sleep -Milliseconds 8 }

# Flick: one very large step, the hardest case for a gain that clamps.
Write-Host "flick"
[Probe]::Move($cx, $cy)
for ($i = 0; $i -lt 4; $i++) { [Probe]::Move($cx + 6000, $cy); Start-Sleep -Milliseconds 8 }

[Probe]::Move($cx, $cy)
Write-Host "done"
