# Console commands — what the mod menu can act through

Written before the first line of the mod menu, because the menu's shape
follows from what is actually in here rather than from what looks
possible.

## How a command reaches the title

Everything the host sends goes onto the title's own command buffer, the
way the chapter select runs `spmap`:

    Kernel::QueueConsoleCommand(text)   ->   Cbuf_AddText (sub_824644D0)

`Cbuf_AddText` separates commands on **semicolons**. It drops what does
not fit in the buffer, so a long run can be lost if it arrives all at
once. `scripts/recompile.ps1` and the settings push newline-separated
blocks through the same door, which works because `Settings::TitleCommandsChanged()`
already hands over finished lines.

Two environment hooks, both proven working on this build:

| | |
|---|---|
| `COD3_EXEC="a;b;c"` | commands at startup |
| `COD3_CMD="second:cmd;second:cmd"` | commands at those seconds |
| `COD3_MAP=name[,second]` | start a level |
| `COD3_SCANMEM=text` | report where text sits in guest memory |

`COD3_EXEC` was broken until 6ab2f11: it turned `;` into `\n` and sent
the lot as one command, so the title read a first token of `40:cg_fov`
and ran nothing. Verified fixed — three commands now arrive as three.

## The title has no console output

`Com_Printf` is compiled out of this build. The overlay console says so
(`overlay.cpp`) and only ever shows the host's own side of the exchange.

**Consequence, and it is the good kind:** a menu does not need to read
anything back. Toggles are fire-and-forget, and the menu tracks what it
has set itself. This removes the need for a cvar query interface
entirely — the single largest piece of work a mod menu normally needs.

It also means a command that the title rejects is **silent**. A menu has
no way to discover that a command failed, so the command list has to come
from measurement rather than guesswork.

## `set` is the form that works

    set cg_fov 30          works
    cg_fov 30              not the pattern to use

`settings.cpp` uses the `set` form for every cvar it pushes
(`cg_fov`, `r_lodscale`, `aim_slowdown_enabled`), and it is what the
title parses. Use it.

## The cvar table: what actually exists

Read out of the title's own memory with `COD3_DUMP=82078D00:8192`. The
cvar name table sits at guest `0x82078D00`..`0x82079000`, and another copy
of it — the one that takes new names — is at `0x8289D483`. 188 names came
out. The complete list is in `docs/cvars.txt`.

**There is no `god`, no `noclip`, no `notarget`, no `timescale`, no
`com_maxfps`, no `sv_` or `cl_` or `g_` or `dmp_` prefix at all.**

So the commands listed in the overlay console's help text — `god`,
`noclip`, `give all`, `timescale`, `seta com_maxfps` — are **not in this
table**. That help text was written by the inherited author and is not
evidence. This is the single most important thing found so far, and it
contradicts what the help text implies.

What is there:

| Group | Count | Examples |
|---|---|---|
| `cg_` client graphics/HUD | 29 | `cg_fov`, `cg_widescreen`, `cg_letterbox`, `cg_hudAlpha`, `cg_drawGun`, `cg_hudCompassVisible` |
| `r_` renderer | 48 | `r_fullbright`, `r_fog`, `r_nocull`, `r_norefresh`, `r_shadows` equivalents, `r_showtris`, `r_LightScale` |
| Screenshots/debug | — | `screenshot`, `screenshotHigh`, `levelshot`, `saveshot`, `stats`, `gfxinfo`, `sys_cpuid`, `r_meminfo` |
| Script/console verbs | — | `set`, `setCameraOrigin`, `startCam`, `stopCam`, `viewpos`, `fade`, `openvictoryscreen` |
| Weapon verbs | — | `weapprev`, `weapalt`, `weaponslot`, `noweapon`, `weapon` |
| Misc | — | `friendlyfire`, `black`, `white`, `Rain`/`Sky`, `exploder_stay` |

### What this means for a mod menu

**There is no player-state cvar.** Nothing in the table changes health,
ammo, money, score or position. So the classic private-server menu items
— god mode, infinite ammo, give weapon, teleport, noclip — have nothing
here to switch on. They would need to reach the game through some other
door than the console.

That door may exist. `setCameraOrigin`, `startCam`, `stopCam` and
`viewpos` are camera commands, and `cg_widescreen` shows this table does
contain gameplay-facing state. But it is not the cvar table, and finding
where player state lives is real work with a real chance of not working.

The honest position: **the menu's first version should be the `cg_` and
`r_` groups, which are real, proven, and numerous** — a HUD and graphics
tweaker, like the "Vision Menu" and "Menu Customization" entries in the
menus this is modelled on. Gameplay cheats are a second phase that depends
on finding the player-state path, and if that search fails they are not
shipped.

## Commands confirmed

Read out of the title's own resources, not guessed:

| Command | Where |
|---|---|
| `set <cvar> <value>` | `config/*.cfg`, and the settings push |
| `spmap <level>` | the chapter select's own command |
| `map_restart` | overlay console help |
| `timescale <n>` | overlay console help |
| `god`, `noclip` | overlay console help |
| `give all` | overlay console help |
| `seta com_maxfps <n>` | overlay console help |
| `togglecl_paused` | `config/default.cfg` (`toggle cl_paused`) |
| `togglemenu` | `config/default.cfg`, bound to ESCAPE |
| `toggleconsole` | `config/default.cfg`, bound to backtick |
| `bind <key> <cmd>`, `unbind <key>` | `config/default.cfg` |
| `bindaxis`, `stickalias`, `buttonalias`, `clearallaliases` | `config/ts_*.cfg` |

`give all` needs testing — it may want a weapon name rather than `all`.

## How a command was proven to land

The console gives no output, so `COD3_SCANMEM` was used to watch guest
memory. Setting `cg_fov 30` produces, at guest `0x829F0A34`:

    set|cg_fov|30|e into Saint Lo.\state\4\org\-5525 3670 508\ring\1

That is the title's parsed command record — verb, cvar name, value — and
the game state it changed. The command was not merely delivered; it was
understood and executed.

**Control test.** `set cg_nosuchcvar 30` does not produce a parsed
record. The only trace is the name appearing once in the cvar name table
at `0x8289D483`, which is the list of known names, not a value. So a
fabricated cvar is distinguishable from a real one, and the name table at
`0x82078F00` is a complete index of what the title knows.

That table is the next thing to read out in full: it is the authoritative
list of what a mod menu can actually offer.

## Not yet established

- Whether `god`, `noclip`, `give`, `timescale` exist at all. They are
  listed in the overlay console's help text, which was written by the
  inherited author, not proven on this build.
- Whether multiplayer works. Most of the categories in the private-server
  menus this is modelled on — bots, gametypes, spawnables, admin — are
  **server** authority, not client cvars, and would be empty regardless of
  how good the menu is.

## Not overlapping the settings panel

Checked `overlay.cpp` for every control already present. F11 is entirely
graphics, input and keybinds: renderer, window mode, resolution,
filtering, anisotropy, AA, texture quality, model detail, aim blur,
console depth, native command lists, write watching, VSync, frame rate
cap, FPS and stats overlays, raw mouse, aim assist, controller support,
and the key rebinding grid.

**No gameplay mod exists there**, so a mod menu duplicates nothing. In
particular `cg_fov` is already a settings slider and stays where it is.