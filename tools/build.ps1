# =============================================================================
#  COD3 Recomp - full build pipeline
#
#  1. Analyse default.xex for jump tables (XenonAnalyse)
#  2. Translate the XEX to C++ (XenonRecomp)
#  3. Generate the Xbox 360 kernel import stubs
#  4. Compile the host executable and the recompiled game code
#
#  Everything is incremental: re-running skips work that is already done.
#  Pass -Recompile to force the translation and compilation stages to re-run.
# =============================================================================

[CmdletBinding()]
param(
    [string]$Xex = 'X:\AI\Recomp Projects\Call of Duty 3\default.xex',
    [int]$Jobs = 8,
    [switch]$Recompile,
    [switch]$SkipTools
)

$ErrorActionPreference = 'Stop'
$RepoRoot = Split-Path -Parent $PSScriptRoot
$BuildDir = Join-Path $RepoRoot 'build'
$XenonBuild = Join-Path $BuildDir 'xenonrecomp'
$PpcDir    = Join-Path $BuildDir 'ppc'
$RtDir     = Join-Path $BuildDir 'runtime'
$ConfigDir = Join-Path $RepoRoot 'config\recomp'
$EnvBat    = Join-Path $RepoRoot 'tools\env.bat'

function Write-Step($m) { Write-Host "==> $m" -ForegroundColor Cyan }
function Write-Ok($m)   { Write-Host "    OK: $m" -ForegroundColor Green }
function Write-Info($m) { Write-Host "    $m" -ForegroundColor Gray }

New-Item -ItemType Directory -Force -Path $BuildDir, $PpcDir, $RtDir, $ConfigDir | Out-Null

if (-not (Test-Path $Xex)) { throw "XEX not found: $Xex" }

# -----------------------------------------------------------------------------
if (-not $SkipTools) {
    Write-Step 'Toolchain'
    & (Join-Path $PSScriptRoot 'setup.ps1')
    if ($LASTEXITCODE -ne 0) { throw 'Toolchain setup failed.' }
}

$XenonRecompExe = Join-Path $XenonBuild 'XenonRecomp\XenonRecomp.exe'
$XenonAnalyseExe = Join-Path $XenonBuild 'XenonAnalyse\XenonAnalyse.exe'
foreach ($p in @($XenonRecompExe, $XenonAnalyseExe)) {
    if (-not (Test-Path $p)) { throw "Missing tool: $p (run tools/setup.ps1)" }
}

# -----------------------------------------------------------------------------
# The recompiler config embeds paths relative to itself, so keep them in sync
# with wherever the dump actually lives.
# -----------------------------------------------------------------------------
$configToml = Join-Path $ConfigDir 'COD3.toml'
$switchToml = Join-Path $ConfigDir 'COD3_switch_tables.toml'

# Windows PowerShell 5.1 has no [IO.Path]::GetRelativePath, so derive the
# relative path through a URI instead.
function Get-RelativePath {
    param([string]$BaseDirectory, [string]$Target)
    $baseFull = [System.IO.Path]::GetFullPath($BaseDirectory).TrimEnd('\') + '\'
    $tgtFull = [System.IO.Path]::GetFullPath($Target)
    $baseUri = [Uri]$baseFull
    $tgtUri = [Uri]$tgtFull
    $rel = $baseUri.MakeRelativeUri($tgtUri).ToString()
    return [System.Uri]::UnescapeDataString($rel).Replace('/', '/')
}

$xexRelative = Get-RelativePath -BaseDirectory $ConfigDir -Target $Xex
Write-Info "XEX (relative to config): $xexRelative"

# -----------------------------------------------------------------------------
Write-Step 'Analysing jump tables (XenonAnalyse)'

$needAnalyse = $Recompile -or -not (Test-Path $switchToml)
if ($needAnalyse) {
    & $XenonAnalyseExe $Xex $switchToml
    if ($LASTEXITCODE -ne 0) { throw 'XenonAnalyse failed.' }
    $n = (Select-String -Path $switchToml -Pattern '^\[\[switch\]\]').Count
    Write-Ok "$n jump tables -> $(Split-Path -Leaf $switchToml)"
} else {
    Write-Ok 'up to date'
}

# -----------------------------------------------------------------------------
Write-Step 'Translating XEX to C++ (XenonRecomp)'

$recompMarker = Join-Path $PpcDir 'ppc_func_mapping.cpp'
$needRecomp = $Recompile -or -not (Test-Path $recompMarker)

if ($needRecomp) {
    # Rebuild the config with the current XEX path.
    $template = Join-Path $ConfigDir 'COD3.toml'
    if (Test-Path $template) {
        $text = Get-Content $template -Raw
        $text = [regex]::Replace($text, '(?m)^file_path\s*=.*$', "file_path = `"$xexRelative`"")
        Set-Content -Path $configToml -Value $text -Encoding UTF8
    }

    Get-ChildItem $PpcDir -Filter 'ppc_recomp.*.cpp' -ErrorAction SilentlyContinue | Remove-Item -Force

    $log = Join-Path $BuildDir 'recomp.log'
    & $XenonRecompExe $configToml (Join-Path $RepoRoot '.tools\XenonRecomp\XenonUtils\ppc_context.h') *>&1 |
        Tee-Object -FilePath $log | Out-Null

    if (-not (Test-Path $recompMarker)) { throw "XenonRecomp produced no output; see $log" }

    $files = Get-ChildItem $PpcDir -Filter 'ppc_recomp.*.cpp'
    $bytes = ($files | Measure-Object Length -Sum).Sum
    Write-Ok ("{0} translation units, {1:N0} bytes of generated C++" -f $files.Count, $bytes)

    # Surface translator gaps: these become __builtin_debugtrap() in the output.
    $unimpl = (Select-String -Path $log -Pattern '^Unrecognized instruction' -ErrorAction SilentlyContinue).Count
    if ($unimpl -gt 0) {
        Write-Info "$unimpl instructions the translator does not implement (become debugtraps)"
    }
} else {
    Write-Ok 'up to date'
}

# -----------------------------------------------------------------------------
Write-Step 'Generating kernel import stubs'

$stubs = Join-Path $RtDir 'kernel_imports_stubs.cpp'
& python (Join-Path $PSScriptRoot 'gen_import_stubs.py') $PpcDir $stubs
if ($LASTEXITCODE -ne 0) { throw 'Stub generation failed.' }

# -----------------------------------------------------------------------------
Write-Step 'Compiling the port'

$CmakeBuild = Join-Path $BuildDir 'cod3'
& cmd /c "`"$EnvBat`" && cmake -S `"$RepoRoot\src`" -B `"$CmakeBuild`" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl && cmake --build `"$CmakeBuild`" --config Release -j $Jobs"
if ($LASTEXITCODE -ne 0) { throw 'Compilation failed.' }

$exe = Join-Path $BuildDir 'host\COD3.exe'
if (Test-Path $exe) {
    Write-Ok "built $exe ($([math]::Round((Get-Item $exe).Length / 1MB, 1)) MB)"
} else {
    throw 'Build reported success but COD3.exe was not produced.'
}

Write-Host ''
Write-Host 'Build complete. Launch with:' -ForegroundColor Green
Write-Host "  `"$exe`" --xex `"$Xex`""
Write-Host 'See docs/STATUS.md for what works and what does not.' -ForegroundColor Green