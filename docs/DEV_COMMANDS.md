# Developer commands — found, and why they are not in the menu

Written 2026-10-15, because the answer here was wrong three times before it
was investigated, and the wrong answer shipped in a menu.

## What I claimed

That the title has no godmode, noclip or spawning, and that therefore there is
nothing for the mod menu to surface. That was repeated in the console help, in
`docs/cvars.txt`, and in the menu itself.

It was based on one table. `docs/cvars.txt` is 188 names read out of guest
memory at `0x82078D00`, and it genuinely contains no player-state variable.
Generalising from one table to the whole title was the error.

## What is actually there

`COD3_SCANMEM` finds text anywhere in guest memory. Scanning for the usual
names lands in a **second table**, at `0x82065D60`..`0x82065F20`, which the
188-name dump does not cover:

    setviewpos   jumptonode   where   kill   ufo   noclip
    notarget     invinc       god     take   give  mr
    entity_stats                                     ("unrecognized command")

and nearby, `timescale` at `0x82066288` and `com_maxfps` at `0x82068764`.

There is also a third table: `cg_fov` appears at both `0x82078F00` and
`0x828830C0`, in different layouts. So "the variable table" was never one
thing.

## Do they work? Tested, and no.

`setviewpos x y z yaw` is the best probe available: it moves the player, and a
moved player is something the capture harness can see.

    COD3_CMD="30:setviewpos 100 100 100 0"     # sent at 30s, not at startup
    COD3_SHOT_EVERY=400                         # a frame every 8s

The camera did not move. The scene before and after is the same Saint Lo
village, frame for frame. Guest faults: 0.

`COD3_EXEC` was tried first and is a weaker test - it fires at startup, before
the player exists, so a no-op there proves nothing. `COD3_CMD` is the one that
means anything.

The strings are there and the code behind them is not. That is ordinary for a
retail build: the command table is often left in while its handlers are
stripped, so the title recognises the name and does nothing with it.

## Consequence, and it is the whole point

**They are not going in the menu.** Thirty-four variables were added to the
Mods tab on 2026-10-14 on the strength of their names being real, and a play
test showed most of them did nothing and two of them broke the compass. Adding
fifteen cheat commands on the strength of their strings being present would be
the same mistake with a worse result, because a cheat that silently does
nothing looks exactly like a cheat that is not implemented.

## What would make them real

Not a menu change. Something that either patches the guest's command dispatch,
or writes the player state directly. Both are real pieces of work and neither
is a weekend. The honest position is: the door is visible, it is locked, and
nobody has picked the lock yet.
