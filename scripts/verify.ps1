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
# Globbed, newest first, for the same reason as in recompile.ps1: a fixed list
# named only one Visual Studio version and broke on every other.
$VcVars = @(
    Get-ChildItem 'C:\Program Files\Microsoft Visual Studio\*\*\VC\Auxiliary\Build\vcvars64.bat' -ErrorAction SilentlyContinue |
        Sort-Object FullName -Descending | Select-Object -ExpandProperty FullName
    Get-ChildItem 'C:\Program Files (x86)\Microsoft Visual Studio\*\*\VC\Auxiliary\Build\vcvars64.bat' -ErrorAction SilentlyContinue |
        Sort-Object FullName -Descending | Select-Object -ExpandProperty FullName
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
# ppc_testing_native_thunks is Xenia's test for its synthetic native thunks.
#
# A JIT intercepts instructions it does not translate and emulates them in host
# code. This is a static recompiler: everything becomes C++ at build time, so
# there is no thunk and nothing to test. It does not compile either - the symbol
# assembles to `.save_registers_ctx`, which is not a valid C++ identifier - and
# one unbuildable file fails the entire suite.
#
# Excluded here, where the list is built, so they never reach the recompiler.
#
# seq_jumptable_constants goes for the same reason: it tests the emulator's own
# jump-table constant handling, and it takes its constants from ppc_config.h -
# which the thunk file was what pulled in. Without that file it does not compile
# either.
#
# Both are emulator MECHANICS rather than instruction TRANSLATION. What this
# suite is for is whether the recompiler turns instructions into the values the
# hardware produces, and that is what the 1,000-odd instr_* tests cover.
$ExcludeTests = @(
    'ppc_testing_native_thunks'
    'seq_jumptable_constants'
)

if ($All) {
    $Selected = Join-Path $Tests 'bin-all'
    $wanted = Get-ChildItem "$Bin\*.o" | Where-Object {
        $name = $_.BaseName
        -not ($ExcludeTests | Where-Object { $name -like "$_*" })
    }
}
else {
    $Selected = Join-Path $Tests 'bin-covered'
    $wanted = @()
    foreach ($op in $Covered) { $wanted += Get-ChildItem "$Bin\instr_$op.o" -ErrorAction SilentlyContinue }
}

if (Test-Path $Selected) { Remove-Item $Selected -Recurse -Force }
New-Item -ItemType Directory -Force $Selected | Out-Null

foreach ($obj in $wanted) {
    foreach ($extension in @('o', 'dis')) {
        $file = [IO.Path]::ChangeExtension($obj.FullName, $extension)
        if (Test-Path $file) { Copy-Item $file $Selected }
    }
}

$count = (Get-ChildItem "$Selected\*.o").Count
if ($All) { Write-Host "    testing all $count of Xenia's instruction tests, excluding $($ExcludeTests -join ', ')" }
else      { Write-Host "    testing the $count instructions this project added" }

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
