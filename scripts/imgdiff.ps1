# Compare two folders of pictures index by index, so "it looks the same" gets
# a number behind it.
#
#   .\scripts\imgdiff.ps1 <folderA> <folderB>
#
# Per pair: the mean and the worst absolute difference over red, green and
# blue, and how many pixels differ at all. The scene is alive, so the camera
# and the animation move between shots; a run that is merely the same game
# still shows a small mean. A correctness fault from a stale cached resource
# shows up as whole regions flat at the worst, not spread thinly.

param(
    [Parameter(Mandatory = $true)][string]$A,
    [Parameter(Mandatory = $true)][string]$B,
    [int]$Step = 2,
    [double]$Limit = 2.0
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$fa = @(Get-ChildItem -LiteralPath $A -Filter *.png | Sort-Object Name)
$fb = @(Get-ChildItem -LiteralPath $B -Filter *.png | Sort-Object Name)
if ($fa.Count -eq 0 -or $fb.Count -eq 0) { Write-Host "no pictures in one of the folders" -ForegroundColor Yellow; exit 1 }

Write-Host ""
Write-Host "A $A ($($fa.Count))" -ForegroundColor Cyan
Write-Host "B $B ($($fb.Count))" -ForegroundColor Cyan
Write-Host ""
Write-Host ("{0,-34} {1,9} {2,9} {3,10}" -f 'pair', 'mean', 'worst', 'changed%')
Write-Host ("-" * 66)

$worstMean = 0.0
$n = [Math]::Min($fa.Count, $fb.Count)
for ($i = 0; $i -lt $n; $i++) {
    $ba = [System.Drawing.Bitmap]::FromFile($fa[$i].FullName)
    $bb = [System.Drawing.Bitmap]::FromFile($fb[$i].FullName)
    try {
        if ($ba.Width -ne $bb.Width -or $ba.Height -ne $bb.Height) {
            Write-Host ("{0,-34} size {1}x{2} vs {3}x{4}" -f $fa[$i].Name, $ba.Width, $ba.Height, $bb.Width, $bb.Height) -ForegroundColor Red
            continue
        }
        $sum = 0.0; $worst = 0; $changed = 0; $samples = 0
        for ($y = 0; $y -lt $ba.Height - 1; $y += $Step) {
            for ($x = 0; $x -lt $ba.Width - 1; $x += $Step) {
                $p = $ba.GetPixel($x, $y); $q = $bb.GetPixel($x, $y)
                $d = [Math]::Abs($p.R - $q.R); $e = [Math]::Abs($p.G - $q.G); $f2 = [Math]::Abs($p.B - $q.B)
                if ($e -gt $d) { $d = $e }; if ($f2 -gt $d) { $d = $f2 }
                $sum += $d; $samples++
                if ($d -gt $worst) { $worst = $d }
                if ($d -gt 4) { $changed++ }
            }
        }
        $mean = $sum / $samples
        if ($mean -gt $worstMean) { $worstMean = $mean }
        $colour = if ($mean -le $Limit) { 'Green' } else { 'Red' }
        Write-Host ("{0,-34} {1,9:F3} {2,9} {3,9:F2}" -f $fa[$i].Name, $mean, $worst, (100.0 * $changed / $samples)) -ForegroundColor $colour
    }
    finally { $ba.Dispose(); $bb.Dispose() }
}

Write-Host ""
Write-Host ("worst mean over the pairs: {0:F3} (limit {1:F3})" -f $worstMean, $Limit)
if ($worstMean -gt $Limit) { Write-Host "DIFFERENT" -ForegroundColor Red; exit 1 }
Write-Host "SAME within the limit" -ForegroundColor Green
exit 0
