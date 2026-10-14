# Measure alternating A/B captures: picture 1 vs 2, 3 vs 4, and so on.
#
#   .\scripts\ab.ps1 <folder>
#
# Consecutive captures alternate between the settings and the COD3_SHOT_ALT
# values, so each neighbouring pair is the same scene one frame apart with only
# the feature differing. Comparing two separate runs cannot do this - the
# camera is somewhere different each time, which is what made the first
# attempt to measure sharpening useless.

param([Parameter(Mandatory = $true)][string]$Folder, [int]$Step = 2)

Add-Type -AssemblyName System.Drawing

$files = @(Get-ChildItem -LiteralPath $Folder -Filter *.png | Sort-Object Name)
if ($files.Count -lt 2) { Write-Host "need at least two pictures in $Folder" -ForegroundColor Yellow; return }

function Measure-Picture([string]$path) {
    $bmp = [System.Drawing.Bitmap]::FromFile($path)
    try {
        $n = 0; $luma = 0.0; $sat = 0.0; $grad = 0.0
        for ($y = 0; $y -lt $bmp.Height - $Step; $y += $Step) {
            for ($x = 0; $x -lt $bmp.Width - $Step; $x += $Step) {
                $a = $bmp.GetPixel($x, $y)
                $b = $bmp.GetPixel($x + $Step, $y)
                $r = $a.R / 255.0; $g = $a.G / 255.0; $bl = $a.B / 255.0
                $l = 0.299 * $r + 0.587 * $g + 0.114 * $bl
                $luma += $l; $n++
                $sat += ([math]::Abs($r - $l) + [math]::Abs($g - $l) + [math]::Abs($bl - $l)) / 3.0
                $grad += [math]::Abs($g - $b.G / 255.0)
            }
        }
        return [pscustomobject]@{ luma = $luma / $n; sat = $sat / $n; grad = $grad / $n }
    } finally { $bmp.Dispose() }
}

$even = @(); $odd = @()
for ($i = 0; $i -lt $files.Count; $i += 2) {
    if ($i + 1 -ge $files.Count) { break }
    $a = Measure-Picture $files[$i].FullName      # first of the pair
    $b = Measure-Picture $files[$i + 1].FullName  # second of the pair
    $even += $a; $odd += $b
}

function Mean($arr, $prop) { ($arr | Measure-Object $prop -Average).Average }

Write-Host ""
Write-Host "== $Folder" -ForegroundColor Cyan
Write-Host ("  pairs: {0}" -f $even.Count)
foreach ($prop in @('luma', 'sat', 'grad')) {
    $e = Mean $even $prop; $o = Mean $odd $prop
    $pct = if ($e -ne 0) { 100.0 * ($o - $e) / $e } else { 0 }
    Write-Host ("  {0,-5} first {1:F5}   second {2:F5}   change {3:+0.00;-0.00;0.00}%" -f $prop, $e, $o, $pct)
}
Write-Host "  (odd pictures are the ones taken with the forced values, and are one frame later)"