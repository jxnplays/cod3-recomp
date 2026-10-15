# Find, for every picture in A, the picture in B it most resembles.
#
#   .\scripts\imgnearest.ps1 <folderA> <folderB> [-Limit 2.0] [-Step 4]
#
# Comparing two runs index by index assumes they are at the same moment, which
# is only true when they also run at the same speed. Change the speed and the
# timed parts of a level - the intro flyover, a firefight - drift apart, and an
# index-by-index diff then reports a difference that is really a different
# scene. This asks the question that survives that: does every frame the
# changed build drew also exist in the other build's frames?
#
# A frame that matches something well is correct. A frame whose best match is
# still poor is geometry that the other build never drew, which is what a
# missed cache invalidation looks like.

param(
    [Parameter(Mandatory = $true)][string]$A,
    [Parameter(Mandatory = $true)][string]$B,
    [double]$Limit = 2.0,
    [int]$Step = 4
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$fa = @(Get-ChildItem -LiteralPath $A -Filter *.png | Sort-Object Name)
$fb = @(Get-ChildItem -LiteralPath $B -Filter *.png | Sort-Object Name)
if ($fa.Count -eq 0 -or $fb.Count -eq 0) { Write-Host 'no pictures in one of the folders' -ForegroundColor Yellow; exit 1 }

# Held small on purpose: the full frames are thousands of pixels wide and this
# is a comparison, not a render. Every Step'th pixel, every channel.
function Get-Sample([string]$Path) {
    $bmp = [System.Drawing.Bitmap]::FromFile($Path)
    try {
        $v = [System.Collections.Generic.List[byte]]::new()
        $rect = [System.Drawing.Rectangle]::new(0, 0, $bmp.Width, $bmp.Height)
        $data = $bmp.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly, [System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
        try {
            $bytes = [byte[]]::new($data.Stride * $data.Height)
            [System.Runtime.InteropServices.Marshal]::Copy($data.Scan0, $bytes, 0, $bytes.Length)
            for ($y = 0; $y -lt $data.Height; $y += $Step) {
                $row = $y * $data.Stride
                for ($x = 0; $x -lt $data.Width; $x += $Step) {
                    $p = $row + $x * 3
                    $v.Add($bytes[$p]); $v.Add($bytes[$p + 1]); $v.Add($bytes[$p + 2])
                }
            }
        }
        finally { $bmp.UnlockBits($data) }
        return $v.ToArray()
    }
    finally { $bmp.Dispose() }
}

Write-Host ''
Write-Host "A $A ($($fa.Count) frames)" -ForegroundColor Cyan
Write-Host "B $B ($($fb.Count) frames)" -ForegroundColor Cyan
Write-Host "step $Step, limit $Limit" -ForegroundColor Cyan
Write-Host ''
Write-Host ("{0,-34} {1,9}  best match in B" -f 'A frame', 'mean')
Write-Host ('-' * 72)

$sa = @(); foreach ($f in $fa) { $sa += , (Get-Sample $f.FullName) }
$sb = @(); foreach ($f in $fb) { $sb += , (Get-Sample $f.FullName) }
Write-Host "sampled: A $(($sa | ForEach-Object { $_.Length } | Measure-Object -Sum).Sum) bytes, B $(($sb | ForEach-Object { $_.Length } | Measure-Object -Sum).Sum) bytes" -ForegroundColor DarkGray
Write-Host ''

$worst = 0.0
$bad = 0
for ($i = 0; $i -lt $sa.Count; $i++) {
    $best = [double]::MaxValue; $bestJ = -1
    for ($j = 0; $j -lt $sb.Count; $j++) {
        $x = $sa[$i]; $y = $sb[$j]
        $n = [Math]::Min($x.Length, $y.Length)
        $sum = 0.0
        for ($k = 0; $k -lt $n; $k += 3) {
            $d = [Math]::Abs($x[$k] - $y[$k]); $e = [Math]::Abs($x[$k + 1] - $y[$k + 1]); $f2 = [Math]::Abs($x[$k + 2] - $y[$k + 2])
            if ($e -gt $d) { $d = $e }; if ($f2 -gt $d) { $d = $f2 }
            $sum += $d
        }
        $m = $sum / (($n / 3))
        if ($m -lt $best) { $best = $m; $bestJ = $j }
    }
    if ($best -gt $worst) { $worst = $best }
    $ok = $best -le $Limit
    if (-not $ok) { $bad++ }
    $colour = if ($ok) { 'Green' } else { 'Red' }
    Write-Host ("{0,-34} {1,9:F3}  {2}" -f $fa[$i].Name, $best, $fb[$bestJ].Name) -ForegroundColor $colour
}

Write-Host ''
Write-Host ("worst best-match: {0:F3}, limit {1:F3}, unmatched {2} of {3}" -f $worst, $Limit, $bad, $fa.Count)
if ($bad -gt 0) { Write-Host 'SOME FRAMES HAVE NO COUNTERPART' -ForegroundColor Red; exit 1 }
Write-Host 'EVERY FRAME HAS A COUNTERPART' -ForegroundColor Green
exit 0
