# Recompiles one level's DLL (sp\<name>\<name>.dll) into CoD3RecompLib/levels/<name>.
#
# Every level ships its script code as a XEX DLL of its own. This does for one
# what recompile.ps1 does for default.xex, and the rest that a level needs:
#
#   1. finds the register save/restore helpers with CoD3Scan and writes the
#      level's TOML from them (config/<name>.toml)
#   2. detects the jump tables with XenonAnalyse
#   3. recompiles; then bounds the functions the analyser cut short at a jump
#      table (CoD3Scan --fixfuncs on the recompiler's report) and the ones it
#      cut into pieces elsewhere (level_functions.py on the generated code,
#      kept in config/<name>_functions.toml), and recompiles again, until
#      nothing changes
#   4. renames the helpers and the function table for the level
#      (rename_level.py) and puts the returns after the yield calls
#      (patch_recomp.py)
#
#   .\scripts\recompile_level.ps1 saint_lo
#   .\scripts\recompile_level.ps1 -All
#
# The DLL is taken from the installed game (game.path names it) and copied to
# CoD3RecompLib/private, which is never committed.

[CmdletBinding()]
param(
    [Parameter(Position = 0)] [string]$Level = '',
    [switch]$All,
    [string]$GameRoot = ''
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot

$Recomp  = Join-Path $Root 'tools\build\XenonRecomp\XenonRecomp.exe'
$Analyse = Join-Path $Root 'tools\build\XenonAnalyse\XenonAnalyse.exe'
$Scan    = Join-Path $Root 'tools\build-scan\CoD3Scan.exe'
$ContextHdr = Join-Path $Root 'tools\XenonRecomp\XenonUtils\ppc_context.h'
foreach ($tool in @($Recomp, $Analyse, $Scan)) {
    if (-not (Test-Path $tool)) { throw "$tool is missing; build the tools first (recompile.ps1 -BuildTools, tools/CoD3Scan)" }
}

# Where the installed game is: named, or next to the repository, or where
# game.path (written by the installer beside the executable) points.
if ($GameRoot -eq '') {
    foreach ($candidate in @((Join-Path $Root 'game'), 'D:\Games\x360\game')) {
        if (Test-Path (Join-Path $candidate 'default.xex')) { $GameRoot = $candidate; break }
    }
    $GamePath = Join-Path $Root 'game.path'
    if ($GameRoot -eq '' -and (Test-Path $GamePath)) { $GameRoot = (Get-Content $GamePath -Raw).Trim() }
    if ($GameRoot -eq '') { throw 'the installed game was not found; pass -GameRoot' }
}

$Levels = if ($All) {
    Get-ChildItem (Join-Path $GameRoot 'sp') -Directory | Where-Object { Test-Path (Join-Path $_.FullName "$($_.Name).dll") } | ForEach-Object { $_.Name }
} elseif ($Level -ne '') { @($Level) } else { throw 'name a level, or -All' }

foreach ($name in $Levels) {
    Write-Host "==> $name" -ForegroundColor Cyan
    $Dll = Join-Path $GameRoot "sp\$name\$name.dll"
    if (-not (Test-Path $Dll)) { throw "$Dll is missing" }
    $Private = Join-Path $Root "CoD3RecompLib\private\$name.dll"
    Copy-Item $Dll $Private -Force

    # 1. The helpers, from the scan.
    $scanText = & $Scan $Private 2>&1 | Out-String
    $helpers = @()
    foreach ($line in ($scanText -split "`n")) {
        if ($line -match '^(rest|save)(gprlr|fpr|vmx)_(14|64)_address = (0x[0-9A-F]+)') {
            $helpers += ("{0}{1}_{2}_address = {3}" -f $Matches[1], $Matches[2], $Matches[3], $Matches[4])
        }
    }
    if ($helpers.Count -lt 4) { throw "$name`: the save/restore helpers were not all found:`n$scanText" }
    $info = if ($scanText -match 'base\s+= (0x[0-9A-F]+)') { $Matches[1] } else { '?' }
    $entry = if ($scanText -match 'entry_point = (0x[0-9A-F]+)') { $Matches[1] } else { '?' }

    $Config = Join-Path $Root "CoD3RecompLib\config\$name.toml"
    $SwitchToml = Join-Path $Root "CoD3RecompLib\config\${name}_switch_tables.toml"
    $Out = Join-Path $Root "CoD3RecompLib\levels\$name"
    New-Item -ItemType Directory -Force $Out | Out-Null
    Get-ChildItem $Out -File | Remove-Item -Force

    # 2. Jump tables.
    & $Analyse $Private $SwitchToml | Out-Null

    # 3. Recompile until the function boundaries are right.
    #
    # Two kinds of cut are mended. A function ended at a jump table has its
    # cases reported by the recompiler as "trying to jump outside function",
    # and CoD3Scan --fixfuncs bounds it from that report. A function cut
    # elsewhere (its cases branching to a tail after the last case, a compare
    # chain's "not found" tail) comes out as pieces whose branches into each
    # other are "// ERROR" and a return; scripts/level_functions.py reads the
    # generated code, groups the pieces and writes their extents into
    # config/<name>_functions.toml, which is merged over the scan's on every
    # pass and can be added to by hand.
    $KeptPath = Join-Path $Root "CoD3RecompLib\config\${name}_functions.toml"
    function Read-Kept {
        $lines = @()
        if (Test-Path $KeptPath) {
            foreach ($line in (Get-Content $KeptPath)) {
                if ($line -match '^\s*\{ address = 0x[0-9A-F]+, size = 0x[0-9A-F]+ \},.*$') { $lines += $line.TrimEnd() }
            }
        }
        return $lines
    }
    function Merge-Functions([string[]]$scan, [string[]]$hand) {
        $byAddress = [ordered]@{}
        foreach ($line in ($scan + $hand)) {
            if ($line -match 'address = (0x[0-9A-F]+)') { $byAddress[$Matches[1]] = $line }
        }
        return @($byAddress.Values | Sort-Object)
    }
    function Invoke-Recompile([string[]]$functions) {
        $toml = @(
            "# Call of Duty 3 (Xbox 360) - the $name level's own code: sp\$name\$name.dll",
            "# Generated by scripts/recompile_level.ps1 from CoD3Scan; image base $info, entry $entry.",
            "",
            "[main]",
            "file_path = `"../private/$name.dll`"",
            "out_directory_path = `"../levels/$name`"",
            "switch_table_file_path = `"${name}_switch_tables.toml`"",
            "",
            "skip_lr = false", "skip_msr = false", "ctr_as_local = false", "xer_as_local = false",
            "reserved_as_local = false", "cr_as_local = false", "non_argument_as_local = false",
            "non_volatile_as_local = false",
            ""
        ) + $helpers + @(
            "",
            "invalid_instructions = [",
            "    { data = 0x00000000, size = 4 }, # padding",
            "]"
        )
        if ($functions.Count -gt 0) {
            $toml += @("", "# Bounded by CoD3Scan --fixfuncs from the recompiler's reports and by", "# scripts/level_functions.py (config/${name}_functions.toml).", "functions = [") + $functions + @("]")
        }
        Set-Content -Path $Config -Value ($toml -join "`n") -Encoding ascii

        Get-ChildItem $Out -File | Remove-Item -Force
        Push-Location (Join-Path $Root 'CoD3RecompLib\config')
        try { cmd /c "`"$Recomp`" `"$Config`" `"$ContextHdr`" > `"$Log`" 2>&1" } finally { Pop-Location }
        if ($LASTEXITCODE -ne 0) { throw "XenonRecomp failed for $name (exit $LASTEXITCODE). See $Log" }
    }

    $Log = Join-Path $Root "recomp_$name.log"
    $kept = Read-Kept
    if ($kept.Count -gt 0) { Write-Host "    $($kept.Count) function boundaries kept from $KeptPath" }
    $functions = Merge-Functions @() $kept
    for ($pass = 1; $pass -le 4; $pass++) {
        Invoke-Recompile $functions
        $outside = Select-String -Path $Log -Pattern 'is trying to jump outside function'
        if (-not $outside) { break }
        $fix = & $Scan $Private --fixfuncs $Log 2>&1 | Out-String
        $found = @()
        foreach ($line in ($fix -split "`n")) {
            if ($line -match '^\s*\{ address = 0x[0-9A-F]+, size = 0x[0-9A-F]+ \},.*$') { $found += $line.TrimEnd() }
        }
        if ($found.Count -eq 0) { throw "$name`: functions to fix were reported but CoD3Scan found none" }
        $functions = Merge-Functions ($functions + $found) $kept
        Write-Host ("    pass {0}: {1} function boundaries fixed" -f $pass, $found.Count)
    }
    # The pieces, grouped and made whole, until a recompile adds no more.
    for ($pass = 1; $pass -le 6; $pass++) {
        $report = python (Join-Path $Root 'scripts\level_functions.py') $name
        if ($LASTEXITCODE -ne 0) { throw 'level_functions.py failed' }
        Write-Host "    $report"
        if ($report -match ': 0 function extents') { break }
        $kept = Read-Kept
        $functions = Merge-Functions $functions $kept
        Invoke-Recompile $functions
    }
    $errors = (Select-String -Path "$Out\ppc_recomp.*.cpp" -Pattern '// ERROR [0-9A-F]+').Count
    if ($errors -gt 0) {
        Write-Host "    $errors branches out of their function remain (grep '// ERROR' in $Out); pieces of padding, or extend by hand in $KeptPath" -ForegroundColor Yellow
    }
    $count = (Select-String -Path "$Out\ppc_recomp.*.cpp" -Pattern '^PPC_FUNC_IMPL').Count
    $unknown = Select-String -Path $Log -Pattern 'Unrecognized instruction' | Measure-Object | Select-Object -ExpandProperty Count
    Write-Host "    $count functions, $unknown unrecognized instructions"

    # 4. Names and seams.
    python (Join-Path $Root 'scripts\rename_level.py') $name
    if ($LASTEXITCODE -ne 0) { throw 'rename_level.py failed' }
    python (Join-Path $Root 'scripts\patch_recomp.py') $name
    if ($LASTEXITCODE -ne 0) { throw 'patch_recomp.py failed' }
}
Write-Host 'Done. Reconfigure the build (cmake) so the new level libraries are picked up.' -ForegroundColor Green
