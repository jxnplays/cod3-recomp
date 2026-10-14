# How changes get made here

Written after a week in which four changes shipped described as "verified"
and were wrong. None of them stopped the build; all of them were found by
someone else. This is the process that stops that happening again.

## The rule

**Nothing is described as verified unless it was run.**

A green `scripts/selfcheck.ps1` writes the evidence to `.git/SELFCHECK`:

```
MEASURED: 88e8ef5
  checks:  12, all passed
  at:      2026-10-14 03:46:41
  game:    not run - logic and defaults only
```

The commit message quotes that rather than asserting it. A failing run deletes
the file, so a stale one cannot be leaned on.

**This exists because the author writing `MEASURED` about their own work is
worthless.** That is the exact failure this file exists to fix: a scheme where
the person whose error was overclaiming marks their own homework would have
been marked `MEASURED` anyway. It had already failed to appear once in thirty
commits. The script writing the file is what makes the claim mean something.

Two labels remain the author's, and are weaker for it:

```
REVIEWED:  an adversarial reviewer went through it and found nothing
REASONED:  only read the code - NOT TESTED
```

Most of what went wrong this week was `REASONED` written up as `MEASURED`.
The suffix test is the clearest case: eleven characters compared against a
four-character string, described as working, and it had never been run once.
Reading three lines is not the same as executing them.

## Review is not optional, and it is not a favour

Every non-trivial change gets an adversarial review before it is called done.
"Adversarial" is the important word: the reviewer is told that its default
hypothesis is that the change is **wrong**, and is asked for file and line
evidence rather than an opinion.

That framing is what made review worth its cost. Reviewers operating that way
found, in one week:

- a sensitivity curve that made a 0.3 setting 1.83x faster and was the jank
- `Guest::Shutdown()` freeing the memory guest threads were executing from
- a field list replacing `memcmp` that silently dropped nine settings
- `XGetVideoMode` left behind while the other two mode reporters were merged
- a job object limit that reported success and enforced nothing

None of those would ever have been caught by the build passing.

**Trivial changes skip it.** A comment fix, a typo, a doc correction that
only removes a false claim. Everything touching behaviour, defaults, memory,
or anything that runs on the player's machine goes through review.

`REVIEWED` is the author's claim, not a record, and nothing here enforces it -
only the script writes `MEASURED`. In practice the reviews that mattered were
run because the author expected to be wrong, not because a rule said to.

## `scripts/selfcheck.ps1` before every commit

```powershell
.\scripts\selfcheck.ps1              # logic, defaults and documentation
.\scripts\selfcheck.ps1 -Game        # and boot the game on the default path
```

Measured at about two seconds including PowerShell's own startup. It is not
under a second, which is what an earlier draft of this file claimed without
running it.

`-Game` checks three things and no more: the process is still running after 75
seconds, it took no guest fault, and `ps2` went out exactly once. It does **not**
check that anything was drawn, or how fast - a client that boots to a black
screen and spins at 5 fps passes it.

It exists because the failure mode was never "the build broke". It was "the
build was fine and the logic was wrong". The checks cover the specific things
that went wrong:

| check | what it stops | proven? |
|---|---|---|
| `logic.h` compiled and run: the `.xex` suffix test and the sensitivity gain | a relaunch that can never match; a curve that changes the range it was not meant to | yes - reverting either bug was tested and the check fails |
| every `g_edit` field is in the dirty check | Apply discarding a setting | yes - deleting a comparison was tested and the check fails |
| the 16:9 floor on the present path | a stretched picture on taller windows | pattern match only |
| one `Render::VideoMode`, no hardcoded 1280 | the three mode reporters drifting | counts calls; it does **not** prove each reporter is right |
| `fprintf` conversions against arguments | undefined behaviour from a half-edited format | catches too few; misses extra arguments, and misses a format split across adjacent literals |
| every ini key is read back | a key written and never read | string presence only - it does not prove the value is used |
| aspect, field of view and sensitivity defaults | those three defaults moving | 3 of roughly 15 clamps in `settings.cpp`; the rest are uncovered |
| `ps2` sent once | hundreds of commands a second on the title's buffer | pattern match only |
| no doc claims a feature works | the specific false claim that was made | **one exact sentence.** Any rewording sails through. It is a tripwire, not a detector |
| every relative doc link resolves | a link to something deleted | only `.md`, `.png` and `.txt` |

The "proven?" column is the honest one. Four of these were tested by reverting
the bug they name and confirming the check went red. The rest are pattern
matches, which is weaker, and saying so is the point - a check presented as
coverage it does not have is the thing that made the first version of this
package worse than nothing.

If a check fails, the change is not done. If a check cannot run, that is a
failure too - a check that silently cannot run looks exactly like one that
passed, which is how the memory guard in `docs/MEMORY.md` shipped reporting
success while enforcing nothing.

## When a check is wrong

If a check fails, fix the change or fix the check - but only after establishing
which is wrong, and say which in the commit. "Changed the test" is a sentence
that should be rare enough to be noticeable.

The first version of `selfcheck.ps1` was exactly this failure: it compiled a
hand-copied paraphrase of the logic it claimed to guard, so reverting the real
bug changed nothing and every check still passed. Five of its thirteen checks
could not fail for the reason their label claimed. A check that cannot fail is
worse than no check, because it reports a regression as covered.

So: when a check is added, revert the thing it guards and confirm it goes red.
`CoD3Host/logic.h` exists for this reason - the host and the check compile the
same header, so there is no copy to drift.

## The three things a check cannot do

Stated plainly, because these are where the failures actually came from:

1. **Only a person playing it can say whether it feels right.** The jank was
   reported by a person. No check can tell that a curve is wrong in a way that
   matters.
2. **A check cannot know what the code was supposed to do.** It compares the
   change against the claim. If the claim is wrong, the check passes.
3. **Review is not a substitute for reading the thing you changed.** The
   reviewers were useful because they were given a specific claim to attack.

## What I owe you, specifically

- If I say "verified", you should be able to ask "ran it or read it?" and get
  an honest answer the first time.
- If a check fails, I fix the change rather than the check, unless the check
  is demonstrably wrong.
- When I cannot measure something, I say so in the commit message rather than
  leaving it implied.