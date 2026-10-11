# =============================================================================
#  COD3 Recomp - toolchain setup
#
#  Fetches and builds the XenonRecomp static recompiler plus the LLVM/clang
#  toolchain it needs. Safe to re-run; each step is skipped when already done.
#
#  Requirements (all checked, nothing is installed silently):
#    * Visual Studio 2022 Build Tools with the MSVC x64 toolset
#    * LLVM 18 or newer (clang-cl)
#    * CMake 3.20+ and Ninja  (bundled with the VS Build Tools)
#    * Python 3.11+ with pycryptodome (only for the analysis helpers)
# =============================================================================

[CmdletBinding()]
param(
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
$RepoRoot = Split-Path -Parent $PSScriptRoot
$ToolsDir = Join-Path $RepoRoot '.tools'
$XenonDir = Join-Path $ToolsDir 'XenonRecomp'
$BuildDir = Join-Path $RepoRoot 'build'

$VsWhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'

function Write-Step($msg) { Write-Host "==> $msg" -ForegroundColor Cyan }
function Write-Ok($msg)   { Write-Host "    OK: $msg" -ForegroundColor Green }
function Write-Warn2($msg){ Write-Host "    !!  $msg" -ForegroundColor Yellow }

# -----------------------------------------------------------------------------
Write-Step 'Checking prerequisites'

if (-not (Test-Path $VsWhere)) {
    throw 'Visual Studio 2022 Build Tools not found. Install them with the "Desktop development with C++" workload.'
}
$VsPath = & $VsWhere -products * -format value -property installationPath | Select-Object -First 1
if (-not $VsPath) { throw 'No Visual Studio installation found.' }
Write-Ok "Visual Studio: $VsPath"

$clang = 'C:\Program Files\LLVM\bin\clang-cl.exe'
if (-not (Test-Path $clang)) {
    throw 'clang-cl not found at C:\Program Files\LLVM\bin. Install LLVM 18 or newer.'
}
$clangVersion = (& $clang --version | Select-Object -First 1)
Write-Ok "clang: $clangVersion"

$cmake = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
if (-not (Test-Path $cmake)) { throw 'CMake not found (expected alongside the VS Build Tools).' }
Write-Ok "cmake: $(& $cmake --version | Select-Object -First 1)"

if (Get-Command git -ErrorAction SilentlyContinue) {
    Write-Ok "git: $(git --version)"
} else {
    throw 'git is required to fetch XenonRecomp.'
}

# -----------------------------------------------------------------------------
Write-Step 'Fetching XenonRecomp'

if ((Test-Path $XenonDir) -and -not $Force) {
    Write-Ok 'already present'
} else {
    if (Test-Path $XenonDir) { Remove-Item $XenonDir -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $ToolsDir | Out-Null
    # XenonRecomp requires recursive submodules (fmt, tomlplusplus, xxHash,
    # libmspack, tiny-AES-c, simde).
    git clone --depth 1 --recurse-submodules --shallow-submodules `
        https://github.com/hedge-dev/XenonRecomp.git $XenonDir
    if ($LASTEXITCODE -ne 0) { throw 'Failed to clone XenonRecomp.' }
}

$commit = git -C $XenonDir log -1 --format='%H'
Write-Ok "XenonRecomp at $commit"

# -----------------------------------------------------------------------------
Write-Step 'Building XenonRecomp and XenonAnalyse'

$XenonBuild = Join-Path $BuildDir 'xenonrecomp'
$Exe = Join-Path $XenonBuild 'XenonRecomp\XenonRecomp.exe'

if ((Test-Path $Exe) -and -not $Force) {
    Write-Ok 'already built'
} else {
    & cmd /c "`"$RepoRoot\tools\env.bat`" && cmake -S `"$XenonDir`" -B `"$XenonBuild`" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl && cmake --build `"$XenonBuild`" --config Release"
    if ($LASTEXITCODE -ne 0) { throw 'XenonRecomp build failed.' }
}

foreach ($tool in @('XenonRecomp\XenonRecomp.exe', 'XenonAnalyse\XenonAnalyse.exe')) {
    $p = Join-Path $XenonBuild $tool
    if (-not (Test-Path $p)) { throw "Expected tool missing: $p" }
    Write-Ok $tool
}

# -----------------------------------------------------------------------------
Write-Step 'Python helpers (optional)'

$python = Get-Command python -ErrorAction SilentlyContinue
if ($python) {
    Write-Ok "python: $(& python --version)"
    & python -c 'import Crypto' 2>$null
    if ($LASTEXITCODE -ne 0) {
        Write-Warn2 'pycryptodome missing; tools/xex_extract.py will not run.'
        Write-Warn2 'Fix with: pip install pycryptodome'
    } else {
        Write-Ok 'pycryptodome present'
    }
} else {
    Write-Warn2 'python not found; analysis helpers will be unavailable.'
}

Write-Host ''
Write-Host 'Setup complete. Run tools/build.ps1 to recompile and build the port.' -ForegroundColor Green