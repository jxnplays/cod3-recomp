# Resume log

State at the point the machine was rebooted to free memory. Everything below
is either finished and pushed, or is a lead with the evidence that got us
there. Nothing here is inference dressed up as fact unless it says so.

**HEAD `f84b762`, pushed, 0 unpushed.** CI runs on every push.

---

## Stopped because of memory, not because of a decision

RAM hit 99.2% used with the client running (0.26 GB free of 31.85 GB) and the
PC was hard locking up. The client was killed (`taskkill /F /IM CoD3.exe`);
memory recovered to 4.38 GB free. **Standing rule from the user: if PC memory
exceeds 90%, throttle.** Do not launch the client while the machine is already
under pressure, and watch free memory before and during long scripted runs.

The client itself allocates around 512 MB, so the pressure came from running
several stacked launches during the `ps2` investigation plus the build's
`--parallel 4` LLVM processes. Treat those as a pair, not as separate things.

---

## Done and pushed in this session

### The multiplayer crash (the real bug, and it was not networking)

Selecting Multiplayer from the main menu closed the game. The log said why:

```
The game asked to launch another title: "d:\codmp_xenonf.xex", flags 0x00000000, from 0x82514710.
  guest call chain, innermost first: sub_8234EBA0 <- sub_825788DC <- ...
Shutting down.
```

`CoD3Host/kernel_xam.cpp` handled exactly one relaunch target, `default.xex`
(the next mission), and fell through to a deliberate shutdown for anything
else. Multiplayer asks for `codmp_xenonf.xex`, which is the same request for a
different file — and the file is present in the installed game (6.8 MB).

`Relaunch()` was already fully generic: it relaunches the same exe, passes
the path through `launchdata.bin`, and strips scripted env vars. Only the
gate above it was wrong. It now relaunches any `.xex` and still refuses the
dashboard, since there is no dashboard to go to.

**Not yet verified by hand.** The rebuilt exe was launched at the main menu
and the user was asked to pick Multiplayer, but the machine rebooted before
the answer came back. First thing to check after the reboot.

### The custom button artwork is gone

`button_icons.cpp`, `button_icons.h`, all 38 PNGs under `assets/buttons/`,
the build copy, the WIC decoder wiring, the picker, the per-keybind pad
icons, `PadEquivalent`, and the `button_icons` setting are all removed.
Controller menu navigation is kept — it was unrelated to artwork.

**Source and exe both verified clean:** `Select-String` finds no `Button
icons` / `buttonIcons` / `PadEquivalent` / `kSetNames` in `overlay.cpp`, and
no `Button icons` string in the built `CoD3.exe`.

Worth remembering: for a while the user kept reporting the old art was still
visible while it was actually **the title's own Activision glyphs** in-game.
The 38 deleted PNGs were still tracked in git at that point, which was also
true but only affected the F11 menu.

### The ImGui ID clash

Every "Set" button in the mods menu was captioned `"Set"` with no `PushID`,
so ImGui saw sixteen widgets sharing one identifier and printed
"16 visible items with conflicting ID!" over the menu. Fixed by pushing the
row index around each one.

### Two bugs found but deliberately not fixed yet

- `dirty = memcmp(&g_edit, &applied, sizeof(Settings::Values))` compares
  **struct padding**, which is uninitialised memory. Apply can therefore be
  spuriously enabled and write stale values back — this affects every
  setting, not just the button icons. Needs a field-by-field comparison.
- `settings.cpp` has no range validation on load, which is how a bad value
  could survive into the ini.

---

## The glyph investigation — where it got to

The user's goal: **make the in-game button prompts switch between real Xbox
and real PlayStation**, the way another user of this exact repo apparently
does. They sent a screenshot showing a magenta/pink **square** where COD3's
usual blue **X** prompt appears — PlayStation's SQUARE glyph, drawn by the
title, in-game. An earlier claim from me that this was impossible was wrong,
and the screenshot is the proof.

### Ruled out, with evidence

**`ExGetXConfigSetting`** — the Xbox dashboard's settings store, the obvious
place a 360 would keep a button layout. Now instrumented to log every query.
The title makes **exactly two calls, ever**, both at startup:

```
ExGetXConfigSetting: category 0x0003 setting 0x000A -> 0x00040000   (video flags: widescreen)
ExGetXConfigSetting: category 0x0003 setting 0x0009 -> 0x00000001   (language: English)
```

No button-layout query. The dashboard's store is not the source.

**Grep proves nothing here.** `cg_fov`, which is known to work, returns zero
hits in the recompiled code. These are data tables read by pointer. Control
test already performed — do not repeat it.

### The live lead: the `ps2` cvar

`ps2` is a genuine entry in the title's own cvar name table — sorted between
`ProfileWriteData` and `r_clear` in `docs/cvars.txt`, 198 names. This is the
shape of the switch that picks PS2-era prompts over Xbox-era ones.

- `set ps2 1` **reached the command buffer**: `video: "set ps2 1" put on the
  title's command buffer`
