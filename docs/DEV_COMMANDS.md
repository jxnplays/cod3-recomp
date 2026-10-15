# The title's debug commands, and how this file was wrong twice

Written 2026-10-15. Rewritten the same day. The first version of this file
said the cheat commands did not exist. That was wrong, and the way it was
wrong is the part worth keeping.

## Wrong the first time: the wrong table

The claim was that the title has no god mode, no no-clip, no notarget and
nothing to spawn with, and therefore nothing for the menu to put a button on.
That claim was in the console help, in `docs/cvars.txt`, and in the menu.

It came from one table. `docs/cvars.txt` is 188 names read out of guest
memory at `0x82078D00`, and it really does contain no player-state variable —
but it is ONE table. `COD3_SCANMEM` finds text anywhere in guest memory, and
the usual names land in a second table at `0x82065D60..0x82065F20` that the
dump never covered. There is a third table too: `cg_fov` is at both
`0x82078F00` and `0x828830C0`, in different layouts. "The variable table" was
never one thing, and generalising from it was the error.

## Wrong the second time: the wrong names

The first search found `god`, `noclip`, `notarget`, `invinc`, `take`,
`give`, `kill`, `setviewpos`, `jumptonode`, `where`, `ufo`. Those strings are
real and they are in the table.

They were tested. `setviewpos x y z yaw` is the best probe available,
because a moved player is something the capture harness can see, so:

    COD3_CMD="30:setviewpos 100 100 100 0"     # sent at 30s, not at startup
    COD3_SHOT_EVERY=400                         # a frame every 8s

The camera did not move. Same village, same frame, before and after. Zero
guest faults. From that, this file concluded: *the command table is present
and its handlers are not.*

**That conclusion was wrong, because those were not the command names.**

The names are `GAME_GODMODEON` and `GAME_GODMODEOFF`, `GAME_NOTARGETON` and
`GAME_NOTARGETOFF`, `GAME_NOCLIPON` and `GAME_NOCLIPOFF`, `GAME_UFOON` and
`GAME_UFOOFF`, at `0x82065CB8..0x82065D30`. Alongside them sit `GAME_UFOOFF`,
`fog %s`, `%f %f %f %f %f %f %f`, `all`, `health`, `weapons`, `ammo`,
`allammo`, and the string `GAME_CHEATSNOTENABLED`.

The short names are the *arguments*, not the commands. `setviewpos` was in
that table as a command; `god` was not, and there was no way to tell the
difference by looking at a string table.

`GAME_CHEATSNOTENABLED` is what the title prints when it refuses, so
something has to open the gate first. It is `sv_cheats`, in the title's
variable table at `0x82061F48`.

## What was actually measured

**`fog off` works.** Sent into live gameplay at 42 seconds, the atmospheric
haze left the frame: the mid-distance went from a grey wash to individually
defined houses, rooflines, tree foliage and grass. That is a visible,
unambiguous change in a static scene, and it is the same dispatch table the
`GAME_*` commands live in.

**`sv_cheats 1` followed by `GAME_GODMODEON`, `GAME_NOCLIPON`,
`GAME_NOTARGETON`, `give ammo`, and their OFF counterparts**: all queued
through the title's command buffer, 88 seconds of play, window alive, **zero
guest faults**.

**None of their individual effects was measured.** Proving god mode needs the
player to be shot. Proving no clip needs the player to walk into a wall. This
harness has no way to simulate either, and the HUD's ammo counter — the one
thing `give ammo` would visibly change — fades out on its own schedule, which
made the one automated attempt unreadable.

So: the dispatch works, the gate is known, the commands do not fault, and
their effects are for a human to confirm in ten seconds of play. That is what
the menu's Cheats tab is for, and it is labelled that way rather than
claiming a measurement that was not taken.

## What is in the menu

    God mode            GAME_GODMODEON / OFF
    Enemies ignore you  GAME_NOTARGETON / OFF
    Walk through walls  GAME_NOCLIPON / OFF
    Clear the fog       fog off / on          <- measured
    Give ammo           give ammo
    Give everything     give all

All of them start off. `sv_cheats 1` goes out once per level, the first time
any of the gated ones is switched on — `fog` is a level author's control and
is not gated, which is why it is the one that could be measured.

`Cheats::LevelStarted` turns every one of them off when a level loads. It is
called from `Kernel::QueueConsoleCommand` on the exact prefix `"spmap "`,
because that is how every level in this title is loaded — the chapter select,
`COD3_MAP`, and anything typed into the console all arrive through it. God
mode in one level being god mode in the next one is not a thing anybody
asked for.
