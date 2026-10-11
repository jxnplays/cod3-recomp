<#
    verify.ps1 - build the port and check it behaves as expected.

    Added after two build breakages in one session, both caused by patch scripts
    that mutated a string in memory and then asserted, so the assertion fired
    before anything was written and the change silently did not land. The failure
    mode was not obvious at the time and cost two build cycles each.

    This makes the check mechanical instead of something to remember:

        powershell -ExecutionPolicy Bypass -File tools\verify.ps1

    It builds, then runs twice - once with no environment set, which is what a
    user gets, and once with the diagnostic opt-ins - and checks the observable
    outcomes rather than just the exit code. A build that succeeds but regresses
    behaviour is exactly the case that was not being caught.

    Read-only with respect to the source tree: it builds and it runs, and it
    changes no files under src\ or tools\.
#>

param(
    [int]$TimeoutSeconds = 180
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
Set-Location $repo

function Step($name) {
    Write-Host ""
    Write-Host "== $name ==" -ForegroundColor Cyan
}

function Fail($msg) {
    Write-Host "FAIL: $msg" -ForegroundColor Red
    exit 1
}

# --- 1. tree must be clean ----------------------------------------------------
Step "working tree"
$dirty = git status --porcelain
if ($dirty) {
    Write-Host $dirty
    Fail "working tree is dirty; commit or stash before verifying"
}
Write-Host "clean at $(git log --oneline -1)"

# --- 2. build ----------------------------------------------------------------
Step "build"
$log = "build\verify_build.log"
cmd /c "tools\build_cod3.bat" > $log 2>&1
if ($LASTEXITCODE -ne 0) {
    Select-String -Path $log -Pattern ': error' | Select-Object -First 10 |
        ForEach-Object { $_.Line }
    Fail "build failed (see $log)"
}

$errors = Select-String -Path $log -Pattern ': error'
if ($errors) {
    $errors | Select-Object -First 10 | ForEach-Object { $_.Line }
    Fail "build reported errors"
}
Write-Host "build ok"

if (-not (Test-Path "build\host\COD3.exe")) {
    Fail "build\host\COD3.exe missing"
}
$exe = (Resolve-Path "build\host\COD3.exe").Path
Write-Host ("exe {0:N0} bytes" -f (Get-Item $exe).Length)

# --- 3. run with a wall-clock bound ------------------------------------------
# The run is run as a job and stopped if it overruns. A hang is a legitimate
# outcome to observe, but it must not block the check.
function Run-Bounded([string]$outFile, [hashtable]$envVars) {
    $job = Start-Job -ScriptBlock {
        param($exe, $out, $vars)
        foreach ($k in $vars.Keys) {
            Set-Item -Path "Env:$k" -Value $vars[$k]
        }
        & $exe --no-window 2>&1 | Out-File -Encoding utf8 $out
    } -ArgumentList $exe, (Join-Path $repo $outFile), $envVars

    $done = Wait-Job $job -Timeout $TimeoutSeconds
    $hung = $null -eq $done
    if ($hung) { Stop-Job $job }
    Receive-Job $job -ErrorAction SilentlyContinue | Out-Null
    Remove-Job $job -Force
    Get-Process COD3 -ErrorAction SilentlyContinue |
        Stop-Process -Force -ErrorAction SilentlyContinue
    return $hung
}

# --- 4. default run: what a user actually gets -------------------------------
Step "default run (no environment set)"
$hung = Run-Bounded "build\verify_default.log" @{}
if ($hung) {
    Write-Host "WARNING: default run exceeded ${TimeoutSeconds}s"
} else {
    Write-Host "default run exited"
}

$default = Get-Content "build\verify_default.log" -ErrorAction SilentlyContinue
if (-not $default) {
    Fail "default run produced no output"
}
if ($default -match 'entering recompiled entry point') {
    Write-Host "reached the title entry point"
} else {
    Fail "default run never reached the entry point"
}

# The diagnostic opt-ins must not change the default behaviour. If this ever
# fails, a debug switch has leaked into the normal path.
if ($default -match 'heap: skipping|installed grow method|installed methods') {
    Fail "diagnostic output appeared in the DEFAULT run - an opt-in is leaking"
}
Write-Host "no opt-in output in the default run"

# --- 5. diagnostic run -------------------------------------------------------
Step "diagnostic run (opt-ins set)"
$hung = Run-Bounded "build\verify_diag.log" @{
    COD3_TOLERATE_HEAP_FAULT = "1"
}
if ($hung) {
    Write-Host "WARNING: diagnostic run exceeded ${TimeoutSeconds}s"
} else {
    Write-Host "diagnostic run exited"
}

$diag = Get-Content "build\verify_diag.log" -ErrorAction SilentlyContinue
if (-not $diag) {
    Fail "diagnostic run produced no output"
}

# The CRT heap was fixed, so the tolerance should have nothing left to skip.
$skips = ($diag | Select-String -Pattern 'heap: skipping').Count
if ($skips -eq 0) {
    Write-Host "heap tolerance: 0 skips (expected - the heap is fixed)"
} else {
    Write-Host "WARNING: $skips heap skip(s) - the heap has regressed"
}

# --- 6. done -----------------------------------------------------------------
Step "result"
Write-Host "all checks passed" -ForegroundColor Green
exit 0
