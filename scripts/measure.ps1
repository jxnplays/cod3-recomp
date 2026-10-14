# Measure pictures, so "it looks different" can be replaced by a number.
#
#   .\scripts\measure.ps1 <folder> [<folder> ...]
#
# Reports, per picture:
#   luma    mean brightness
#   sat     mean distance of a pixel from its own luma, 0..1  (0 is pure grey)
#   grad    mean absolute gradient between neighbouring pixels (high frequency
#           energy - this is what sharpening raises and what grading leaves
#           alone)
#
# Comparing the columns across two folders answers a question that looking at
# two pictures cannot: whether a change did what it was supposed to do to the
# image, as opposed to whether the two images merely look different.

param([Parameter(Mandatory = $true)][string[]]$Folders, [int]$Step = 3)

Add-Type -AssemblyName System.Drawing

foreach ($folder in $Folders) {
    if (-not (Test-Path -LiteralPath $folder)) { Write-Host "missing: $folder" -ForegroundColor Yellow; continue }
    Write-Host ""
    Write-Host "== $folder" -ForegroundColor Cyan
    $rows = @()
    foreach ($f in (Get-ChildItem -LiteralPath $folder -Filter *.png | Sort-Object Name)) {
        $bmp = [System.Drawing.Bitmap]::FromFile($f.FullName)
        try {
            $n = 0; $luma = 0.0; $sat = 0.0; $grad = 0.0; $gn = 0
            for ($y = 0; $y -lt $bmp.Height - $Step; $y += $Step) {
                for ($x = 0; $x -lt $bmp.Width - $Step; $x += $Step) {
                    $a = $bmp.GetPixel($x, $y)
                    $b = $bmp.GetPixel($x + $Step, $y)
                    $r = $a.R / 255.0; $g = $a.G / 255.0; $bl = $a.B / 255.0
                    $l = 0.299 * $r + 0.587 * $g + 0.114 * $bl
                    $luma += $l; $n++
                    # Saturation as an actual measurement of the shipped meaning:
                    # how far the pixel sits from its own luma.
                    $sat += ([math]::Abs($r - $l) + [math]::Abs($g - $l) + [math]::Abs($bl - $l)) / 3.0
                    # High frequency energy, from the green channel only.
                    $grad += [math]::Abs($g - $b.G / 255.0)
                    $gn++
                }
            }
            if ($n -gt 0) {
                $rows += [pscustomobject]@{
                    Picture = $f.Name
                    luma    = [math]::Round($luma / $n, 4)
                    sat     = [math]::Round($sat / $n, 4)
                    grad    = [math]::Round($grad / $gn, 5)
                }
            }
        } finally { $bmp.Dispose() }
    }
    $rows | Format-Table -AutoSize | Out-String | Write-Host
    if ($rows.Count -gt 0) {
        $meanL = ($rows | Measure-Object luma -Average).Average
        $meanS = ($rows | Measure-Object sat -Average).Average
        $meanG = ($rows | Measure-Object grad -Average).Average
        Write-Host ("  mean over {0} pictures:  luma {1}  sat {2}  grad {3}" -f $rows.Count, [math]::Round($meanL,4), [math]::Round($meanS,4), [math]::Round($meanG,5))
    }
}