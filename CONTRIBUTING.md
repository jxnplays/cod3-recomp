# Contributing

Thanks for looking. This is a recompilation of Call of Duty 3 for Windows,
built on [XenonRecomp](https://github.com/hedge-dev/XenonRecomp), and it runs
the title's own code as native x86-64 rather than emulating it.

Two things are worth knowing before you start.

**You need your own copy of the game.** No game code, data, audio or shaders
are in this repository, and there is no `.xex` anywhere in it. The tools build
from a bare clone; `CoD3.exe` needs a legally owned Xbox 360 disc or dump.
[docs/SETUP.md](docs/SETUP.md) has the full walkthrough.

**Most of what is here is not ours.** The host runtime, the recompiler
configuration, the analysis tooling and the scripts are
[FrankHUN88453/CoD3Recomp](https://github.com/FrankHUN88453/CoD3Recomp)'s work
under BSD 3-Clause, used with his permission. `LICENSE` keeps his notice.
What this project has added is the direction, the tooling fixes that make it
build on a current machine, the measurements in `docs/`, and the remaster work.
[docs/PROVENANCE.md](docs/PROVENANCE.md) has the detail. If you are fixing
something in `CoD3Host/`, read that first — you may be working on his code,
and a patch that suits us may not suit everyone.

---

## Where things would be useful

Read [README.md](README.md#known-gaps) for the short list. Ordered roughly by
how much they unblock:

**Test the other fourteen levels.** Saint-Lô is the only one we have run
end to end. The other fourteen are compiled and linked and nobody has checked
that each one loads and plays. `COD3_MAP=<level>` in the environment will take
you straight to one — `docs/SETUP.md` has the variables and the button
scripting needed to get through the front end first. This is the single most
useful thing you could do.

**The frame rate is unexplained.** The same build and the same level have
been observed at 22, 138, 144 and 299 fps. Nobody knows why, and we would
rather say so than guess. `docs/SETUP.md` and the status lines in
`CoD3.log` are where the measurements are. If you can characterise it, that is
a real contribution.

**Mouse look does not turn the view.** A previous attempt to fix it was
reverted in full, because it made things worse. `docs/MOUSE.md` has the
measurements and, more usefully, the list of leads already exhausted — read it
before trying, so you do not walk one twice. The open lead is upstream in the
kernel, not in the input pump.

**Seventy-three failing instruction checks.** All sharing one root cause in
Xenia's four-argument `make_full_test_constant` macro and the `clrldi r3,r3,32`
inside it. `.\scripts\verify.ps1 -All`.

**The remaster itself.** An Escape-key settings menu, ultrawide and arbitrary
aspect ratios, uncapped frame rate, proper anisotropic filtering. This is
Phase 2 of the roadmap and nobody has started it.

---

## Before you open a pull request

Run the build. This catches more than you would think:

```powershell
git clone --recurse-submodules <your fork>
cd cod3-recomp
git -C tools\XenonRecomp apply ..\patches\XenonRecomp.patch
.\scripts\recompile.ps1 -BuildTools
```

The patch line is not optional. Without it the recompiler mistranslates the
game and the title will not boot. CI re-applies it to a pristine checkout on
every push, so it is known to apply cleanly.

CI runs on Windows and does two things: it checks the patch applies, and it
builds XenonRecomp and XenonAnalyse. It cannot build the game, because it has
no game data — so a change to the runtime is only as verified as your own
testing. Please say in the PR what you ran and what you saw.

If you touched anything under `CoD3RecompLib/config/`, say so particularly
loudly. Those files carry addresses the recompiler depends on, and a wrong one
produces code that compiles cleanly and misbehaves at run time.

---

## How pull requests work here

`main` is protected. Contributions go through a pull request and need one
approving review.

You may notice the maintainer commits to `main` directly and skip that. That
is deliberate: this is a project with one maintainer, GitHub does not count
your own approval, and requiring review of your own work would mean nobody
could commit at all. Contributors are gated; the maintainer is not. CI still
runs on every push either way, so nothing lands unverified.

If you would rather review your own PR than wait, say so in a comment and it
can be merged on that basis.

---

## Measuring rather than inferring

The one piece of advice that matters most here.

A number in this repository should be something someone measured. Early on a
gain in the mouse code was wrong by a factor of four, another by twenty, and a
third by forty-one; one change killed the mouse outright and another crashed
the title after fifty-eight seconds because the loop it changed also presents
frames. All of it came from adjusting constants until the behaviour looked
right.

The habit that would have caught all of it: write the tool that shows you the
number *before* changing anything. `scripts/mouse_measure.py` is the clearest
example — it came last, when it should have come first. It is kept in the
repository for that reason.

If you change something and cannot say what you measured before and after,
say that in the PR rather than claiming it works.

---

## Reporting a bug

A level name, what you did, and what happened. Frame rate and the contents of
`CoD3.log` help enormously. If it hangs or shows a black window, say whether
you ran `recompile_level.ps1 -All` — a host built without the level libraries
looks exactly like a driver fault and is the single most common cause.

Please do not ask where to obtain the game. You provide your own copy.

## Licence

By contributing you agree your work is published under the repository's terms:
BSD 3-Clause for the inherited work, MIT for everything else. See
[LICENSE](LICENSE).