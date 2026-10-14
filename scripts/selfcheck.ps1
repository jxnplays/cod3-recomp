<#
.SYNOPSIS
    Checks the changes in this repository, in the order that catches most.

.DESCRIPTION
    Written after a week in which four separate changes were shipped as
    "verified" and were wrong:

      - the .xex suffix test compared 11 characters against a 4-character
        string, so it never matched and the multiplayer relaunch stayed broken
      - the sensitivity curve was changed across its whole range to raise its
        top, which is what the aiming jank turned out to be
      - replacing memcmp with a field-by-field list silently dropped nine
        settings
      - XGetVideoMode was left reporting 1280x720 while the other two places
        that report a mode were changed, so the ultrawide feature had drifted
        exactly as the change claimed to prevent

    Every one of those was found by reading, or by a person playing the game.
    None was found by the build passing, because none of them stopped the
    build.

    So this checks the things a passing build does not: that the logic is what
    it claims, that the defaults have not moved, and that every field the
    settings menu can edit is compared when Apply decides whether it is dirty.

    The checks here need no game data and take under a second, because they
    run often. The game checks are in -Game and take a couple of minutes.

.PARAMETER Game
    Boot the game on the default path and check three things: the process is
    still running after 75 seconds, it took no guest fault, and ps2 went out
    exactly once. It does NOT check that anything was drawn, or how fast -
    see docs/PROCESS.md. Needs the game installed.

.PARAMETER SkipBuild
    Do not rebuild first. Useful when iterating on the checks themselves.

.EXAMPLE
    .\scripts\selfcheck.ps1

.EXAMPLE
    .\scripts\selfcheck.ps1 -Game
