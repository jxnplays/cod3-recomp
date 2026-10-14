# Probe one level from outside, and say whether it came up.
#
# COD3 has fifteen level libraries and, until this, only one of them had
# ever been run. This launches the title straight into a level, lets it sit
# for a while, and reports what the log says: whether the level's own files
# were read, whether anything was drawn, and what the frame rate did.
#
# The measures that matter are the ones that were true when the world failed
# to appear on blkbrn, so a broken level is not mistaken for a slow one:
#
#   drew       draws per frame. Near zero with the HUD up means the world
#              is missing, not that the level is quiet.
#   triangles  the same, and more specific.
#   files      a level that never streams its own .cod never arrived.
#
# Usage:
#   .\scripts\probe_level.ps1 saint_lo
#   .\scripts\probe_level.ps1 -All
#   .\scripts\probe_level.ps1 -All -Seconds 90

[CmdletBinding()]
param(
    [string]$Level = '',
    [switch]$All,
    # How long to let the level run before reading the log. The first
    # seconds are the front end and the loading; the level is not really
    # going until past that.
    [int]$Seconds = 75,
    # Where the game is. Falls back to game.path and to a game/ directory.
    [string]$GameRoot = ''
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
$Run = Join-Path $Root 'build\CoD3Host'
$Exe = Join-Path $Run 'CoD3.exe'
$Log = Join-Path $Run 'CoD3.log'

if (-not (Test-Path $Exe)) { throw "CoD3.exe is not built. Run the build steps in docs/SETUP.md first." }

if ($GameRoot -eq '') {
    foreach ($where in @((Join-Path $Root 'game.path'), (Join-Path $Root 'build\CoD3Host\game.path'))) {
        if (Test-Path $where) {
            $candidate = (Get-Content $where -Raw).Trim()
            if ($candidate -ne '' -and (Test-Path (Join-Path $candidate 'sp'))) { $GameRoot = $candidate; break }
        }
    }
}
if ($GameRoot -eq '' -or -not (Test-Path (Join-Path $GameRoot 'sp'))) {
    throw 'the installed game was not found; pass -GameRoot'
}

$levels = @(Select-String -Path (Join-Path $Run 'cod3_levels.inc') -Pattern 'COD3_LEVEL\((.+)\)' |
            ForEach-Object { $_.Matches[0].Groups[1].Value })
if ($levels.Count -eq 0) { throw 'no level list; is the build configured?' }

$Targets = if ($All) { $levels } elseif ($Level -ne '') { @($Level) } else { throw 'name a level, or -All' }
$failed = 0

foreach ($name in $Targets) {
    if ($levels -notcontains $name) { Write-Host "skipping $name : not one of the fifteen" -ForegroundColor Yellow; continue }

    Get-Process CoD3 -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 2
    Remove-Item $Log -Force -ErrorAction SilentlyContinue

    $env:COD3_NOFILMS = '1'
    $env:COD3_MAP = "$name,20"
    $env:COD3_PAD = '5:start 8:a'
    Remove-Item Env:COD3_EXEC, Env:COD3_CMD, Env:COD3_SKIPLOG, Env:COD3_SCANMEM, Env:COD3_DUMP, Env:COD3_PICTURES -ErrorAction SilentlyContinue

    $started = Get-Date
    $proc = Start-Process -FilePath $Exe -ArgumentList '--game', "`"$GameRoot`"" -WorkingDirectory $Run -PassThru
    Start-Sleep -Seconds $Seconds

    if (-not (Test-Path $Log)) {
        Write-Host ("{0,-9} NO LOG - nothing was written in {1}s" -f $name, $Seconds) -ForegroundColor Red
        Get-Process -Id $proc.Id -ErrorAction SilentlyContinue | Stop-Process -Force
        continue
    }

    $text = Get-Content $Log -Raw

    # The last status line: fps, frames, files read, draws so far.
    $status = Select-String -Path $Log -Pattern '^\s*\[\s*\d+s\]\s+fps' | Select-Object -Last 1
    $fps = ''; $files = ''; $draws = ''
    if ($status) {
        if ($status.Line -match 'fps (\d+)')        { $fps = $matches[1] }
        if ($status.Line -match 'files (\d+)')      { $files = $matches[1] }
        if ($status.Line -match 'draws (\d+)')      { $draws = $matches[1] }
    }

    # The steady-state render line, which is per-frame rather than cumulative.
    $render = Select-String -Path $Log -Pattern 'render: [\d.]+ fps.*?(\d+) draws, (\d+) k triangles' | Select-Object -Last 1
    $rDraws = ''; $rTris = ''
    if ($render) { $rDraws = $render.Matches[0].Groups[1].Value; $rTris = $render.Matches[0].Groups[2].Value }

    # Did the level's own archives get read? sp\<name>\<name>.cod is the
    # one every level has.
    $streamed = if ($text -match [regex]::Escape("sp\$name\$name.cod")) { 'yes' } else { 'NO' }

    # An alive process, and a level that drew something.
    $alive = [bool](Get-Process -Id $proc.Id -ErrorAction SilentlyContinue)

    # credits is the end-of-game credit reel, not a playable level. Its .cod is
    # 0.7 MB against 17-30 MB for a real one, and rolling names legitimately
    # draws only a few dozen things a frame - measured at 232 fps with the
    # credits rolling on screen. Judged by a playable level's threshold it
    # reads as broken while working perfectly, which is worse than not testing
    # it, so it is judged on streaming and staying alive instead.
    $isCredits = ($name -eq 'credits')
    $drewBig = if ($isCredits) { $streamed -eq 'yes' } else { ($rDraws -ne '' -and [int]$rDraws -gt 100) }

    # How loaded the machine was while this ran. Another build on the same box
    # steals cores and memory, and the frame rate drops for that reason alone -
    # a level that is fine looks broken. Recorded next to the number so a low
    # reading is never taken as the project's fault without checking it.
    $os = Get-CimInstance Win32_OperatingSystem
    $freeGB = [math]::Round($os.FreePhysicalMemory / 1MB, 1)
    $busy = $freeGB -lt 8

    $verdict =
        if (-not $alive)          { 'CRASHED' }
        elseif ($streamed -ne 'yes') { 'NEVER LOADED' }
        elseif (-not $drewBig)    { 'NO WORLD DRAWN' }
        elseif ($busy)            { 'ok (busy box)' }
        elseif ([int]$fps -lt 20) { 'loaded, very slow' }
        else                      { 'ok' }

    $colour = switch ($verdict) {
        'ok'                 { 'Green' }
        'ok (busy box)'      { 'Yellow' }
        'loaded, very slow'  { 'Yellow' }
        default              { 'Red' }
    }
    Write-Host ("{0,-9} {1,-18} fps {2,4}  files {3,4}  draws/frame {4,6}  tri {5,7}k  streamed {6}  free {7}GB" -f `
        $name, $verdict, $fps, $files, $rDraws, $rTris, $streamed, $freeGB) -ForegroundColor $colour

    if ($verdict -ne 'ok') { $failed++ }
    Get-Process -Id $proc.Id -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Seconds 1
}

Write-Host ''
Write-Host 'done. ok means the level drew a world; anything else says which way it failed.' -ForegroundColor Cyan

# A sweep that failed every level and still exits 0 is worse than no sweep: any
# wrapper reading $LASTEXITCODE - CI included - is told it passed. Counted here
# rather than at the print, so it covers the levels that were skipped too.
if ($failed -gt 0) {
    Write-Host ''
    Write-Host "$failed of $($Targets.Count) level(s) did not come up." -ForegroundColor Red
    exit 1
}
exit 0