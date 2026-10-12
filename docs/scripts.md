# The level scripts

CoD3 does not run GSC. Its level scripts are C++, written against a script
library Treyarch called "Broc" (`c:\cod\code\script\include\BrocEntity.inl`,
`Threads.inl`, `Strings.inl`, `Arrays.inl`, `Entrypoint.inl`,
`ExtendedEntity.cpp`, `Broc.cpp`), and compiled into each level's DLL: the
`sp/<level>/<level>.dll` that is recompiled into `CoD3RecompLib/levels`.
What the IW engine's scripts would be (`maps\_anim`, `_spawner`, `_drones`)
are C++ namespaces there: the Chambois DLL names `_anim::simple_anim`,
`chambois_drones::drone_combat_cover`, `chambois_event5::gunner_only_player_damage`
and 150 more in its messages. The script threads the recompilation runs
as host fibers (coroutines.cpp) are these functions waiting.

## The table

A level and the engine talk through one table of function pointers, and
the level owns it: it is in the level's data (0x89235308 in Chambois).
sub_824BB360 loads the level's DLL, calls its first export with
`&*(0x82A2A2A0)` and an engine block at 0x82A909C0, and the level puts its
table's address in 0x82A2A2A0 and copies three of the block's functions
into it (+0 to 0xAE0, +152 to 0xB78, +156 to 0xB7C). sub_824BB028 then
fills the rest:

- two lists of (byte offset, function) pairs in the executable's data,
  265 from 0x825F5638 and 222 from 0x825F5E80;
- one slot at a time in sub_824B9D70 (272 slots), sub_824B9798 (93),
  sub_824AE550 (59), sub_824B9560 (35), sub_824B7608 (32), sub_824AD028
  (12) and sub_824BB028 itself (14, eleven of them the same as the lists').

That is 1007 engine functions. The level fills 0xAE4 to 0xB74 with 35 of
its own that the engine calls back (below).

The level's code calls

    lwz r11, offset(table)
    mtctr r11
    bctrl

with the arguments already in registers, the ordinary calling convention:
an entity reference in r3 (the low 14 bits index the table of 4096
entities at 0x82A2DCD0, the rest a number that must match the slot's),
integers and handles in r4 to r10, floats in f1 up. An animation is one
32-bit value: the anim list's index in the table at `*(0x82A2A204)` in
the high half and the entry in the low half. Names and event names are
passed as hashes: h = h * 33 + c over the lowercased text, from 0.

The built-ins have no names anywhere, in the executable or in a loaded
level: the C++ headers' inline wrappers compiled down to the offsets. They
can be named only by what they do. `scripts/builtins.py` lists all 1007
with what can be read off each (the function, which list or function put
it in the table, how often Chambois and all fifteen levels call it, the
argument registers it reads, the functions it calls, the text it refers
to) and a name for every one. The list is
[builtins-table.md](builtins-table.md). The fifteen levels make 244,766
calls through the table; 713 of the 1007 are called, 294 never.

Where the name is IW's (setgoalpos, allowedstances, traversemode,
objective_add, attachpath, fireweapon...) the function does what IW's
does, found from its text, its fields and the values the levels pass.
Where no IW name fits, the name says what it touches.

The most used:

| offset | Chambois | all levels | does |
|---|---|---|---|
| 0x11C | 1138 | 16139 | whether an entity reference is valid |
| 0xAC8 / 0xAC4 / 0xAC0 | 964 / 309 / 294 | 15380 / 4657 / 4472 | new and delete on the script heap; new from the fixed pools |
| 0x3C | 626 | 12523 | thread: starts a script thread (with the .bro file, line and function name) |
| 0x10D8 / 0x10DC | 549 / 727 | 8173 / 10966 | registers an object with its destructor (a `BrocDtor<T>`, e.g. for `Broc::string`) on the running thread's stack, and lets it go |
| 0xCF4 | 579 | 8265 | the entity's origin |
| 0x74 | 456 | 7081 | notify (entity, event name hash) |
| 0x58 | 465 | 6864 | endon (entity, event name hash) |
| 0x4E0 | 434 | 6218 | the system clock |
| 0x140 / 0x144 | 332 / 189 | 4294 / 2581 | randomint, randomfloat (one linear congruential generator, ×214013 + 2531011) |
| 0x694 | 240 | 3525 | setanimknoball |
| 0x94 | 186 | 3465 | getent / getentarray (value, key) |
| 0x42C | 241 | 3423 | gettime, the level's time in milliseconds |
| 0x5DC | 230 | 3280 | plays a sound alias |
| 0x534 / 0xAD4 / 0xAD8 | 213 / 224 / 90 | 3049 / 2829 / 946 | prof_begin (a stub), assert and a script warning (both disabled) |
| 0x118 | 214 | 2690 | isalive |
| 0xD6C / 0xD70 | 199 / 113 | 2643 / 1545 | the entity's health, and setting it |
| 0xFD4 / 0xFD8 | 172 / 71 | 2525 / 765 | an actor's pose ("stand", "crouch", "prone", "back"), and setting it |
| 0xD24 / 0xD2C / 0xD7C | 132 / 143 / 116 | 2078 / 1963 / 1855 | the entity's target, targetname, angles |
| 0x1030 | 115 | 1431 | sets a sentient's goalradius |

The animation ones reach the functions in [animation.md](animation.md).

## What the table covers

By offset, roughly:

- 0x0 to 0x13C: printing, the C runtime's string functions, threads and
  waits, events, entity lookups (getent, getentarray, getnode), zones,
  sounds, traces.
- 0x140 to 0x1C8: math (random numbers, sin to atan2, sqrt, vectors,
  angles).
- 0x1CC to 0x248: objectives: config string 16 + index, an info string
  with "state", "str", "org", "ring", "ent", "height", "order"; child
  objectives in config strings 32 + n with "pobj".
- 0x24C to 0x770 (the lists): dvars, saved variables, statistics,
  spawning, models, sounds, effects, render settings, damage, the
  mission's end, HUD slots, animation, weapons, teams.
- 0x778 to 0x8DC: the AI. An actor is the entity's +532, its sentient
  +536: goals (setgoalpos, setgoalnode, setgoalentity), shooting,
  teleport, stances, animmode, orientmode, traversemode, grenades, the
  critical section.
- 0x8E0 to 0x90C: movers (moveto, movex/y/z, rotateto, rotatepitch/yaw/roll,
  rotatevelocity, movegravity, solid, notsolid).
- 0x910 to 0x9E8: the player (+528): weapons and ammo, flags, noclip,
  playlocalsound.
- 0x9EC to 0xA7C: vehicles (+540): paths, speed, the turret, fireweapon.
- 0xB84 to 0xBF8: C runtime helpers, name hashes, fog and render
  settings, scene lines.
- 0xBFC to 0xC48: HUD elements' fields (x, y, alignx, aligny, sort,
  fontscale, alpha, red, green, blue; 124-byte entries at 0x829B01B8).