#>
[CmdletBinding()]
param(
    [switch]$Game,
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
$Src = Join-Path $Root 'CoD3Host'

$script:Failures = 0
$script:Checks = 0

# clang-cl is not on PATH in a normal shell, and this script must not have to
# be told where it is: a check that silently cannot run is worse than no
# check, because it looks like it passed. Located, not guessed.
$clang = if (Get-Command clang-cl -ErrorAction SilentlyContinue) {
    (Get-Command clang-cl).Source
} else {
    # Each root is searched literally. A wildcard like 'C:\Program Files*\LLVM'
    # finds nothing on a machine where LLVM is under "Program Files (x86)": the
    # * matches the directory name including its brackets and PowerShell then
    # reads "(x86)" as grouping syntax. That is the same trap the CI job fell
    # into, and it is why this locates rather than assumes.
    $found = ''
    foreach ($clangRoot in @($env:ProgramFiles, ${env:ProgramFiles(x86)})) {
        if (-not $clangRoot) { continue }
        $dir = Join-Path $clangRoot 'LLVM\bin'
        if (Test-Path -LiteralPath (Join-Path $dir 'clang-cl.exe')) { $found = Join-Path $dir 'clang-cl.exe'; break }
    }
    $found
}

function Pass([string]$what) {
    $script:Checks++
    Write-Host "  ok    $what" -ForegroundColor Green
}
function Fail([string]$what, [string]$why) {
    $script:Checks++
    $script:Failures++
    Write-Host "  FAIL  $what" -ForegroundColor Red
    Write-Host "          $why" -ForegroundColor DarkRed
}
function Section([string]$name) { Write-Host "`n$name" -ForegroundColor Cyan }

# ---------------------------------------------------------------------------
Section 'Logic, run for real'

# Both of these compile CoD3Host/logic.h - the header the host itself uses -
# and run it. An earlier version pasted a paraphrase of the logic into a
# here-string and ran that, so reverting the bug in installer.cpp or
# input.cpp changed nothing and every check still passed. A check that cannot
# fail on the regression it names is worse than no check, because it reports
# the regression as covered.
$tmp = Join-Path ([IO.Path]::GetTempPath()) 'cod3_selfcheck'
# Cleared first, and a leftover binary is never trusted: a stale one turned
# "could not compile" into "passed", which is the exact failure this script
# exists to prevent.
if (Test-Path -LiteralPath $tmp) {
    Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
}
New-Item -ItemType Directory -Force -Path $tmp | Out-Null

if (-not $clang) {
    Fail 'the shared logic could not be run' 'clang-cl was not found; the compiled checks were SKIPPED, not passed'
} else {
    $cpp = Join-Path $tmp 'logic_test.cpp'
    $outExe = Join-Path $tmp 'logic_test.exe'
    $body = @'
#include "logic.h"
#include <cstdio>
int main() {
    int bad = 0;
    struct { const char* name; bool want; } cases[] = {
        {"d:\\codmp_xenonf.xex", true},   // what Multiplayer asks for
        {"d:\\default.xex",      true},   // the next mission
        {"game:\\default.xex",   true},
        {".xex",                 true},
        {"D:\\CODMP.XEX",        true},   // case does not matter
        {"xex",                  false},
        {"default.xex.bak",      false},
        {"d:\\media\\a.wmv",      false},
        {"",                     false},  // the dashboard
    };
    for (auto& c : cases) {
        const bool got = HostLogic::IsBootableXex(c.name);
        if (got != c.want) {
            printf("  xex %s: got %d want %d\n", c.name[0] ? c.name : "(empty)", got, c.want);
            bad++;
        }
    }
    // Bit-identical to the old linear gain below 3.0, which is the whole
    // point: the first attempt was a plain square root and changed every
    // setting below the top it was meant to affect.
    for (int i = 1; i <= 30; i++) {
        const double s = i / 10.0;
        if (s > 3.0) break;
        if (HostLogic::MouseGain(s) != 1300.0 * s) {
            printf("  gain %.2f: %.4f, expected %.4f\n", s, HostLogic::MouseGain(s), 1300.0 * s);
            bad++;
        }
    }
    if (!(HostLogic::MouseGain(10.0) > 1300.0 * 3.0)) { printf("  gain 10.0 is not above 3.0\n"); bad++; }
    if (!(HostLogic::MouseGain(10.0) < 1300.0 * 10.0)) { printf("  gain 10.0 is not below the clipping linear gain\n"); bad++; }
    return bad;
}
'@
    [IO.File]::WriteAllText($cpp, $body)
    & $clang /nologo /std:c++17 "/Fe:$outExe" "/I$Src" $cpp /link /nologo 2>&1 | Out-Null
    if (-not (Test-Path -LiteralPath $outExe)) {
        Fail 'the shared logic could not be run' 'it did not compile, so the compiled checks were SKIPPED, not passed'
    } else {
        $logicOut = & $outExe 2>&1
        if ($LASTEXITCODE -eq 0) { Pass 'the .xex suffix test and the sensitivity gain, run against the header the host uses' }
        else { Fail 'the shared logic is wrong' ($logicOut -join '; ') }
    }
}

# ---------------------------------------------------------------------------
Section 'Settings: what the menu edits, Apply must compare'

# memcmp over the whole struct used to be the dirty check. It compared
# padding bytes, which made Apply spuriously clickable, so it was replaced
# with a field list - and the list was missing nine fields, so dragging any of
# them left Apply greyed out and threw the edit away. A field list is only
# correct if it is complete, and nothing but this check notices.
$overlay = Get-Content (Join-Path $Src 'overlay.cpp') -Raw
$edited = [regex]::Matches($overlay, 'g_edit\.(\w+)') |
          ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique
$compared = [regex]::Matches($overlay, 'a\.(\w+) != b\.\1(?!\w)') |
            ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique
$missing = @($edited | Where-Object { $_ -notin $compared -and $_ -ne 'keys' })
if ($missing.Count -eq 0) {
    Pass "Apply compares all $($edited.Count) fields the menu edits"
} else {
    Fail 'Apply drops settings' ("not in the dirty check: " + ($missing -join ', ') +
        " - drag one and Apply stays greyed out and the edit is discarded")
}

# ---------------------------------------------------------------------------
Section 'One answer for the video mode, everywhere'

# Three places report a mode to the title. Two were merged into one function
# and the third was left behind at 1280x720, so with a wider window the title
# was told two different things - which is the drift the merge was meant to
# stop.
$videoMode = 0
foreach ($f in @('kernel_video.cpp', 'kernel_system.cpp')) {
    $text = Get-Content (Join-Path $Src $f) -Raw
    $videoMode += ([regex]::Matches($text, 'Render::VideoMode')).Count
}
$hardcoded = 0
foreach ($f in @('kernel_video.cpp', 'kernel_system.cpp')) {
    $text = Get-Content (Join-Path $Src $f) -Raw
    $hardcoded += ([regex]::Matches($text, 'Write32\(base, mode \+ 0x00,\s*1280')).Count
}
if ($videoMode -ge 3 -and $hardcoded -eq 0) {
    Pass 'all three mode reporters go through Render::VideoMode, none hardcodes 1280'
} else {
    Fail 'the video mode has drifted' "$videoMode calls to Render::VideoMode (want 3), $hardcoded hardcoded 1280 widths (want 0)"
}

# The present path has to floor the aspect at sixteen by nine, or a window
# taller than that gets the title's 16:9 frame stretched into it - about
# eleven percent on a 16:10 window.
$render = Get-Content (Join-Path $Src 'render_d3d11.cpp') -Raw
if ($render -match 'std::max\(float\(clientWidth\) / float\(clientHeight\), 16\.0f / 9\.0f\)') {
    Pass 'the present path floors the aspect at 16:9, so taller windows are not stretched'
} else {
    Fail 'the present path can stretch a taller-than-16:9 window' 'no 16:9 floor on the present-path aspect'
}

# ---------------------------------------------------------------------------
Section 'The ini round trip'

# A %d with no matching argument is undefined behaviour and prints garbage or
# crashes. One mismatch was introduced by a key added to one half of the
# save only.
$settings = Get-Content (Join-Path $Src 'settings.cpp') -Raw
$fprintf = [regex]::Matches($settings, 'fprintf\(file,\s*"((?:[^"\\]|\\.)*)"((?:[^;])*?)\);', 'Singleline')
$bad = @()
foreach ($m in $fprintf) {
    $fmt = $m.Groups[1].Value
    $rest = $m.Groups[2].Value
    $specs = ([regex]::Matches($fmt, '%[-+ #0-9.]*(d|s|f|lu|llu|zu)')).Count
    # arguments are the comma-separated names after the string literal
    $args = ($rest -split ',') | Where-Object { $_ -match '\b(v\.|resolution|ActionKeys|i\b)' }
    if ($specs -gt 0 -and $args.Count -lt $specs) {
        $bad += "$specs conversion(s) but $($args.Count) argument(s)"
    }
}
if ($bad.Count -eq 0) {
    Pass 'every fprintf format has an argument for each conversion'
} else {
    Fail 'an fprintf has more conversions than arguments' ($bad -join '; ')
}

# Every key written must also be read back, or a setting silently does
# nothing on the next run.
$written = [regex]::Matches($settings, '\\n([a-z_]+) = ') | ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique
$read = [regex]::Matches($settings, 'key == "([a-z_]+)"') | ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique
$orphan = @($written | Where-Object { $_ -notin $read })
if ($orphan.Count -eq 0) {
    Pass 'every key written to the ini is read back'
} else {
    Fail 'the ini writes keys nothing reads' ($orphan -join ', ')
}

# ---------------------------------------------------------------------------
Section 'Defaults'

# Every feature added this week is opt-in, and the default has to be exactly
# what it was before any of them. A feature that changes the default is a
# regression nobody was told about.
if ($settings -match 'if \(v\.aspectMode < 0 \|\| v\.aspectMode > 1\) v\.aspectMode = 0;') { Pass 'aspect defaults to letterboxed 16:9' }
else { Fail 'aspect default' 'aspectMode is not clamped to 0 on load' }
if ($settings -match 'if \(v\.fov < 65 \|\| v\.fov > 180\) v\.fov = 65;') { Pass 'field of view defaults to the title own 65' }
else { Fail 'fov default' 'the fov range check is not 65..180' }
if ($settings -match 'mouseSensitivity >= 0\.1f && v\.mouseSensitivity <= 10\.0f') { Pass 'sensitivity range is 0.1 to 10' }
else { Fail 'sensitivity range' 'the check is not 0.1..10' }

# ps2 is sent once per process, not once per frame: TitleCommandsChanged runs
# every frame, and an unguarded append put the command on the title's buffer
# hundreds of times in two minutes.
if ($settings -match 'static bool sentPrompts = false') { Pass 'the ps2 command is sent once per process' }
else { Fail 'ps2 is sent repeatedly' 'no once-per-process guard in TitleCommandsChanged' }

# ---------------------------------------------------------------------------
Section 'Documentation must not claim more than was measured'

# Every claim here that was wrong was a documentation claim, not a code one:
# "the prompts are switched with ps2" in two files and "the title ships both
# sets in its font data" in a comment. There is one button font on the disc
# and no PlayStation counterpart.
$docs = Join-Path $Root 'docs'
$overclaiming = @()
foreach ($f in Get-ChildItem $docs -Filter *.md) {
    $t = Get-Content $f.FullName -Raw
    if ($t -match 'switched with the `?ps2`? variable') { $overclaiming += "$($f.Name): claims ps2 switches the prompts" }
}
$overlayText = $overlay
# Normalised first. The phrase was wrapped across three comment lines, so a
# single-line match could never fire - the check was reporting a pass on a
# branch that was unreachable.
$flatOverlay = ($overlayText -replace '(?m)^\s*//\s?', ' ') -replace '\s+', ' '
if ($flatOverlay -match 'the title ships both sets in its font data') { $overclaiming += 'overlay.cpp: claims the title ships both glyph sets' }
if ($overclaiming.Count -eq 0) { Pass 'no document claims a feature is working when it is not' }
else { Fail 'documentation overclaims' ($overclaiming -join '; ') }

# A link to something that was deleted is the same class of problem.
$broken = @()
$mdFiles = @(Get-ChildItem $Root -Filter *.md) + @(Get-ChildItem $docs -Filter *.md)
foreach ($f in $mdFiles) {
    $t = Get-Content $f.FullName -Raw
    foreach ($m in [regex]::Matches($t, '\]\(([^)#]+\.(?:md|png|txt))\)')) {
        $target = Join-Path $f.DirectoryName $m.Groups[1].Value
        if (-not (Test-Path $target)) { $broken += "$($f.Name) -> $($m.Groups[1].Value)" }
    }
}
if ($broken.Count -eq 0) { Pass 'every relative link in the documentation resolves' }
else { Fail 'broken documentation links' ($broken -join '; ') }

# ---------------------------------------------------------------------------
if (-not $SkipBuild) {
    Section 'Build'
    # Literal roots, not a 'C:\Program Files*\...' wildcard. That form finds
    # nothing when Visual Studio is under "Program Files (x86)": the * matches
    # the directory name including its brackets and PowerShell reads "(x86)" as
    # grouping syntax. Same trap the CI job fell into, and the same one this
    # file explains against at the clang locator above.
    $bt = ''
    foreach ($vsRoot in @($env:ProgramFiles, ${env:ProgramFiles(x86)})) {
        if (-not $vsRoot) { continue }
        $vsBase = Join-Path $vsRoot 'Microsoft Visual Studio'
        if (-not (Test-Path -LiteralPath $vsBase)) { continue }
        $found = @(Get-ChildItem -LiteralPath $vsBase -Filter 'vcvars64.bat' -Recurse -File -ErrorAction SilentlyContinue)
        if ($found.Count -gt 0) { $bt = $found[0].FullName; break }
    }
    if (-not $bt) {
        Fail 'build' 'vcvars64.bat not found'
    } else {
        $cmakeDir = Join-Path (Split-Path (Split-Path (Split-Path (Split-Path $bt)))) 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin'
        $ninjaDir = Join-Path (Split-Path (Split-Path (Split-Path (Split-Path $bt)))) 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja'
        $inner = 'cmake --build build --parallel 3 --target CoD3'
        $out = cmd /c "`"$bt`" >nul 2>&1 && set PATH=C:\PROGRA~1\LLVM\bin;$cmakeDir;$ninjaDir;%PATH% && $inner" 2>&1
        if ($LASTEXITCODE -eq 0) { Pass 'the host builds' }
        else {
            $first = ($out | Select-String -Pattern 'error' | Select-Object -First 3) -join ' | '
            Fail 'the host does not build' "$first"
        }
    }
}

# ---------------------------------------------------------------------------
if ($Game) {
    Section 'The default path, run'
    $run = Join-Path $Root 'build\CoD3Host'
    $exe = Join-Path $run 'CoD3.exe'
    if (-not (Test-Path $exe)) { Fail 'the game check' 'CoD3.exe is not built' }
    else {
        # The ini is put back to defaults first: a feature is only safe if the
        # default is unchanged, and a machine left on a test setting cannot
        # show that.
        $ini = Join-Path $run 'CoD3Recomp.ini'
        if (Test-Path $ini) {
            $t = Get-Content $ini -Raw
            $t = $t -replace 'aspect_mode = \d+', 'aspect_mode = 0'
            $t = $t -replace 'fov = \d+', 'fov = 65'
            $t = $t -replace 'mouse_sensitivity = [\d.]+', 'mouse_sensitivity = 1.00'
            [IO.File]::WriteAllText($ini, $t)
        }
        $log = Join-Path $run 'CoD3.log'
        Remove-Item $log -Force -ErrorAction SilentlyContinue
        $gamePath = Join-Path $Root 'build\CoD3Host\game.path'
        $game = if (Test-Path $gamePath) { (Get-Content $gamePath -Raw).Trim() } else { '' }
        if ($game -eq '' -or -not (Test-Path (Join-Path $game 'default.xex'))) {
            Fail 'the game check' 'the game is not installed, or game.path does not name it'
        } else {
            $env:COD3_LOG = 'always'
            $env:COD3_NOFILMS = '1'
            Remove-Item Env:COD3_MAP, Env:COD3_CMD, Env:COD3_PAD, Env:COD3_XEX -ErrorAction SilentlyContinue
            $proc = Start-Process -FilePath $exe -ArgumentList '--game', "`"$game`"" -WorkingDirectory $run -PassThru
            Start-Sleep -Seconds 75
            $running = [bool](Get-Process -Id $proc.Id -ErrorAction SilentlyContinue)
            if (-not $running) {
                Fail 'the game check' 'the client exited within 75 seconds'
            } else {
                Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
                Start-Sleep -Seconds 2
            }
            $faults = if (Test-Path $log) { (Select-String -Path $log -Pattern 'Guest fault' | Measure-Object).Count } else { -1 }
            $ps2 = if (Test-Path $log) { (Select-String -Path $log -Pattern 'set ps2' | Measure-Object).Count } else { -1 }
            if ($faults -eq 0) { Pass 'the client ran with no guest fault' }
            else { Fail 'the client took a guest fault' "$faults fault lines" }
            if ($ps2 -eq 1) { Pass 'the ps2 command went out exactly once' }
            else { Fail 'ps2 was sent $ps2 times, expected once' }
        }
    }
}

