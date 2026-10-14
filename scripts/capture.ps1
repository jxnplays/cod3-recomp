# Capture a level's picture with scripted screenshots, so the present path can
# be looked at without anybody pressing a key.
#
#   .\scripts\capture.ps1 -Level saint_lo -Seconds 70 -Every 900
#
# Writes PNGs beside the executable and says where they went. It reports only
# what it can see - that a file exists, how big it is, and whether the picture
# is a single flat colour - because deciding whether a picture looks right is
# an eye's job and not this script's.

param(
    [string]$Level = 'saint_lo',
    [int]$Seconds = 70,
    [int]$Every = 0,
    [int]$At = 0,
    [string]$Alt = '',
    [string]$GameRoot = '',
    [switch]$NoWait
)

$Root = Split-Path -Parent $PSScriptRoot
$Run = Join-Path $Root 'build\CoD3Host'
$Exe = Join-Path $Run 'CoD3.exe'

if (-not (Test-Path -LiteralPath $Exe)) { throw "CoD3.exe is not built: $Exe" }

if ($GameRoot -eq '') {
    foreach ($where in @((Join-Path $Root 'game.path'), (Join-Path $Run 'game.path'))) {
        if (Test-Path $where) {
            $candidate = (Get-Content $where -Raw).Trim()
            if ($candidate -ne '' -and (Test-Path (Join-Path $candidate 'sp'))) { $GameRoot = $candidate; break }
        }
    }
}
if ($GameRoot -eq '' -or -not (Test-Path (Join-Path $GameRoot 'sp'))) {
    throw 'the installed game was not found; pass -GameRoot'
}

Get-Process CoD3 -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 2

$shots = Join-Path $Run 'screenshots'
Remove-Item $shots -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item (Join-Path $Run 'CoD3.log') -Force -ErrorAction SilentlyContinue

$env:COD3_NOFILMS = '1'
$env:COD3_MAP = "$Level,20"
$env:COD3_PAD = '5:start 8:a'
$env:COD3_LOG = 'always'
# The two the host reads for scripted captures. Absent means no capture.
if ($Every -gt 0) { $env:COD3_SHOT_EVERY = "$Every" } else { Remove-Item Env:COD3_SHOT_EVERY -ErrorAction SilentlyContinue }
if ($At -gt 0) { $env:COD3_SHOT_AT = "$At" } else { Remove-Item Env:COD3_SHOT_AT -ErrorAction SilentlyContinue }
# -Alt makes consecutive captures alternate between the settings and this
# "contrast,saturation,temperature,brightness,sharpen" list, so two pictures
# one frame apart differ only by the feature. Comparing two separate runs
# cannot do that: the camera is somewhere different each time.
if ($Alt -ne '') { $env:COD3_SHOT_ALT = $Alt } else { Remove-Item Env:COD3_SHOT_ALT -ErrorAction SilentlyContinue }

Write-Host "booting $Level for $Seconds s" -ForegroundColor Cyan
$proc = Start-Process -FilePath $Exe -ArgumentList '--game', "`"$GameRoot`"" -WorkingDirectory $Run -PassThru

if ($NoWait) {
    Write-Host "started; pid $($proc.Id). Captures land in $shots"
    return
}

try { Wait-Process -Id $proc.Id -Timeout $Seconds -ErrorAction Stop }
catch { Get-Process -Id $proc.Id -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue }
Start-Sleep -Seconds 2

$log = Join-Path $Run 'CoD3.log'
if (Test-Path $log) {
    Get-Content $log | Select-String -Pattern 'screenshot:' | ForEach-Object { Write-Host "  $($_.Line.Trim())" }
    $faults = @(Get-Content $log | Select-String -Pattern 'guest fault|unhandled exception' -CaseSensitive:$false)
    Write-Host "  guest faults: $($faults.Count)"
}

# These exit non-zero rather than returning quietly.
#
# A caller checking $LASTEXITCODE used to get 0 after a run that measured
# nothing at all - no directory, or no pictures in it. That is the worst kind
# of wrong answer this repository has: a check that reports success because it
# did not run. Any wrapper around a sweep, including CI, would have read it as
# a pass.
if (-not (Test-Path $shots)) { Write-Host "no screenshots directory was made" -ForegroundColor Red; exit 1 }

$files = @(Get-ChildItem $shots -Filter *.png | Sort-Object Name)
if ($files.Count -eq 0) { Write-Host "no pictures were written" -ForegroundColor Red; exit 1 }

Add-Type -AssemblyName System.Drawing
foreach ($f in $files) {
    $bmp = [System.Drawing.Bitmap]::FromFile($f.FullName)
    try {
        # A flat picture is how a broken present path looks, so say so rather
        # than leaving a black PNG to be discovered by eye.
        $first = $bmp.GetPixel(0, 0)
        $flat = $true
        $sum = 0L
        $n = 0
        for ($y = 0; $y -lt $bmp.Height; $y += 16) {
            for ($x = 0; $x -lt $bmp.Width; $x += 16) {
                $c = $bmp.GetPixel($x, $y)
                if ($c.R -ne $first.R -or $c.G -ne $first.G -or $c.B -ne $first.B) { $flat = $false }
                $sum += $c.R + $c.G + $c.B
                $n += 3
            }
        }
        $mean = if ($n) { [math]::Round($sum / $n, 1) } else { 0 }
        $note = if ($flat) { 'FLAT - one colour only, the present path produced nothing' } else { 'has variation' }
        Write-Host ("  {0}  {1}x{2}  {3:N0} KB  mean {4}  {5}" -f $f.Name, $bmp.Width, $bmp.Height, ($f.Length / 1KB), $mean, $note)
    } finally { $bmp.Dispose() }
}
Write-Host "pictures in $shots"

# And a picture that is one flat colour is a broken present path, not a result.
$flat = 0
foreach ($f in $files) {
    $bmp = [System.Drawing.Bitmap]::FromFile($f.FullName)
    try {
        $first = $bmp.GetPixel(0, 0)
        $same = $true
        for ($y = 0; $y -lt $bmp.Height - 32; $y += 32) {
            for ($x = 0; $x -lt $bmp.Width - 32; $x += 32) {
                $c = $bmp.GetPixel($x, $y)
                if ($c.R -ne $first.R -or $c.G -ne $first.G -or $c.B -ne $first.B) { $same = $false; break }
            }
            if (-not $same) { break }
        }
        if ($same) { $flat++ }
    } finally { $bmp.Dispose() }
}
if ($flat -eq $files.Count) {
    Write-Host "every picture is a single flat colour - the present path produced nothing" -ForegroundColor Red
    exit 1
}
exit 0
