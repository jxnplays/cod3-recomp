<#
.SYNOPSIS
    Runs a command with an enforced memory ceiling, by watching and acting.

.DESCRIPTION
    The memory ceiling asked for on 2026-10-13, which is the machine rule as
    stated: do not pass 90% RAM, and keep 4 GB free. There is no 15 GB trigger
    in this file, and three documents claimed there was until 2026-10-14 -
    including this one, two lines down, which said so in its own header.

    This started life as a Windows job object with JOB_OBJECT_LIMIT_JOB_MEMORY,
    which is the right tool. It does not work in this environment and the
    reason is worth recording, because it is silent:

        SetInformationJobObject returned TRUE, with GetLastError() = 203
        (ERROR_ENVVAR_NOT_FOUND), and reading the limit straight back out of
        the job gave JobMemoryLimit = 0. The agent harness already runs every
        process inside a job object, so this creates a nested one, and on a
        nested job Windows accepts the limit-setting call and drops the limit.

        A test proved it: a python process told to allocate without limit
        reached 24.3 GB. The same process under a 2 GB "limit" reached
        158 GB. The ceiling was never applied at all - only printed.

    So the ceiling here is enforced by watching the machine and killing the
    process when it goes over, which works regardless of job nesting:

        -RefuseFreeMB   total free RAM below which the run is stopped (default
                        4096 MB, which is about the 87% mark on a 32 GB
                        machine)
        -RefusePct      percentage used above which the run is stopped, as a
                        second trigger (default 90, the rule as stated)
        -PeakMB         report the peak total RAM used during the run

    When a trigger fires the process tree is killed, so the machine recovers
    immediately rather than waiting for a hard lock.

.PARAMETER Cmd
    Command line to run. Shell syntax works.

.PARAMETER Exe
    Program to run directly. Use this instead of Cmd when the path has spaces
    in it: going through cmd means the path has to survive a second round of
    quoting, and cmd /c splits it. ("python C:\Users\Jett Owens\x.py" arrives
    as the command "python C:\Users\Jett".)

.PARAMETER ArgumentList
    Arguments for -Exe. Quoted automatically where they contain spaces.

.PARAMETER PollSeconds
    How often to sample. 1 second by default, which is responsive enough to
    catch a fast climb and cheap enough to be free.

.PARAMETER NoWait
    Start the command and return, leaving the watchdog running detached.

.PARAMETER GraceSeconds
    How long to wait for a killed process to exit before forcing it. 5 by
    default.

.EXAMPLE
    .\scripts\limited.ps1 -Cmd 'cmake --build build --parallel 4 --target CoD3'

.EXAMPLE
    .\scripts\limited.ps1 -Exe CoD3.exe -ArgumentList '--game', 'X:\...\Call of Duty 3'
#>
[CmdletBinding(DefaultParameterSetName = 'Cmd')]
param(
    # Watch a process that is already running, instead of starting one. This is
    # how -NoWait gets a watchdog that survives the caller: the detached child
    # has its own console and this one has to let go of it, so the watcher is a
    # separate copy of this script pointed at the pid rather than the command.
    [Parameter(ParameterSetName = 'Watch', Mandatory = $true)]
    [int]$WatchPid,

    [Parameter(ParameterSetName = 'Cmd', Mandatory = $true, Position = 0)]
    [string]$Cmd,

    [Parameter(ParameterSetName = 'Exe', Mandatory = $true)]
    [string]$Exe,

    [Parameter(ParameterSetName = 'Exe')]
    [string[]]$ArgumentList = @(),

    [string]$WorkDir,

    [int]$RefuseFreeMB = 4096,

    [int]$RefusePct = 90,

    [int]$PollSeconds = 1,

    [int]$GraceSeconds = 5,

    [switch]$NoWait
)

$ErrorActionPreference = 'Stop'

function Get-Ram {
    # TotalVisibleMemorySize and FreePhysicalMemory are in kilobytes.
    $os = Get-CimInstance Win32_OperatingSystem
    $totalMB = [int][math]::Round($os.TotalVisibleMemorySize / 1024)
    $freeMB = [int][math]::Round($os.FreePhysicalMemory / 1024)
    [pscustomobject]@{
        TotalMB = $totalMB
        FreeMB = $freeMB
        UsedPct = [math]::Round(($totalMB - $freeMB) / $totalMB * 100, 1)
    }
}