- A memory scan for `ps2` hit the unrelated `.ps2tex` / `.ps2lm` / `.ps2font`
  file-extension table first and stopped at its 20-result limit, so the cvar
  entry itself was never shown. Not evidence against it.

**Never confirmed visually.** Every attempt to screenshot the effect was cut
short by the reboots. The open question is whether it must be set *before*
the font/UI loads — at t=90s the UI is long since up, and if the glyph set is
chosen once at font init, a live `set` would do nothing and look like a
failure of the cvar.

**How to settle it on the first clean run:** set it as early as possible
rather than at t=90s. The title execs `d:\config\xenon_a.cfg` and
`d:\config\ts_def.cfg` at startup, so putting `set ps2 1` into the game's own
config is the reliable way to have it set before the UI loads. That mutates
the installed game folder, so ask first and keep it to one line that is easy
to revert.

---

## The four button layouts, which are a separate thing from glyphs

The user asked that the menu's button list be **strictly layouts**. These are
the four the title ships, and they are *binds*, not artwork:

| file | name | what it does |
|---|---|---|
| `config/xenon_a.cfg` | Default | |
| `config/xenon_b.cfg` | Lefty | reverse triggers and shoulder buttons |
| `config/xenon_c.cfg` | SCI-FI / Tactical | stance on left thumbstick, binoculars/holdbreath to stance |
| `config/xenon_d.cfg` | Legacy | COD2 Finest Hour controls, with stancing |

Plus `mp_xenon_a..d.cfg` for multiplayer. **No layout selector has been built
yet** — that is the next piece of menu work, and it should send the title's
own `exec`/`bind` commands rather than touching artwork.

---

## Standing constraints

- Graphics-only focus. Priority: ultrawide + HUD scaling, FSR/XeSS + CAS,
  SSAO/GTAO, soft shadows, colour grading/LUT, SMAA, FOV to 180.
- `cod3-reborn-features.md` in Downloads is a **goal sheet only**. Do not
  implement from it yet.
- Mouse work was reverted in full and stays reverted.
- Mod menu is scoped to cvars plus verified dvars. God/noclip/give/spawnables
  are excluded — no player-state cvar or dvar exists for them. `sv_cheats` +
  `god` is still unproven and needs a human to take damage.
- 210 dvars found at guest `0x82061400`, including `sv_cheats`, `g_gravity`
  (800), `g_speed` (190), `g_gameskill`, `g_knockback`, `g_spawnai`,
  `g_aiEffectiveRange`, `g_aiMissRange`, `g_vehControlMode`,
  `g_weaponrespawn`, `player_deathInvulnerableTime/ToMelee`. Verified
  `set g_gravity 0` executes (parsed record `set g_gravity 0.` at guest
  `0x829EE0E4`).
- Measure, don't infer. That rule has now caught three separate wrong
  answers this session, and one of them was mine.
- No generated code (`CoD3RecompLib/ppc/`, `levels/`) or game data committed.
- `tools/XenonRecomp` is a submodule pinned at `ddd128b`; it shows `-dirty` in
  `git status` from build artefacts. **Not committed, deliberately.**
- All XenonRecomp changes go through `tools/patches/XenonRecomp.patch` via
  **cmd** redirection — PowerShell `>` writes UTF-16 and git reports
  "No valid patches in input".
- Console output that looks like mojibake is the console, not the files. This
  has been misread as file corruption twice.

---

## First moves after the reboot

1. Confirm the client is gone and memory is back to normal before anything
   else launches.
2. Launch, pick Multiplayer from the main menu, and confirm the relaunch fix.
   This is unverified and it is the most user-visible thing outstanding.
3. Settle `ps2`. Get a prompt on screen with it set as early as possible and
   compare against a run without it. Ask before writing to the game's
   `config` folder.
4. Build the **layout** selector for `xenon_a/b/c/d` — no artwork.
5. Fix the `memcmp` dirty check and add range validation on load.
6. Ultrawide, once the above is settled: `CoD3Host/render_d3d11.cpp` line
   ~1150 still hardcodes `const float aspect = 16.0f / 9.0f;`, which is the
   sole aspect handling in the whole client and letterboxes anything else.
   `cg_widescreen` exists in the cvar name table but is referenced nowhere in
   game code, and no aspect config exists in `config/*.cfg`.

---

## Build command

cmake/ninja/LLVM are not on PATH:

```
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
  && set PATH=<BT>\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;<BT>\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;C:\PROGRA~1\LLVM\bin;%PATH%
  && cmake --build build --parallel 4 --target CoD3
```

**`--parallel 4`, never 16** — 16 exhausts 32 GB RAM with
`LLVM ERROR: out of memory`, and given the memory problem that just cost us,
this matters more than it used to.

Multi-line commit messages must go through `git commit -F <file>`. Passing
`-m` with a multi-line string from PowerShell splits it into a hundred bogus
pathspec errors.