- 0xC4C to 0x10A8: fields, a getter and a setter for each: path nodes,
  vehicle nodes, entities, actors, sentients.
- 0x10AC to 0x10D4: a table of 208-byte records at 0x82909990; 0x10D8 to
  0x10E4 the thread's destructor list and ending a thread.

## The fields

The executable keeps some field tables, (name, offset, type) rows, which
name the getters and setters:

| table | rows | what |
|---|---|---|
| 0x82065908 | 29 | entity fields: classname 564, origin 336, model 556, spawnflags 628, speed 716, target 580, targetname 572, health 776, angles 352, script_noteworthy 596, animname 604... |
| 0x825CC440 | 12 | path nodes: classname 0, type 40, spawnflags 44, targetname 48, script_noteworthy 52, target 56, on_goal 60, reservename 64, animscript 68, origin 76, angles 88, radius 92 |
| 0x825CC510 | 7 | vehicle nodes: targetname 0, target 4, speed 8, lookahead 12, script_noteworthy 16, origin 20, angles 44 |
| 0x825F4678 | 31 | vehicle types: turretHorizSpanLeft 352 ... turretRotRate 368, the cameras... |

The actor's and sentient's fields have no table; they are named by
their offset, type and the values the levels give them (goalradius, a
float at the sentient's +24, is set to 32, 64, 384, 512; maxsightdistsqrd,
the actor's +2228, to 1000000 and 90000).

The engine compares names by hash against a table filled at start:
`0x82A50E30 + n` holds the hash of the `Broc::string` at `0x82A555D0 + n`
("active", "activate", "angle deltas", "animdone"... 157 of them:
"crouch" +40, "face angle" +104, "gravity" +172, "stand" +392...).

## Events

- notify (0x74): a 20-byte event (+8 the name hash, +12 the entity) on the
  script VM's list (0x82A90128 +40, the count at +32) for the threads
  waiting on it, and the entity's handlers run at once. 0x78, 0x7C, 0x80,
  0x84 notify with an argument (an integer, a string, a float and an
  integer, a name hash).
- endon (0x58): a 32-byte record of type 3 (entity, name hash) on the
  running thread's list (+24); the thread ends when that notify comes.
- handlers (0x8C adds, 0x90 removes): (event name hash, level function
  hash) pairs on the entity's +516 list of 68-byte blocks, seven pairs a
  block. notify walks them (sub_8251BE50) and calls the level's slot
  0xB60 with the function's hash, the entity's number and the point.
- a thread that waits: 0x40 starts one waiting for an entity's notify.

A waiting thread's state is one of the RTTI classes `AeThreadState`,
`AeThreadWaitState` (wait, 0x4C), `AeThreadWaitFramesState` (0x50),
`AeThreadEntityNotifyState`, `AeThreadEntityNotifyTimeoutState`,
`AeThreadPakNotifyState` (0x54, a zone or bank to be ready) and
`AeThreadEntityNotifyMatchState`. The script VM is at 0x82A90128: +16 the
thread count, +24 the list, +80 the table of 256 thread handles, +2132
the running thread, +2136 the last one made. A thread object is 56 bytes
(+8 the function, +12 self, +16 flags, +20 its handle, +40 its objects
with destructors, 15 to each 128-byte chunk).

## The level's callbacks

What the level puts in 0xAE4 to 0xB74 (Chambois' functions; each level
has its own):

| offset | what the engine uses it for |
|---|---|
| 0xAE4 / 0xAE8 / 0xAEC / 0xAF0 | `ExtendedEntity::CreateExtendedEntity`, `DeleteExtendedEntity`, `MatchExtendedEntityKey`, `CopyExtendedEntity` (their names are in the level) |
| 0xAF4 to 0xB10 | a path node's extended keys: set and get a string, an integer, a float, a vector (sub_823CC9B8 sets them from the map: "pathnode field KEY=%s, VAL=%s will be inaccessable") |
| 0xB14 to 0xB30 | the same for vehicle nodes (sub_8245E2E8) |
| 0xB34 | an anim script by name hash (from sub_824F6BF0) |
| 0xB38 / 0xB4C | an animation by (list, entry) and by (animtree name, anim name) |
| 0xB3C | whether the level's animations are ready (sub_82498E50) |
| 0xB40 | the level's animtrees ("drone_animtree", "flak88", "generic_human", "mg42", "panzerIV", "stuka"...) |
| 0xB44 / 0xB48 / 0xB70 | nothing in Chambois |
| 0xB50 / 0xB54 | two of the level's strings |
| 0xB60 | runs a level function by the hash of its name (`_spawner::friendly_wave_callback`, `chambois_drones::drone_death`...): one switch over all of them |
| 0xB64 | whether a level function with that hash exists |
| 0xB68 / 0xB6C | a virtual call, and a copy of a structure |
| 0xB74 | the level's shutdown: its objects deleted (from sub_8256D978) |

## The mission's end

The game state at 0x82A4E790: +2744 the time of a failure, +2752 an end
requested, +2756 "gamefinished", +2760 success, +2764 failed, +2768 the
failure's message, +2772 and +2776 times, +2780 the next level's name (256
bytes). 0x4A4 (missionsuccess / changelevel) writes the next level there;
0x4A8 (missionfailed) the reason.

## How it was read

- The level DLLs dumped at run time (`COD3_MEMDUMP=89000000,800000,38,<file>`,
  one run per level), so their text, their data and the table they own can
  be read.
- The engine's data (`COD3_MEMDUMP=82A00000,100000,...`) and the heap the
  name strings are on, for the hash tables.
- The calls: `scripts/builtins.py` follows the register holding the
  table's address through each level function, so a virtual call
  (`lwz r11,8(r11)`) is not counted; the arguments set before each call
  (strings from the level's dump, integers, hashes looked up against every
  string in the dumps) show what a built-in is given.
- `COD3_PROBE=1` (CoD3Host/probe.cpp) runs guest functions with known
  inputs at the first game start: that is how the math ones were checked.