Write-Host ''
if ($script:Failures -eq 0) {
    Write-Host "$($script:Checks) checks, all passed." -ForegroundColor Green

    # The verdict, written where it cannot be forgotten and cannot be edited
    # by whoever wrote the change.
    #
    # MEASURED / REVIEWED / REASONED is worthless as a thing the author writes
    # about their own work: that is self-assessment by the person whose failure
    # mode was self-assessment, and it had already failed to appear once in
    # thirty commits. So a green run writes the evidence itself - how many
    # checks, when, and with the tree's commit - and the commit message is
    # expected to quote it rather than assert it.
    $stamp = Join-Path $Root '.git\SELFCHECK'
    $head = (& git -C $Root rev-parse --short HEAD 2>$null)
    if (-not $head) { $head = 'uncommitted' }
    @(
        "MEASURED: $head"
        "  checks:  $script:Checks, all passed"
        "  at:      $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"
        "  game:    $(if ($Game) { 'run on the default path' } else { 'not run - logic and defaults only' })"
    ) | Set-Content -LiteralPath $stamp -Encoding UTF8
    Write-Host "  wrote .git\SELFCHECK" -ForegroundColor DarkGray
    exit 0
}
Write-Host "$($script:Checks) checks, $($script:Failures) failed." -ForegroundColor Red
if (Test-Path -LiteralPath (Join-Path $Root '.git\SELFCHECK')) {
    Remove-Item -LiteralPath (Join-Path $Root '.git\SELFCHECK') -Force -ErrorAction SilentlyContinue
}
exit 1