# Kill the process and everything it started. Stop-Process alone leaves
# children, and the client relaunches itself for the next mission, so the
# tree has to go or the process comes straight back.
function Stop-Tree {
    param([int]$RootPid)

    $children = @()
    $queue = New-Object System.Collections.Queue
    $queue.Enqueue($RootPid)
    while ($queue.Count -gt 0) {
        $id = $queue.Dequeue()
        foreach ($c in @(Get-CimInstance Win32_Process -Filter "ParentProcessId=$id" -ErrorAction SilentlyContinue)) {
            $children += $c.ProcessId
            $queue.Enqueue($c.ProcessId)
        }
    }

    foreach ($id in @($children + $RootPid)) {
        Stop-Process -Id $id -Force -ErrorAction SilentlyContinue
    }
    return $children.Count
}

$ram = Get-Ram
if ($ram.FreeMB -lt $RefuseFreeMB -or $ram.UsedPct -gt $RefusePct) {
    Write-Error ("Refusing to start: {0} MB free of {1} MB ({2}% used). " +
                 "Triggers are {3} MB free / {4}%. Close something first." -f `
                 $ram.FreeMB, $ram.TotalMB, $ram.UsedPct, $RefuseFreeMB, $RefusePct)
    exit 2
}

$psi = New-Object System.Diagnostics.ProcessStartInfo
if ($PSCmdlet.ParameterSetName -eq 'Exe') {
    $psi.FileName = $Exe
    $psi.Arguments = (($ArgumentList | ForEach-Object {
        if ($_ -match '\s') { '"' + $_ + '"' } else { $_ }
    }) -join ' ')
}
else {
    $psi.FileName = $env:ComSpec
    $psi.Arguments = '/c ' + $Cmd
}
$psi.UseShellExecute = $false
if ($WorkDir) { $psi.WorkingDirectory = $WorkDir }
else { $psi.WorkingDirectory = (Get-Location).Path }

if ($PSCmdlet.ParameterSetName -eq 'Watch') {
    # Adopt an already-running process and go straight to the polling loop.
    $proc = [System.Diagnostics.Process]::GetProcessById($WatchPid)
}
else {
    $proc = [System.Diagnostics.Process]::Start($psi)
}
if ($proc -eq $null) { throw "failed to start" }

Write-Host ("pid {0}, watching; stops at {1} MB free or {2}% used." -f `
            $proc.Id, $RefuseFreeMB, $RefusePct)

if ($NoWait) {
    # -NoWait must NOT be implemented by leaving the child attached to this
    # script's own stdout. The client writes CoD3.log only when its standard
    # output is a console: CoD3Host/log.cpp returns early for FILE_TYPE_PIPE
    # or FILE_TYPE_DISK, reasoning that "whoever started the run keeps it".
    # A child left on a pipe therefore produces NO LOG AT ALL, and a crash
    # becomes undebuggable - which is exactly what happened to the first
    # multiplayer test.
    #
    # So the already-started child is stopped and started again with its own
    # console. That makes stdout a real character device, the log gets
    # written, and the window is where the run is watched.
    $si = New-Object System.Diagnostics.ProcessStartInfo
    $si.FileName = $psi.FileName
    $si.Arguments = $psi.Arguments
    $si.UseShellExecute = $false
    $si.WorkingDirectory = $psi.WorkingDirectory
    $si.RedirectStandardOutput = $false
    $si.RedirectStandardError = $false

    try { $proc.Kill() } catch { }
    $detached = [System.Diagnostics.Process]::Start($si)

    Write-Host ("pid {0} stopped: it was on a pipe, so it would have written no log." -f `
                $proc.Id)
    Write-Host ("pid {0} started with its own console, so CoD3.log is written." -f `
                $detached.Id)
    # The watchdog, detached, BEFORE returning.
    #
    # This used to just say "watching ..." and exit. The polling loop is below
    # this point, so it never ran, and nothing was watching anything - while
    # docs/MEMORY.md, docs/RESUME.md and this script's own help all described a
    # ceiling that was silently not being enforced. Every build and capture
    # overnight was run this way.
    #
    # Re-launching this script on itself, attached to nothing, is the only way
    # to get a watcher that outlives the caller without a job object (which
    # silently fails under a nested job, the reason this file exists).
    # The script path is quoted explicitly. Start-Process does NOT quote
    # ArgumentList entries for you, so a repository path containing a space -
    # which this one does - arrives as two arguments and the watcher dies with
    # "Processing -File 'X:\AI\Recomp' failed". That is the same quoting trap
    # this file's own header warns about, reached from the other direction.
    $watchArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', ('"{0}"' -f $PSCommandPath),
                   '-WatchPid', $detached.Id, '-RefuseFreeMB', $RefuseFreeMB,
                   '-RefusePct', $RefusePct, '-PollSeconds', $PollSeconds)
    $watcher = Start-Process -FilePath 'powershell.exe' -ArgumentList $watchArgs -WindowStyle Hidden -PassThru

    Write-Host ("pid {0} watching; the watchdog is pid {1}, and stops the tree at {2} MB free or {3}% used." -f `
                $detached.Id, $watcher.Id, $RefuseFreeMB, $RefusePct)

    # And then check that it is actually there.
    #
    # This used to say "watching ..." and exit, with no watchdog at all - the
    # polling loop is below this point and never ran. Three documents and this
    # file's own help described a ceiling that was silently not being enforced.
    #
    # It is also worth knowing whether the watcher survived the hand-off. Under
    # a nested job object - the harness this repository is developed in -
    # a child can be torn down with its parent, and a watchdog that dies with
    # the shell that started it is worse than none, because it was reported as
    # running. So: look, and say plainly if it is not there.
    Start-Sleep -Milliseconds 700
    $alive = Get-Process -Id $watcher.Id -ErrorAction SilentlyContinue
    if ($alive) {
        Write-Host ("the watchdog is running (pid {0}). To stop it: Stop-Process -Id {0}" -f $watcher.Id)
    } else {
        Write-Warning ("the watchdog did not survive the hand-off (pid {0} is gone). This shell" -f $watcher.Id)
        Write-Warning ("environment tears down child processes with their parent, so NOTHING is")
        Write-Warning ("watching this run and the memory ceiling is NOT being enforced. Run this")
        Write-Warning ("command WITHOUT -NoWait to get a real watchdog, or watch the log yourself.")
    }
    exit 0
}

$peakMB = 0
$killed = $false
$childrenKilled = 0

while (-not $proc.HasExited) {
    Start-Sleep -Seconds $PollSeconds

    $ram = Get-Ram
    $usedMB = $ram.TotalMB - $ram.FreeMB
    if ($usedMB -gt $peakMB) { $peakMB = $usedMB }

    if ($ram.FreeMB -lt $RefuseFreeMB -or $ram.UsedPct -gt $RefusePct) {
        $killed = $true
        Write-Warning ("Memory ceiling reached: {0} MB free of {1} MB ({2}% used). Stopping pid {3}." -f `
                       $ram.FreeMB, $ram.TotalMB, $ram.UsedPct, $proc.Id)
        $childrenKilled = Stop-Tree -RootPid $proc.Id

        $deadline = (Get-Date).AddSeconds($GraceSeconds)
        while (-not $proc.HasExited -and (Get-Date) -lt $deadline) {
            Start-Sleep -Milliseconds 250
        }
        if (-not $proc.HasExited) {
            Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
        }
        break
    }
}

$ram = Get-Ram
$usedMB = $ram.TotalMB - $ram.FreeMB
if ($usedMB -gt $peakMB) { $peakMB = $usedMB }

$exitCode = if ($killed) { 1 } else { $proc.ExitCode }

Write-Host ("peak {0} MB used of {1} MB ({2}% of the machine); now {3} MB free." -f `
            $peakMB, $ram.TotalMB, [math]::Round($peakMB / $ram.TotalMB * 100, 1),
            $ram.FreeMB)

if ($killed) {
    Write-Host "Stopped on the memory ceiling; the machine was at risk of a hard lock."
    exit $exitCode
}

exit $exitCode