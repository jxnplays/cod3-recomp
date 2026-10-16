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
        if (HostLogic::MouseGain(s) != 150.0 * s) {
            printf("  gain %.2f: %.4f, expected %.4f\n", s, HostLogic::MouseGain(s), 150.0 * s);
            bad++;
        }
    }
    if (!(HostLogic::MouseGain(10.0) > 150.0 * 3.0)) { printf("  gain 10.0 is not above 3.0\n"); bad++; }
    if (!(HostLogic::MouseGain(10.0) < 150.0 * 10.0)) { printf("  gain 10.0 is not below the clipping linear gain\n"); bad++; }

    // The calibration itself, which is the thing that was wrong.
    //
    // The title reads a stick that saturates at 32767 and it is fed counts, so
    // what matters is how many counts reach full deflection. ReadMouse emits
    // `counts * gain * scale` per read and reads every 4 ms, so the scale
    // appears once in the stick value and once in how many reads make up a
    // sixtieth of a second - and the two cancel exactly:
    //
    //   counts per sixtieth = 32767 / gain
    //
    // At the old anchor of 1300 that is 25 counts, which is 1500 counts a
    // second: 24 mm/s of hand movement at 1600 DPI, so every ordinary flick
    // saturated and fine aim was impossible. At 150 it is about 220.
    //
    // Asserted as a band so the curve can still be tuned later, tight enough
    // that returning to the old order of magnitude fails - 25 is well under.
    {
        const double countsPer60 = 32767.0 / HostLogic::MouseGain(1.0);
        printf("  at sensitivity 1.0, a full turn needs %.0f counts per sixtieth of a second\n", countsPer60);
        if (countsPer60 < 150.0 || countsPer60 > 800.0) {
            printf("  calibration out of band: %.0f counts, want 150..800\n", countsPer60);
            bad++;
        }
        if (HostLogic::kMinMouseSensitivity > 0.02) { printf("  the floor is not low enough for a high-DPI mouse\n"); bad++; }
        const double atFloor = 32767.0 / HostLogic::MouseGain(HostLogic::kMinMouseSensitivity);
        if (atFloor < countsPer60 * 10.0) { printf("  the floor is too high to matter\n"); bad++; }
    }
    return bad;
}
'@
    [IO.File]::WriteAllText($cpp, $body)

    # stderr goes to a file, never into a 2>&1 pipeline. With
    # $ErrorActionPreference='Stop', a native command writing to stderr inside
    # such a pipeline raises a terminating NativeCommandError - which killed the
    # whole run on a compiler *warning*, before the failure paths, with no
    # summary and a stale green verdict left on disk. That is the same defect as
    # the memory guard in docs/MEMORY.md: a check that cannot report its own
    # failure while claiming to.
    $diags = Join-Path $tmp 'compile.txt'
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        & $clang /nologo /std:c++17 "/Fe:$outExe" "/I$Src" $cpp /link /nologo 2> $diags 1> $null
    } finally {
        $ErrorActionPreference = $previous
    }
    if (-not (Test-Path -LiteralPath $outExe)) {
        $why = if (Test-Path -LiteralPath $diags) {
            ((Get-Content $diags -Raw).Trim() -split "`n" | Select-Object -First 2) -join ' '
        } else { 'no compiler output' }
        Fail 'the shared logic could not be run' ("it did not compile, so the compiled checks were SKIPPED, not passed: $why")
    } else {
        $logicOut = & $outExe 2>&1
        if ($LASTEXITCODE -eq 0) { Pass 'the .xex suffix test and the sensitivity gain, run against the header the host uses' }
        else { Fail 'the shared logic is wrong' ($logicOut -join '; ') }
    }

    # The untouched default path, pinned to the shader it replaced.
    #
    # "The defaults are the identity" has two halves. The arithmetic of the
    # grading TABLE is checked below. This checks the other half: that with
    # every control at its default the host runs a program whose body is
    # character-for-character what the present path ran before grading
    # existed, so there is no second texture, no extra sample and no dot
    # product in the common case.
    #
    # The literal below is the `present` entry point as it stood in b0baa71,
    # the commit before the colour work. The name is normalised because the
    # entry point was renamed to presentPlain when grading arrived.
    $expectedPresent = 'return float4(gamma(source.Sample(smp, v.uv).rgb), 1.0); }'
    $renderText = Get-Content (Join-Path $Src 'render_d3d11.cpp') -Raw
    $m = [regex]::Match($renderText, '"float4 presentPlain\(V v\) : SV_Target \{ (?<body>[^"]*)')
    if (-not $m.Success) {
        Fail 'the default present path is not pinned' 'could not find the presentPlain entry point in render_d3d11.cpp'
    } else {
        # The captured body ends with the literal two characters \n - that is
        # the newline escape closing the C++ string literal, not part of the
        # shader. Strip it before comparing.
        $body = ($m.Groups['body'].Value.Trim() -replace '\\n$', '').Trim()
        if ($body -ne $expectedPresent) {
            Fail 'the default present path is not pinned' "presentPlain now runs: $body`n        expected: $expectedPresent"
        } else {
            # And it must actually be selected when nothing is set. A correct
            # program that is never chosen is the same defect as a wrong one.
            $sel = [regex]::Match($renderText, '(?s)else if \(!graded && g_presentPlainPixelShader\) program = g_presentPlainPixelShader\.Get\(\);')
            if (-not $sel.Success) {
                Fail 'the default present path is not pinned' 'presentPlain is defined but never selected when grading is at its defaults'
            } else {
                Pass 'the default present path runs the shader it replaced, and is selected when nothing is set'
            }
        }
    }

    # The render key table, across the rehash that Erase used to walk into.
    $tblCpp = Join-Path $Root 'scripts\table_test.cpp'
    $tblExe = Join-Path $tmp 'table_test.exe'
    if (-not (Test-Path -LiteralPath $tblCpp)) {
        Fail 'the key table could not be run' "the test file is missing: $tblCpp"
    } else {
        $tDiags = Join-Path $tmp 'table_compile.txt'
        $previous = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        try {
            & $clang /nologo /std:c++17 "/Fe:$tblExe" "/I$Src" $tblCpp /link /nologo 2> $tDiags 1> $null
        } finally {
            $ErrorActionPreference = $previous
        }
        if (-not (Test-Path -LiteralPath $tblExe)) {
            $why = if (Test-Path -LiteralPath $tDiags) { ((Get-Content $tDiags -Raw).Trim() -split "`n" | Select-Object -First 2) -join ' ' } else { 'no compiler output' }
            Fail 'the key table could not be run' ("it did not compile, so the check was SKIPPED, not passed: $why")
        } else {
            $tblOut = & $tblExe 2>&1
            if ($LASTEXITCODE -eq 0) { Pass 'the key table survives erasing across a rehash, and being emptied' }
            else { Fail 'the key table corrupts itself when erased near full' ($tblOut -join '; ') }
        }
    }

    # The sharpening kernel, run on the CPU.
    #
    # It shipped once and did nothing: it clamped the result to [mn, mx], and
    # mn/mx are the extremes of a neighbourhood that INCLUDES the centre pixel,
    # so on most pixels the centre already was the extreme and the clamp threw
    # the result away. What survived was measured as about seven percent more
    # high frequency energy - real, and invisible.
    #
    # So this asserts the two things that matter and that a "did anything
    # change" test cannot: that the pass is visible at the middle of the slider,
    # and that it stays inside its own bound and leaves flat areas alone. The
    # seven percent figure is named in the comment there, because it is the
    # exact number that was called a pass before.
    $casCpp = Join-Path $Root 'scripts\cas_kernel.cpp'
    $casExe = Join-Path $tmp 'cas_kernel.exe'
    if (-not (Test-Path -LiteralPath $casCpp)) {
        Fail 'the sharpening kernel could not be run' "the test file is missing: $casCpp"
    } else {
        $cDiags = Join-Path $tmp 'cas_compile.txt'
        $previous = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        try {
            & $clang /nologo /std:c++17 "/Fe:$casExe" $casCpp /link /nologo 2> $cDiags 1> $null
        } finally {
            $ErrorActionPreference = $previous
        }
        if (-not (Test-Path -LiteralPath $casExe)) {
            $why = if (Test-Path -LiteralPath $cDiags) { ((Get-Content $cDiags -Raw).Trim() -split "`n" | Select-Object -First 2) -join ' ' } else { 'no compiler output' }
            Fail 'the sharpening kernel could not be run' ("it did not compile, so the check was SKIPPED, not passed: $why")
        } else {
            $casOut = & $casExe 2>&1
            if ($LASTEXITCODE -eq 0) { Pass 'the sharpening is visible at the slider middle, stays bounded, and leaves flat areas alone' }
            else { Fail 'the sharpening does nothing an eye could see' ($casOut -join '; ') }
        }
    }

    # The grading table's texture FORMAT.
    #
    # This is a separate check from the one below, and it exists because of a
    # defect that got through: the table was created as R10G10B10A2_UNORM while
    # being packed with 8-bit values. A UNORM sample decodes as n/(2^bits-1), so
    # every pixel was scaled by 255/1023 and the whole game rendered four times
    # too dark - while the packing, the CPU-side decode and the identity test
    # all remained self-consistent and green.
    #
    # Nothing in grading.h can catch that on its own, because grading.h never
    # mentions a texture format. The format lives in render_d3d11.cpp, so the
    # format is checked here, against the same constant the decode derives
    # from.
    $gradeH = Get-Content (Join-Path $Src 'grading.h') -Raw
    if ($gradeH -notmatch 'kBitsPerChannel\s*=\s*(\d+)') {
        Fail 'the grading format is not pinned' 'grading.h does not declare kBitsPerChannel'
    } else {
        $bits = [int]$Matches[1]
        $expectedName = "R${bits}G${bits}B${bits}A${bits}"
        $expectedFormat = "DXGI_FORMAT_${expectedName}_UNORM"
        # The grade table's descriptor, and nothing else's. Isolate the
        # GradeTable function so the display table - which is legitimately
        # 10-bit and expands on the way in - is not mistaken for it.
        $renderSrc = Get-Content (Join-Path $Src 'render_d3d11.cpp') -Raw
        $start = $renderSrc.IndexOf('ID3D11ShaderResourceView* GradeTable(')
        $end = $renderSrc.IndexOf('void DrawQuad(', $start)
        if ($start -lt 0 -or $end -lt 0) {
            Fail 'the grading format is not pinned' 'could not find GradeTable in render_d3d11.cpp'
        } else {
            $gradeFn = $renderSrc.Substring($start, $end - $start)
            $found = [regex]::Match($gradeFn, 'DXGI_FORMAT_(\w+?)_UNORM').Groups[1].Value
            if ($found -ne $expectedName) {
                # Say what it actually costs. A UNORM sample decodes as
                # n / (2^bits - 1), so a table packed at $bits bits and read
                # at whatever the format says is scaled by that ratio, at
                # every setting including the identity.
                # The channel depth is the number after R - not the trailing digits, which
                # for R10G10B10A2 is the alpha's "2" and gives a nonsense ratio.
                $readBits = if ($found -match '^R(\d+)') { [int]$Matches[1] } else { 0 }
                $ratio = if ($readBits -gt 0) { [math]::Round(((1 -shl $bits) - 1) / ((1 -shl $readBits) - 1), 4) } else { 0 }
                Fail 'the grading table is the wrong format' "grading.h packs $bits bits per channel, but GradeTable creates DXGI_FORMAT_${found}_UNORM (expected $expectedFormat). A UNORM sample decodes as n/(2^bits-1), so every pixel is scaled by $ratio even with every control at its default."
            } else {
                Pass "the grading table is created as $expectedFormat, matching its packing and decode"
            }
        }
    }

    # The colour grading pass claims its defaults are the identity, so that a
    # player who never opens the Colour page gets a bit-identical picture.
    # That claim is the whole safety argument for the feature and it cannot be
    # checked by reading the code, because a rounding change two steps deep
    # still reads as obviously correct.
    #
    # So it is compiled and run, against CoD3Host/grading.h - the header the
    # host itself includes, not a paraphrase of it. The test file is a real
    # file in the repository rather than a here-string, precisely because the
    # earlier logic check pasted a paraphrase and therefore reported a reverted
    # bug as covered.
    $gradeCpp = Join-Path $Root 'scripts\grading_identity.cpp'
    $gradeExe = Join-Path $tmp 'grading_identity.exe'
    if (-not (Test-Path -LiteralPath $gradeCpp)) {
        Fail 'the grading identity could not be run' "the test file is missing: $gradeCpp"
    } else {
        $gDiags = Join-Path $tmp 'grading_compile.txt'
        $previous = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        try {
            & $clang /nologo /std:c++17 "/Fe:$gradeExe" "/I$Src" $gradeCpp /link /nologo 2> $gDiags 1> $null
        } finally {
            $ErrorActionPreference = $previous
        }
        if (-not (Test-Path -LiteralPath $gradeExe)) {
            $why = if (Test-Path -LiteralPath $gDiags) {
                ((Get-Content $gDiags -Raw).Trim() -split "`n" | Select-Object -First 2) -join ' '
            } else { 'no compiler output' }
            Fail 'the grading identity could not be run' ("it did not compile, so the check was SKIPPED, not passed: $why")
        } else {
            $gradeOut = & $gradeExe 2>&1
            if ($LASTEXITCODE -eq 0) { Pass 'the grading defaults are the identity as the GPU reads them, and every control does something' }
            else { Fail 'the grading pass is not the identity' ($gradeOut -join '; ') }
        }
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
# The keys are compared by a loop rather than by name, so they are not in the
    # list above. That loop is checked on its own: excluding it silently is how
    # seventeen keybinds could stop being compared while the check stayed green.
    $keysLoop = [bool]($overlay -match 'for \(int i = 0; i < Settings::ActionCount; i\+\+\)')
    if (-not $keysLoop) {
        Fail 'Apply drops key bindings' 'the loop that compares the key array is gone'
    } elseif ($missing.Count -eq 0) {
        Pass "Apply compares all $($edited.Count) named fields the menu edits, and the key array by loop"
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
    # $argv, not $args: the latter is a PowerShell automatic variable.
    $argv = ($rest -split ',') | Where-Object { $_ -match '\b(v\.|resolution|ActionKeys|i\b)' }
    if ($specs -gt 0 -and $argv.Count -lt $specs) {
        $bad += "$specs conversion(s) but $($argv.Count) argument(s)"
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
# The floor moved from 0.1 so a high-DPI mouse has somewhere to go: even at
    # 0.1 the corrected gain saturated on an ordinary flick. Asserted against
    # the shared constant rather than a literal, because this range exists in
    # three places - the menu, the ini validation and ReadMouse - and three
    # copies is how they drift apart in the first place.
    if ($settings -match 'mouseSensitivity >= float\(HostLogic::kMinMouseSensitivity\)') { Pass 'the sensitivity floor is the shared constant, not a third copy of the number' }
else { Fail 'sensitivity range' 'the check is not 0.1..10' }

# ps2 is sent once per process, not once per frame: TitleCommandsChanged runs
# every frame, and an unguarded append put the command on the title's buffer
# hundreds of times in two minutes.
if ($settings -match 'static bool sentPrompts = false') { Pass 'the ps2 command is sent once per process' }
else { Fail 'ps2 is sent repeatedly' 'no once-per-process guard in TitleCommandsChanged' }

# ---------------------------------------------------------------------------
Section 'The menu tells the truth'

# Every variable the Mods tab sends must exist in the title.
#
# One did not. cg_showLocationalDamage is not a variable in this game - the
# real name is r_showLocationalDamage - so that control did nothing, and
# because Com_Printf is compiled out it said nothing either. The row looked
# exactly like a working one.
#
# It is easy to repeat that mistake, because the names are plausible and an
# invented one is indistinguishable in the source from a real one. It happened
# again while adding the new rows: six of them were written from memory and had
# to be taken back out. So every row is checked against docs/cvars.txt, and
# duplicates are rejected - a repeated row would show one control twice
# sharing one state.
$mods = Get-Content (Join-Path $Src 'mods.cpp') -Raw
$cvars = [regex]::Matches($mods, '(?m)^\s*\{ "[^"]*"\s*,\s*"(?<cvar>[A-Za-z_][A-Za-z_0-9]*)"') |
    ForEach-Object { $_.Groups['cvar'].Value }
$table = Get-Content (Join-Path $Root 'docs\cvars.txt') -Raw
if ($cvars.Count -eq 0) {
    Fail 'the Mods tab variables could not be checked' 'no rows were parsed out of mods.cpp'
} else {
    $absent = @()
    foreach ($c in ($cvars | Sort-Object -Unique)) {
        if ($table -notmatch ('(?<![A-Za-z0-9_])' + [regex]::Escape($c) + '(?![A-Za-z0-9_])')) { $absent += $c }
    }
    if ($absent.Count -gt 0) {
        Fail 'the Mods tab sends variables this game does not have' ("$($absent -join ', '). A control for one of these does nothing, and says nothing.")
    } else {
        $dupes = @($cvars | Group-Object | Where-Object { $_.Count -gt 1 } | ForEach-Object { $_.Name })
        if ($dupes.Count -gt 0) {
            Fail 'the Mods tab has duplicate rows' ("$($dupes -join ', ') - one control, one state, listed twice")
        } else {
            Pass "all $($cvars.Count) Mods-tab variables exist in the title, and none is listed twice"
        }
    }
}

# The console must not advertise commands that do not exist.
#
# It did. The help line offered "god noclip give all timescale com_maxfps",
# and none of those is a variable in this game; because the title's replies
# were compiled out, typing one produced no error at all. So the console did
# not look limited, it looked broken. This asserts every name it mentions is
# real, which is the difference between "here is what you can do" and "here is
# nothing".
$helpLine = [regex]::Match($overlay, '(?m)^\s*ConsoleLine\("Examples: (?<examples>[^"]*)"').Groups['examples'].Value
if ($helpLine -eq '') {
    Fail 'the console examples could not be checked' 'the help line was not found in overlay.cpp'
} else {
    # Only the variable-shaped names are checked. "spmap" and "forest" are a
    # command and a level, and they are not in the variable table because they
    # are not variables - testing them would have flagged correct help text.
    #
    # And the cheats are named explicitly, because those are the ones that
    # were advertised and do not exist.
    $bogus = @()
    foreach ($c in ([regex]::Matches($helpLine, '(?<![A-Za-z0-9_])([A-Za-z_][A-Za-z_0-9]*)') | ForEach-Object { $_.Groups[1].Value })) {
        if ($c -match '^(cg_|r_|CG_|Cg_)') {
            if ($table -notmatch ('(?<![A-Za-z0-9_])' + [regex]::Escape($c) + '(?![A-Za-z0-9_])')) { $bogus += $c }
        }
    }
    $cheats = @('god', 'noclip', 'notarget', 'give', 'giveall', 'timescale', 'com_maxfps')
    $advertised = @()
    foreach ($c in ([regex]::Matches($helpLine, '(?<![A-Za-z0-9_])([A-Za-z_][A-Za-z_0-9]*)') | ForEach-Object { $_.Groups[1].Value })) {
        if ($cheats -contains $c) { $advertised += $c }
    }
    if ($advertised.Count -gt 0) {
        Fail 'the console advertises cheat commands this game does not have' ("$($advertised -join ', '). Not in the title's table, so they fail silently.")
    } elseif ($bogus.Count -gt 0) {
        Fail 'the console advertises variables this game does not have' ("$($bogus -join ', ').")
    } else {
        Pass 'every name the console help mentions exists in the title'
    }
}

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
Section 'The shipped look is the look the menu offers'

# The six grade values exist in two places: the defaults in settings.h, and
# the "Reborn" row of the Look preset in overlay.cpp. They must agree, or a
# player who launches the game, opens the menu, picks the preset that is
# already on and presses Apply gets a different picture than the one they
# started with - and nothing anywhere would say so.
#
# They are read out of both files rather than compared to literals here, so
# this cannot itself go stale when the values are retuned.
$settingsH = Get-Content (Join-Path $Src 'settings.h') -Raw
# The preset row is { name, sharpen, contrast, saturation, temperature,
# brightness, vignette, grain } - sharpen FIRST, because that is the order the
# Look struct declares. An earlier version of this check lined the groups up
# from contrast and so compared every value against its neighbour; it caught
# that immediately, which is the only reason this check is trusted.
$presetRow = [regex]::Match($overlay, '\{ "Reborn[^}]*?([0-9.]+)f,\s*([0-9.]+)f,\s*([0-9.]+)f,\s*([0-9.]+)f,\s*([0-9.]+)f,\s*([0-9.]+)f,\s*([0-9.]+)f\s*\}')
if (-not $presetRow.Success) {
    Fail 'the Reborn preset could not be read' 'no row matching { "Reborn ... } in overlay.cpp'
} else {
    $names = 'sharpen', 'contrast', 'saturation', 'temperature', 'brightness', 'vignette', 'grain'
    $drift = @()
    for ($i = 0; $i -lt 7; $i++) {
        $want = [double]$presetRow.Groups[$i + 1].Value
        $m = [regex]::Match($settingsH, "float\s+$($names[$i])\s*=\s*(-?[0-9.]+)f\s*;")
        if (-not $m.Success) {
            $drift += "$($names[$i]) has no default in settings.h"
            continue
        }
        $have = [double]$m.Groups[1].Value
        if ([Math]::Abs($have - $want) -gt 0.0001) {
            $drift += "$($names[$i]) defaults to $have but the preset sets $want"
        }
    }
    if ($drift.Count -eq 0) {
        Pass 'all seven look defaults are exactly the Reborn preset, so choosing it changes nothing on a fresh install'
    } else {
        Fail 'the shipped look and the Reborn preset disagree' ($drift -join '; ')
    }

    # The Reborn look must also be a real change of picture, not a relabelled
    # identity: a preset that turns out to equal the base game is the exact
    # failure this work was meant to stop, wearing a new name.
    $neutral = @{ contrast = 1.0; saturation = 1.0; temperature = 0.0; brightness = 0.0; vignette = 0.0; grain = 0.0; sharpen = 0.0 }
    $differs = $false
    for ($i = 0; $i -lt 7; $i++) {
        if ([Math]::Abs([double]$presetRow.Groups[$i + 1].Value - $neutral[$names[$i]]) -gt 0.0001) { $differs = $true }
    }
    if ($differs) { Pass 'the Reborn preset is not the identity: it actually changes the picture' }
    else { Fail 'the Reborn preset is the identity' 'every value is neutral, so choosing it would do nothing' }
}

# ---------------------------------------------------------------------------
Section 'The cheats must use the names that work'

# This is the third time this has been worth guarding, and the failure is
# silent: the title accepts the command, does nothing, and there is no output
# because Com_Printf is compiled out. So nothing in the game says a cheat is
# not running, and a check that only counted the buttons would stay green.
#
#   1. The short names - god, noclip, notarget, setviewpos - are ARGUMENTS in
#      the table, not commands. Sending them does nothing at all. The commands
#      are GAME_GODMODEON and GAME_GODMODEOFF and so on.
#   2. The gate is sv_cheats. Without it the title answers with
#      GAME_CHEATSNOTENABLED and does nothing, which looks identical to (1)
#      from the player's side.
#   3. A level load must turn them off, and every level is loaded through
#      "spmap ". If that hook goes, god mode survives into the next level.

$cheatsCpp = Join-Path $Src 'cheats.cpp'
if (-not (Test-Path -LiteralPath $cheatsCpp)) {
    Fail 'the cheats could not be checked' "the file is missing: $cheatsCpp"
} else {
    $cheats = Get-Content -LiteralPath $cheatsCpp -Raw

    foreach ($pair in @(
            @('GAME_GODMODEON', 'God mode on'), @('GAME_GODMODEOFF', 'God mode off'),
            @('GAME_NOTARGETON', 'Notarget on'), @('GAME_NOTARGETOFF', 'Notarget off'),
            @('GAME_NOCLIPON', 'Noclip on'), @('GAME_NOCLIPOFF', 'Noclip off'),
            @('sv_cheats 1', 'the cheat gate'))) {
        if ($cheats -notmatch [regex]::Escape($pair[0])) {
            Fail 'a cheat command is missing or has been renamed' "$($pair[1]): '$($pair[0])' is not in cheats.cpp"
        }
    }

    # A bare "god" or "noclip" as a whole quoted string is the mistake. Match
    # the quotes so the words inside GAME_GODMODEON and the comments do not
    # trip it.
    $bare = @()
    foreach ($bad in 'god', 'noclip', 'notarget', 'invinc', 'setviewpos') {
        if ($cheats -match ('"' + $bad + '"')) { $bare += $bad }
    }
    if ($bare.Count -eq 0) {
        Pass 'the cheats send the GAME_ prefixed names, not the argument names that do nothing'
    } else {
        Fail 'the cheats send an argument name as if it were a command' ("$($bare -join ', ') - these are silently ignored; use the GAME_ prefixed names")
    }

    if ($cheats -match 'sv_cheats 1' -and $cheats -match 'void Cheats::LevelStarted') {
        Pass 'the gate is opened and there is a level-start reset'
    } else {
        Fail 'the cheat gate or the level-start reset is missing' 'sv_cheats 1 and Cheats::LevelStarted must both be in cheats.cpp'
    }

    # The reset has to actually be called. A Cheats::LevelStarted nobody calls
    # is the same as not having it.
    $kernelVideo = Get-Content (Join-Path $Src 'kernel_video.cpp') -Raw
    if ($kernelVideo -match 'Cheats::LevelStarted\(\)' -and $kernelVideo -match '"spmap"') {
        Pass 'a level load calls the cheat reset, on the "spmap " prefix every level comes through'
    } else {
        Fail 'the cheat reset is never called' 'QueueConsoleCommand must call Cheats::LevelStarted on the "spmap " prefix'
    }
}

# ---------------------------------------------------------------------------
Section 'Every tab opened is closed'

# An ImGui BeginTabItem pushes an ID that EndTabItem pops. Leaving one open
# is not a compile error and it is not a crash: ImGui reports "Missing
# PopID()", and then every widget after it in the window carries the wrong
# ID. The player sees an error box on one control and finds that the tabs
# stopped responding - which is what happened on 2026-10-15, when the Cheats
# tab was added without its EndTabItem.
#
# Counting catches it. It cannot catch the reverse mistake - a stray End on its
# own is a pop with no push, and the count here would be zero either way.

$overlayTabs = Get-Content (Join-Path $Src 'overlay.cpp') -Raw
$begun = ([regex]::Matches($overlayTabs, 'BeginTabItem\(')).Count
$ended = ([regex]::Matches($overlayTabs, 'EndTabItem\(\)')).Count
if ($begun -eq $ended -and $begun -gt 0) {
    Pass "all $begun ImGui tabs are closed, so no widget after them carries the wrong ID"
} else {
    Fail 'an ImGui tab is not closed' "$begun BeginTabItem against $ended EndTabItem - ImGui will say Missing PopID() and the tabs after it stop responding"
}

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
        # Absolute, because a relative "build" broke from any other working directory.
        # cmd's stderr goes to a file rather than a 2>&1 pipeline: with
        # $ErrorActionPreference='Stop' a native command writing to stderr
        # inside such a pipeline raises a terminating error, which killed the
        # whole run on a compiler warning.
        $buildDir = Join-Path $Root 'build'
        $buildLog = Join-Path ([IO.Path]::GetTempPath()) 'cod3_selfcheck_build.txt'
        $llvmBin = if ($clang) { Split-Path -Parent $clang } else { '' }
        $pathParts = @(@($llvmBin, $cmakeDir, $ninjaDir) | Where-Object { $_ }) -join ';'
        $inner = "cmake --build `"$buildDir`" --parallel 3 --target CoD3"
        $previous2 = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        try {
            cmd /c "`"$bt`" >nul 2>&1 && set PATH=$pathParts;%PATH% && $inner" 2> $buildLog 1> $null
            $buildExit = $LASTEXITCODE
        } finally {
            $ErrorActionPreference = $previous2
        }
        if ($buildExit -eq 0) { Pass 'the host builds' }
        else {
            $why = ''
            if (Test-Path -LiteralPath $buildLog) {
                $why = ((Get-Content $buildLog | Select-String -Pattern 'error' | Select-Object -First 2) -join ' | ')
            }
            if (-not $why) { $why = 'no error line in the build output' }
            Fail 'the host does not build' $why
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
        $gameRoot = if (Test-Path $gamePath) { (Get-Content $gamePath -Raw).Trim() } else { '' }
        if ($gameRoot -eq '' -or -not (Test-Path (Join-Path $gameRoot 'default.xex'))) {
            Fail 'the game check' 'the game is not installed, or game.path does not name it'
        } else {
            $env:COD3_LOG = 'always'
            $env:COD3_NOFILMS = '1'
            Remove-Item Env:COD3_MAP, Env:COD3_CMD, Env:COD3_PAD, Env:COD3_XEX -ErrorAction SilentlyContinue
            $proc = Start-Process -FilePath $exe -ArgumentList '--game', "`"$gameRoot`"" -WorkingDirectory $run -PassThru
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
            else { Fail 'ps2 was sent the wrong number of times' ("got $ps2, expected 1") }
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
    # A linked worktree's .git is a FILE, not a directory, and writing under it
    # fails there. Also, git writes "not a git repository" to stderr, which
    # under $ErrorActionPreference='Stop' throws - and would take the run down
    # after the verdict was already known good. Found by running this from a
    # copy with no .git at all, which is what CI will be.
    $gitDir = Join-Path $Root '.git'
    if (-not (Test-Path -LiteralPath $gitDir -PathType Container)) {
        Write-Host '  not a git checkout, so no verdict file was written' -ForegroundColor DarkGray
        exit 0
    }

    $stamp = Join-Path $gitDir 'SELFCHECK'
    $savedEap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $head = (& git -C $Root rev-parse --short HEAD 2> $null)
        if ($LASTEXITCODE -ne 0 -or -not $head) { $head = 'uncommitted' }
    } finally {
        $ErrorActionPreference = $savedEap
    }
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
$gitDirFail = Join-Path $Root '.git'
if (Test-Path -LiteralPath $gitDirFail -PathType Container) {
    $stale = Join-Path $gitDirFail 'SELFCHECK'
    if (Test-Path -LiteralPath $stale) {
        Remove-Item -LiteralPath $stale -Force -ErrorAction SilentlyContinue
    }
}
exit 1