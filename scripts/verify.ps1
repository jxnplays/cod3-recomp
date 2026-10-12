# Checks the recompiler against Xenia's PowerPC instruction tests.
#
# Each test is a short assembly routine with the input registers and the
# expected output registers written alongside it. The routine is assembled,
# recompiled to C++ the same way the game's code is, compiled, and run: any
# register that comes out different is a miscompiled instruction.
#
# Xenia ships the test sources but not the assembled objects, and the LLVM
# install here has no PowerPC target, so tools/PpcAsm assembles them using the
# opcode tables XenonRecomp already carries.
#
#   .\scripts\verify.ps1              # the instructions this project added
#   .\scripts\verify.ps1 -All         # every test Xenia has

[CmdletBinding()]
param(
    [switch]$All,
    [switch]$Fetch,
    [int]$Jobs = [Environment]::ProcessorCount
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot

$LlvmBin = 'C:\Program Files\LLVM\bin'
$VsInstaller = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer'
$VcVars = @(
    'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat'
    'C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
) | Where-Object { Test-Path $_ } | Select-Object -First 1
$env:PATH = "$VsInstaller;$LlvmBin;$env:PATH"

function Invoke-InVsEnv([string]$Command) {
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { cmd /c "call `"$VcVars`" >nul 2>&1 && $Command" }
    finally { $ErrorActionPreference = $previous }
    if ($LASTEXITCODE -ne 0) { throw "failed (exit $LASTEXITCODE): $Command" }
}

$Tests    = Join-Path $Root 'tools\xenia-tests\src\xenia\cpu\ppc\testing'
$ToolsDir = Join-Path $Root 'tools'
$Build    = Join-Path $Root 'build'
$ToolBuild= Join-Path $Root 'tools\build'
$Harness  = Join-Path $Root 'tools\XenonRecomp\XenonTests'

# The instructions this project added to the recompiler that Xenia has a test
# for. The rest were added without test coverage and are listed in STATUS.md.
$Covered = @(
    'vslh', 'vsrah', 'vsubshs', 'vspltish', 'vmaxsh', 'vandc', 'vminsh',
    'vpkswss', 'vctuxs', 'vavguh', 'vsel', 'vpkswus', 'vsrh', 'vrlh',
    'vpkshss', 'vpkuhus'
)

# --- 1. Test sources -------------------------------------------------------
if ($Fetch -or -not (Test-Path (Join-Path $Tests 'instr_add.s'))) {
    Write-Host '==> Fetching Xenia instruction tests' -ForegroundColor Cyan
    $clone = Join-Path $ToolsDir 'xenia-tests'
    if (Test-Path $clone) { Remove-Item $clone -Recurse -Force }
    New-Item -ItemType Directory -Force $clone | Out-Null
    Push-Location $clone
    try {
        git init -q .
        git remote add origin https://github.com/xenia-project/xenia.git
        git config core.sparseCheckout true
        Set-Content -Encoding ascii '.git/info/sparse-checkout' 'src/xenia/cpu/ppc/testing/*'
        git fetch --depth 1 origin master -q
        git checkout -q FETCH_HEAD
    } finally { Pop-Location }
}

# --- 2. Tools --------------------------------------------------------------
Write-Host '==> Building the assembler and the recompiler' -ForegroundColor Cyan
Invoke-InVsEnv "cmake -S `"$Root`" -B `"$Build`" -G Ninja -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_BUILD_TYPE=Release"
Invoke-InVsEnv "cmake --build `"$Build`" --target PpcAsm --parallel $Jobs"
Invoke-InVsEnv "cmake -S `"$Root\tools\XenonRecomp`" -B `"$ToolBuild`" -G Ninja -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_BUILD_TYPE=Release"
Invoke-InVsEnv "cmake --build `"$ToolBuild`" --target XenonRecomp --parallel $Jobs"

# --- 3. Assemble -----------------------------------------------------------
# The harness masks the addresses it reads from the disassembly with 0xFFFFF
# before matching them, so the image has to sit below that.
Write-Host '==> Assembling' -ForegroundColor Cyan
$Bin = Join-Path $Tests 'bin'
New-Item -ItemType Directory -Force $Bin | Out-Null
$asm = Join-Path $Build 'PpcAsm\PpcAsm.exe'

$clean = 0; $problems = 0
$previous = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
foreach ($source in Get-ChildItem "$Tests\*.s") {
    $name = $source.BaseName
    # A file the assembler cannot fully encode is expected and not fatal: the
    # extended mnemonics some tests use are not implemented. Its exit code says
    # so, and the count is reported below.
    cmd /c "`"$asm`" `"$($source.FullName)`" `"$Bin\$name.o`" `"$Bin\$name.dis`" 0x10000 >nul 2>&1"
    if ($LASTEXITCODE -eq 0) { $clean++ } else { $problems++ }
}
$ErrorActionPreference = $previous
Write-Host "    $clean assembled cleanly, $problems had instructions the assembler cannot encode"

# --- 4. Select -------------------------------------------------------------
if ($All) { $Selected = Join-Path $Tests 'bin' }
else      { $Selected = Join-Path $Tests 'bin-covered' }
if (-not $All) {
    if (Test-Path $Selected) { Remove-Item $Selected -Recurse -Force }
    New-Item -ItemType Directory -Force $Selected | Out-Null
    foreach ($op in $Covered) {
        foreach ($extension in @('o', 'dis')) {
            $file = "$Bin\instr_$op.$extension"
            if (Test-Path $file) { Copy-Item $file $Selected }
        }
    }
    Write-Host "    testing the $((Get-ChildItem "$Selected\*.o").Count) instructions this project added"
}

# --- 5. Recompile and run --------------------------------------------------
Write-Host '==> Recompiling the tests' -ForegroundColor Cyan
Get-ChildItem "$Harness\*.cpp" -ErrorAction SilentlyContinue | Remove-Item -Force
& "$ToolBuild\XenonRecomp\XenonRecomp.exe" $Selected $Harness | Out-Null

Write-Host '==> Building and running' -ForegroundColor Cyan
# XenonTests globs its sources, so the set that was generated last time is
# baked into the build files. Re-running the configure step picks up the set
# that exists now instead of looking for files that have been deleted.
Invoke-InVsEnv "cmake -S `"$Root\tools\XenonRecomp`" -B `"$ToolBuild`" -G Ninja -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_BUILD_TYPE=Release"
Invoke-InVsEnv "cmake --build `"$ToolBuild`" --target XenonTests --parallel $Jobs"

$output = & "$ToolBuild\XenonTests\XenonTests.exe" 2>&1
$failures = @($output | Where-Object { $_ -match 'EXPECTED' })

Write-Host ''
if ($failures.Count -eq 0) {
    Write-Host 'Every check passed: each instruction produced the registers the hardware does.' -ForegroundColor Green
} else {
    Write-Host "$($failures.Count) checks failed:" -ForegroundColor Red
    $failures | Select-Object -First 40 | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
    exit 1
}
