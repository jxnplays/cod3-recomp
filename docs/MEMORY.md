# Memory limits

The rule from 2026-10-13: **this session is capped at 15 GB, and the machine
is not allowed past 90% RAM.** This is supposed to be enforced by
[`scripts/limited.ps1`](../scripts/limited.ps1), not by remembering to be
careful.

**It was not, for the whole of 2026-10-14.** `limited.ps1 -NoWait` printed
"watching 4096 MB free / 90% used" and then returned, before reaching the
polling loop - so no watchdog ran and the ceiling was never applied. Every
build and every capture run that day used `-NoWait`. Peak that day was about
19 GB, over the cap, because the guard that was supposed to stop it was not
running.

There is no 15 GB trigger in the script at all. The two thresholds it has are
`-RefuseFreeMB` (4096) and `-RefusePct` (90), which is the rule as the user
stated it; the 15 GB figure was never turned into one.

`-NoWait` now starts a real watchdog and then checks that it is still alive
before reporting it, because under this repository's nested job object a
child can be torn down with its parent. When it cannot survive, it says so
and says that nothing is watching. **Run without `-NoWait` when the ceiling
actually matters.**

```powershell
# a build
.\scripts\limited.ps1 -Cmd 'cmake --build build --parallel 4 --target CoD3'

# the client, when a path has spaces in it
.\scripts\limited.ps1 -Exe CoD3.exe -ArgumentList '--game', 'X:\...\Call of Duty 3'
```

It samples every second and stops the process tree when the machine crosses
either trigger — 4096 MB free, or 90% used. The tree is killed rather than
just the parent because the client relaunches itself for the next mission and
would otherwise come straight back.

## Why it does not use a job object

A job object with `JOB_OBJECT_LIMIT_JOB_MEMORY` is the correct tool and was
the first implementation. It **silently does not work here**, which is worth
writing down because nothing reports the failure:

- `SetInformationJobObject` returned `TRUE`, `GetLastError()` = 203
  (`ERROR_ENVVAR_NOT_FOUND`), and reading the limit back out of the job gave
  `JobMemoryLimit = 0`.
- The agent harness already runs every process inside a job object, so this
  creates a **nested** one. On a nested job Windows accepts the limit-setting
  call and drops the limit on the floor.

Measured, both ways:

| run | result |
|---|---|
| python told to allocate, no limit | reached **24.3 GB** |
| same process, 2 GB job "limit" | reached **158 GB** |
| same process, this watchdog | stopped at **91.9%**, 6.8 GB free after |

The middle row is the whole point. The limit was never applied at all — it
was only printed. Anything that reports success without being measured is
worth distrusting, which is the rule this project already follows.

`scripts/limited.ps1` now carries this in its header comment so the next
person does not reimplement the job object version and assume it works.

## Verified behaviour

| test | result |
|---|---|
| 24 GB memory hog | killed at 91.9% peak, memory recovered to 6.8 GB free |
| hog, message formatting | `3629 MB free of 32618 MB (88.9% used)` — correct |
| real rebuild of `CoD3.exe` | completed, peak 9126 MB = **28%**, no false kill |
| `ninja: no work to do` | completed, 22.5%, no false kill |

The false-positive check matters as much as the enforcement one. A limit that
kills ordinary builds gets switched off within a day.

## Manual floor

Whatever is running, if the machine goes past 90% the client is the thing to
kill:

```powershell
taskkill /F /IM CoD3.exe
```

The client itself only allocates around 512 MB. On 2026-10-13 the machine hit
99% with the client running, but the cause was **another agent working in a
sibling project on `mws&box`, which was using 25 GB** — the client was a
contributor, not the culprit. Worth remembering before blaming the thing that
is easiest to blame.

## Build parallelism

`--parallel 4`, never 16. Sixteen exhausts 32 GB with
`LLVM ERROR: out of memory`. Given the memory pressure above, this is not
just a speed setting.