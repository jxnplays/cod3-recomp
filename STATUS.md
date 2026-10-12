# Call of Duty 3 static recompilation — state of the project

Call of Duty 3 never shipped on PC. This project does not emulate it: it
translates the Xbox 360 PowerPC executable into C++ with
[XenonRecomp](https://github.com/hedge-dev/XenonRecomp), so the game code can be
compiled as a native x86-64 binary. That is the same approach
[Unleashed Recompiled](https://github.com/hedge-dev/UnleashedRecomp) took for
Sonic Unleashed.

## Where it is (September 2026)

Every level plays: menus, loading, the cutscenes, the fighting, the mission
title cards, checkpoints saved and loaded, mission failed and restarted, at 30
to 60 frames a second through a Direct3D 11 backend that runs the title's own
shaders translated to HLSL. All fifteen level DLLs are recompiled and linked
in (`recompile_level.ps1 -All`), each level's functions prefixed with its name
because every DLL is based at the same address.
Keyboard, mouse and pad work; a settings menu (F11) holds the keys, the mouse
sensitivity, the render scale and a frame counter; F12 takes a screenshot.

What it took, beyond what the sections below describe, in the order found:

- **The swap packet.** `VdSwap` must fill the sixty four words the driver
  reserves for it; left empty they were parsed as whatever the memory held,
  which included impossible waits on the display scaler's registers, and every
  such wait was a five second stall.
- **Two-operand scalar ALU operations** read lanes w and x of the shared third
  operand when the vector operation takes two operands (Xenia's rule) but lanes
  z and w when it takes three (mad, the conditional moves, dp2add). Both cases
  appear in this title; neither open translator has the second.
- **Strips end at the reset index** (`VGT_MULTI_PRIM_IB_RESET_INDX`, enabled by
  bit 21 of `PA_SU_SC_MODE_CNTL`). Widening the title's 16-bit indices to 32
  had turned the cut into a vertex, and every grass blade was joined to the next.
- **An 8_8_8_8 texture's x component is its lowest byte,** blue; the title's
  swizzle 0x60A puts the red first. Reordering the bytes as well swapped them
  back.
- **`RB_MODECONTROL` = 5 is colour and depth** for this title, not depth only.
- **One frame of latency.** The vertical blank runs at sixty hertz here whether
  or not the GPU has kept up, so `VdSwap` waits until the GPU has reached the
  swap before it; two frames ahead, the title overwrote the dynamic vertex data
  the GPU was still drawing from, and the cutscene's letterbox bar flickered.
- **Saved games** are folders under `saves/content`; the storage device the
  title asks for is that folder, mounted under the root name the title gives.
- **The fence counter is the fences' to write.** The title's render thread
  waits on the first word of its write-back block until it equals the number
  of the last command buffer it submitted; every indirect buffer ends with a
  fence that writes its number there. From the days before the fences were
  carried out, the command thread was still writing the submitted number there
  itself every millisecond, which told the title the GPU had finished
  everything the moment it was handed over. The title then wrote the next
  frame's vertices and constants over memory the command thread had not read
  yet: a triangle across the whole screen in about one frame in twelve, found
  by dumping every third frame of an eighty second run and scoring each for
  flat area (`scripts/flatframes.py`). `COD3_FENCELIE=1` brings it back for
  comparison.
- **A pixel program is given its pixel's position** when `SQ_PROGRAM_CNTL`
  bit 18 asks, in the register `SQ_CONTEXT_MISC` bits 8-15 names. The soft
  particles read the depth under themselves there; without it they read the
  top left corner of the depth, and the smoke of a level blinked on and off
  as the tree in that corner swayed (Saint-Lô's first view: visible a quarter
  of the time, in bursts). In a fight it was worse: the dust and smoke came
  out with no fade at all and swallowed the picture. Thirty per cent of 250
  frames of a Chambois fight were swallowed that way, the darkest wholly
  black; none are now.
- **The polygon offset is carried over** (`PA_SU_POLY_OFFSET_*`, the front
  pair or the back pair by bits 11 and 12 of the mode control): the slope's
  scale in sixteenths, the offset in the depth's range.
- **The console multiplies the Direct3D 9 way:** zero times anything is zero,
  infinity and NaN included, and the translation does the same (`mulL`).
- **The mission title card** ("THE BLOODIEST BATTLE OF THE WAR / SAINT LO,
  FRANCE / July 19, 1944 / 1800 HRS") is a front-end panel
  (`SP_chapter_title_screen.PANEL`, four text lines in the `garamond` font)
  whose lines the level script sets through hud elements of types eleven to
  fourteen. The setter, `sub_824C0CD8`, is a four-case switch whose cases all
  branch to a shared tail after the last case; the analyser had bounded the
  function at that tail, the recompiler marked each branch `// ERROR` and
  returned instead, and the four lines never reached the panel. Extending the
  function in the config (with three others cut the same way) drew the card.
  `grep '// ERROR' CoD3RecompLib/ppc/*.cpp` after a recompile finds any more;
  `recompile.ps1` counts them.
- **Ten jump tables the analyser never saw.** A switch whose index the
  compiler knew to be in range has no bounds check before its table, and
  XenonAnalyse, which sized tables from that check, skipped them; the
  recompiler then made each `bctr` a call to whatever the table held, and
  the log said "call: through a pointer to X, which is not a function". One
  was the entity type dispatch (0x823E6610), whose skipped cases left the
  Island level's entity lists tangled and its game thread in an endless
  unlink. XenonAnalyse now sizes such a table from the table itself (it runs
  up to the first case's code, which its lowest entry names).
- **Reads past the end of a file are padded with zeros** to the length asked
  for, the way a whole-sector read off the disc comes back: the streamer reads
  archives in 409600 byte units, the Island level has a 196608 byte one, and
  the end of file it met there was reported as a dirty disc.
- **Any level from the main menu:** `COD3_MAP=<level>` puts the title's own
  `spmap` command on its command buffer once the menu is up; the levels were
  checked with it, seventy five seconds each.
- **The T-pose.** The level DLL had 922 such marks, nearly all in two large
  functions (0x890B91C0 and 0x89195508, compare chains over hashed names
  whose "not found" tails had been cut into separate pieces) and six smaller
  ones. The sergeant who sits on the crate at the start stood rigid with his
  arms out instead; with the functions whole (`config/saint_lo_functions.toml`,
  merged by `recompile_level.ps1`) he sits, turns and gestures. The marks
  that remain in the level are in fragments nothing calls.

### A PC renderer in place of the emulated GPU

The Direct3D 11 backend, the EDRAM model and the software rasteriser are
replaced by a renderer built in layers over the command processor: the
registers of a draw read once into a plain state, the state mapped to
cached pipeline objects by integer handle, the programs found by the hash
of their microcode with a disk cache that carries their metadata and a
background precompile of everything captured, textures uploaded once with
their mip levels, vertex buffers bound as they lie and turned round in the
shader, indices and constants streamed through rings with only the
constants a program reads going up, packed. The frame is presented from
the command thread at the swap with vertical sync, FXAA and a fractional
render scale from the window's height. The forest level went from 30 to
60 frames a second, the draw path from 8.4 to 1.4 µs a draw; the numbers,
the design and the audit of what was emulation are in
[docs/renderer.md](docs/renderer.md).

### The physical heap hands freed memory out again

The guest's allocators were bump pointers: address space handed out and
never reused. The physical heap is half a gigabyte and a level's textures
and buffers come and go with the level, so a second level ran it dry and
`MmAllocatePhysicalMemoryEx` began refusing, which is a level with pieces
missing (the title carries on without them). It is a free list in front of
the bump pointer now, first fit, the neighbours joined; the virtual heap
stays a bump pointer, since the title reserves ranges there and commits
pieces inside them at addresses of its choosing. What is freed is dropped
from the renderer's caches too, so the address holding something else
later is not served the old texture, and `MmQueryAddressProtect` says a
freed range is not mapped, as the console does: the film player walks its
memory with that question and walked off the end when everything answered
"writable".

### A PC render layer over the executor

The layer between the registers and Direct3D was then made explicit:
`render_commands.cpp` turns the registers of a draw into a `DrawCommand`
of handles, views, host pixels and ring offsets, and the executor in
`render_d3d11.cpp` runs commands without seeing a register. A profile of
the draw path (`COD3_RENDER_PROFILE=1`) showed the indices and the
constants as most of a draw's cost; the indices are turned round straight
into the ring now and the constants copied in runs, 1.35 to 1.13 µs a
draw. The settings gained texture quality and the field of view.

### The frame rate, unlocked in a level

The title's ceiling was never its own: it draws a frame, and its stream
then waits on a flag (the second word of the scratch block the driver
names in register 0x1DD) that its vertical blank handler clears a blank
later; the blanks come 60 a second. Its clock is the timebase (the frame
clock at 0x829C2518 read 40002 ms forty seconds into a run with the
rate unlocked), so it plays at the same speed at any frame rate, the way
the Quake 3 engine under it was built. While a level is played (the
title's `sv_running`, pointer at 0x829BAB18, with no film open, the
player at 0x82A2A24C) and the settings leave the frame rate unlocked,
the command processor lets that one wait through, and the next frame
starts as soon as the title has it: Saint-Lô at 2560x1440 drawn at 200
per cent, RTX 4070 Ti, 115 frames a second where it had 60. The menus
and the films keep the wait, so 60 and the films' own 30.

The first way of doing it made the blanks themselves come every
millisecond while a level was played. The frame rate came unlocked, but
the title counts the blanks as well as waiting for them, and its
soldiers' animation jumped back and forth every frame: consecutive
frames of the same idle scene were closer two apart than one apart,
three times the "back and forth" of a locked run, all of it on the
soldiers (and, in some runs, a stretch of ground drawn as nothing for a
frame). Letting the wait through with the blanks at sixty leaves the
frame rate unlocked and the back and forth at the locked run's level.
A blank at 120 a second would not have been 120 frames either: a frame
of Chambois took 10 to 12 ms, over a 120th, so every frame missed a
blank and waited for the next, 60 again. `COD3_UNLOCKFPS=0` keeps the
console's 60; `COD3_VBLANK=N` makes the blanks come N a second, for a
test.

One thing did depend on the frame rate, and it was this project's: the
mouse. The title turns by the right stick's deflection times its frame's
time and reads the pad twice a frame, and the deflection was the
distance the mouse went since the read before, which is a speed at one
frame rate only: at 100 frames a second the turn was 60 per cent of the
turn at 60. The distance is scaled to a sixtieth of a second now, the
rate the gain was set at (`COD3_PADRATE=1` counts the reads).

The settings that are the title's own console variables (the field of
view, the aim assist, and the new model detail, `r_lodscale`, 1 its own
and 0 the finest model at every distance) were written as lines added
to default.cfg, with `CoD3.cfg` beside the executable for more. None of
it ever ran: the title only checks that default.cfg is there
(sub_82532D28, at the file system's start) and runs ts_def.cfg and
xenon_a.cfg instead; `com_maxfps 60` there never took, and the field of
view only did when it was changed while playing. They go onto the
title's command buffer now, `set` so a variable the title has not
registered yet is made and then kept, the first time whatever differs
from the title's own and later what changed; the queue waits until the
title has set its buffer up (cmd_text at 0x829F1BB8), since
Cbuf_AddText drops what does not fit. `CoD3.cfg` is gone and
`COD3_EXEC` goes the same way.

### The soldiers' shadows: a depth only draw runs no pixel program

The soldiers' shadows came and went, on every level, walking or
standing, at any frame rate. The shadow maps and the depth pre-pass are
drawn in the EDRAM's depth only mode (RB_MODECONTROL 5), in which the
hardware writes depth and runs no pixel program, so the title loads none
for those draws and leaves the last one in place. The renderer ran it.
The soldiers' shadow casters inherited, in one cascade, the program of
the clear before them (nothing discarded, a whole shadow) and in another
the alpha tested casters' program, which discards by a texture and a
reference the soldiers' draw never set; which of the two came before
them changed from frame to frame with what each cascade held. A per
frame count of each kind of draw found it (the draws with the soldiers'
vertex program went 14 and 0 with one pixel program, then 10 and 4, frame
after frame). A depth only draw now binds no pixel program and no colour
target: 400 frames of the soldiers standing had 271 and 298 frames with a
region going on and off one frame, and have 1 now (a muzzle flash).
`COD3_DEPTHMODEPS=1` runs the programs as before.

### Buffers the title rewrites between the draws of one frame

The renderer looks at a vertex buffer's memory once a frame and uploads
it when it changed. The title rewrites some of its small buffers between
the draws of a frame: a tank's treads are strips built each frame into
16 KB buffers, one tread after another into the same memory once the
GPU has drawn the one before (the console's GPU reads the memory at the
draw), and a pair of 128 KB buffers in Chambois and Laison go the same
way (`COD3_DRAWLOG` and a look at every draw found them: Saint-Lô one
buffer, the Mace eight, Laison a dozen). Looked at once a frame, the
second tread of a buffer was drawn with the first one's vertices, and
its strip, a few vertices longer, ran on into whatever lay beyond them:
a dark triangle with the tread's texture across the ground for a frame,
or a tank's tread in the wrong place. Every later draw of a frame now
takes a sampled look (64 words) at a buffer of up to 256 KB, and one
found changing is looked at whole by every draw from then on; the cost
did not show in the frame rate (87 against 89 in the Mace).
`COD3_NOMIDFRAME=1` goes back to once a frame.

### Sound, and the films

The XMA contexts decode through FFmpeg's `xma1` decoder (the title's voices
are XMA1, stereo, 48 kHz; `xma.cpp`, loaded at run time from C:fmpeg or
beside the executable). What kept the speakers silent after that was four
bytes: `KeInitializeSemaphore` cleared twenty four and wrote the limit at
twenty, and a KSEMAPHORE is twenty bytes with the limit at sixteen. The
title keeps its mixer's "mixed" event right after its semaphore, so the
limit of six overwrote the event's type, a synchronisation event became a
notification event no wait ever cleared, and the audio callback stopped
waiting for the mixer thread after the first frame. With the layout right
the menu music, the dialogue and the effects play (`COD3_AUDIODUMP=path`
writes what goes out as a WAV).

The films play through the title's own WMV player, which always worked
once the GPU did; what stopped it last was the padded read, which told it
a 121647 byte sound track was 122880 bytes and had it read on past the
end. Reads are padded only for the archives now (`.cod`, `.wbk`, `.cfg`).
The legal notice, the logos, the attract loop and the mission briefings
play, with A (Space) to skip; `COD3_NOFILMS=1` leaves them out, which the
scripted runs do. A mission's briefing film plays while its level loads,
and on the console it cannot be skipped at all: the level's loader sets
the skip threshold (0x82A2A258) to the largest float, and the loop that
plays the rest after the load (sub_824EEBA0, which asks sub_824D0470 for
a button) never gets past it. Here the threshold goes to nought when that
loop starts, so once the level has loaded a press of Space (A, Start)
goes straight into the mission; `COD3_NOFILMSKIP=1` keeps the console's
way.

### A level's teardown: what is fixed and what is not

Loading a level from inside another (the next mission, a restart after
"MISSION FAILED", the console's `spmap`) hung one time in two. Three
causes were found, two fixed:

- **A destructor that ran twice** (the title's own fault). sub_823F0828
  destroys a list of objects by calling their destructor, sub_823F04B0,
  and then deleting them through the vtable, whose deleting destructor
  calls sub_823F04B0 again: six blocks freed twice, a dlmalloc bin turned
  into a cycle, and the next malloc (sub_820CCF00) never returned. The
  second run is left out now (`title_fixes.cpp`).
- **The fault handler sent a thread round for ever.** The title reads
  memory it has freed at a teardown, and memory nothing ever handed it;
  the handler let such a read through by opening the page, and claimed it
  had when the page was not committed at all, so the load faulted again,
  and again. It now leaves such a page to the demand commit (zeros).
  Three restarts of Chambois in one run went through after this, where
  the second had hung every time.
- **Fixed: the wait for the GPU in the indirect buffer pool**
  (sub_822F16A0), which came in the middle of a level too, about one run
  of Chambois in six within a hundred seconds, and in Saint-Lô: "The GPU
  is hung and can't be recovered". The kernel's spin locks did nothing,
  a leftover from before threads ran concurrently, and the driver's count
  of pending counted submissions (device+0x2A74, under the lock at
  +0x2A78) lost an update now and then: left at one, or at minus one,
  every later submission went to a list nothing would flush, and the next
  wait for the GPU never ended. The spin locks are real now, a compare and
  swap on the guest's lock word; twelve runs of Chambois since, none
  hung, at sixty frames a second.
- **Fixed: soldiers in their bind pose after a restart.** The title's
  memset (sub_8234EBA0, and the same routine in every level DLL) ends on
  `stb / bdzlr / stb / bdzlr / stb / blr` for the last one to three bytes,
  and the analyser ended the function at the first `bdzlr`: every memset
  whose length ran two or three bytes past a word left its last one or
  two bytes as they were. An animation tree of 25 animations is 74 bytes,
  so its last slot kept an old byte and named a record of the animation
  info pool (0x82A59DB0) it had never taken. At a restart the title gives
  back every record the trees hold (sub_824F7F90, from the `map_restart`
  and death-restart handlers), that record went onto the free list a
  second time, the list closed into a loop of a few records, and from
  then on every soldier got the same ones: one animation ending took
  another soldier's away, and a tree with nothing playing leaves its
  model in the bind pose. memset is the host's now (`native_crt.cpp`);
  three restarts of Chambois in a row came back with every soldier
  moving, and the pool stays whole (`COD3_TRACEANIMHEAP=1` checks it).
  `scripts/cut_tails.py` lists the functions that end on an instruction
  that does not leave: memset was the only real one. What was worked out
  of the animation trees, the asset banks and the zones on the way is in
  [docs/animation.md](docs/animation.md).
- **Fixed: the soldiers' uniforms were black in patches**, at the first
  load as much as after a restart. It was the renderer, not the level:
  the characters' pixel programs (27f44a32 for the body, 45 programs in
  all, the hands and weapons among them) build their tangent frame from
  screen derivatives with `getGradients`, whose result is d/dx and d/dy
  of the first component, then of the second. The translator put both
  d/dx first, so the frame lost its y half, the lighting (checked with
  `COD3_D3DSHOW=27f44a3227169b05:r2@127 COD3_D3DSHOWSIGN=x`) fell to
  nothing in blotches that looked like camouflage, and the albedo
  (a plain khaki) and the specular term were right all along. The
  order is Xenia's (x and z d/dx, y and w d/dy), and the program itself
  asks for `.x` and `.y` of the gradients of `r0.ww`, which only makes
  sense that way.
- **Fixed: soldiers wholly black, and a ghost of the weapon cut out of the
  smoke in front of it.** The title's driver records a frame's command
  buffer once and plays it through more than once with a different bin
  select (a Z pass, select 0x80000001, and a colour pass, 0x00000002):
  packets whose header has bit 0 set run only when the select and the
  bin mask set before them share a bit (Xenia's rule; a predicated swap
  never runs). A character's colour pass loads its vertex program twice
  in a row, the Z pass's position only program (2e954041, mask
  0x15555555) and the colour pass's (ec1ebcf4, mask 0x2AAAAAAA); running
  both left the position only one current, and the body drew with no
  interpolators: black, at times for seconds on end, the pack and helmet
  lit. The same held the Z pass's draws of the weapon and the particles'
  depth in the colour pass, and the half resolution smoke was cut out
  round a second weapon, several pixels wide, at any resolution. Both
  decoders now keep the bin mask and select and skip what they rule out
  (`COD3_NOPREDICATE=1` runs everything again). What stays round the
  weapon in thick smoke is a pixel or two: the title draws its particles
  at half resolution and puts them back by depth, as it does on the
  console.
- **Fixed: the game closed at the end of a mission.** The title goes
  from one mission to the next the console's way, by rebooting itself:
  once the mission is won its "reboot" command (sub_82514710) hands
  `XamLoaderSetLaunchData` a 664-byte block of campaign state (0x82A8E610)
  and asks `XamLoaderLaunchTitle` for `default.xex`; the new instance reads
  the block back at start (sub_823FCCA8, "retrieving stub data"), skips
  the front end and loads the next mission. The launch here ended the
  process. Now the block goes to `saves/launchdata.bin` and the executable
  starts itself again with `COD3_LAUNCHDATA` naming it (a scripted run's
  `COD3_MAP`, `COD3_CMD`, `COD3_PAD` and `COD3_WIN` stay behind), and the
  new process hands the block to `XamLoaderGetLaunchData`. Chambois won
  with `COD3_WIN=30:mace2` came back as The Mace, Mont Ormel, its title
  card and its level. The game's own way between missions is this, not an
  `spmap` from inside a level, which is why the case below is a debugging
  path rather than one a player meets.
- **Still open:** after the console's `spmap` mid-level the player has no
  weapon and no HUD. The table of weapons (0x82A2A2E0) is not the cause, but it is not
  rebuilt at a reload as an earlier note said either: the title fills it
  once a session (a flag at 0x82AB1BCC that nothing clears) and keeps
  the old entries, whose names happened to still be readable. At the
  teardown the title walks memory it has just freed, and this heap hands
  the lowest free address out first, which is that memory;
  `COD3_QUARANTINE=ms` keeps freed memory back that long and does remove
  those reads, but the reload still came back without the weapon once
  with it, so it stays off.

`COD3_HEAPCHECK=1` names every object sub_823F04B0 destroys and every
free of a block the heap does not hold; a read of freed memory, or of heap
memory never handed out, is printed with its reader and what the region
went through; `COD3_ONESLOT=1` runs every guest thread on one hardware
thread (the teardown hangs came with it too, so they are not a race).

Still open: 5.1 is folded to stereo. (The volume textures that read white
are not: every fetch the title makes "as a volume" names a flat texture,
and reads it; see docs/renderer.md.)

The draws the renderer's report counts as "no target", some thousands a
second in a level, are single points with the colour mask at nothing and
depth off: they write nothing on the console either, and leaving them
out loses nothing. No draw of a level needs one of the captured programs
that do not translate.

The sections that follow are the history of getting here, oldest first.

## What works

Verified on this machine, reproducible with
`scripts/recompile.ps1 -BuildLib -BuildHost`.

| Stage | Result |
| --- | --- |
| XEX decryption and decompression | clean, retail AES key, normal LZX compression |
| Function discovery | 19,462 functions |
| Jump table detection | 323 switch tables |
| Instruction translation | every instruction translated, no diagnostics |
| Generated C++ | 78 translation units, 94 MB |
| Compilation (clang-cl, Release) | 48 MB static library, no errors, no warnings |
| `CoD3.exe` | builds, installs the game, boots the title into its main loop |

The recompiler run is completely silent: no unrecognized instruction, no
out-of-range switch case, no condition-register warning. That is the bar to
hold when anything changes.

### Booting

`CoD3.exe` reserves the full 4 GB guest address space, maps the executable's
15 sections, fills a 19,641 entry indirect call table, binds all 179 kernel
imports, and enters the recompiled entry point. All 179 imports have
implementations, so nothing stops the boot.

The title runs indefinitely. It creates six threads, allocates 387 MB, brings up
its audio and video subsystems, initialises the GPU, and its graphics interrupt
handler runs at 60 Hz. Then it stops making progress, without stopping.

### The title renders, and the frame loop closes

It boots, loads its configuration and its main asset archive, runs a real frame
loop, and presents finished frames. Getting from a dead stop to that took six
runtime bugs, each found by measurement rather than guesswork.

**Mutants were not recursive.** A mutant on this console can be acquired again by
the thread that already holds it. Treating one as a plain count deadlocked the
first thread that took a lock twice.

**The read pointer was published past the write pointer.** The title spins
comparing unsigned distances, and a read pointer beyond the write pointer makes
one of them wrap to nearly four billion. A GPU that has caught up publishes
exactly the write pointer, no more.

**MmQueryAddressProtect always answered "writable".** The title walks the address
space 64 KB at a time and stops at the first inaccessible block. An answer that
is always yes sends it round that loop forever. Asking the host what is actually
committed is what made the title start loading files.

**The file information class was wrong.** FileNetworkOpenInformation is class 34,
not 32, and it is how the title asks how large an archive is.

**The GPU progress pointer was never advanced.** A render thread polls the word
at offset 4 of the write back block and waits for its low two bits to match a
tag it holds, while holding the lock everything else needs. Cycling that tag
released it.

**Indirect buffer addresses are physical.** This was the one that mattered for
drawing. The ring holds `INDIRECT_BUFFER` packets pointing at the real command
stream, and those pointers are physical addresses that have to be resolved
through the 0xA0000000 window, exactly like the ring itself. Following them
literally walked zeroed memory, which parses as an endless run of one-register
writes: hundreds of thousands of packets a second and not one draw.

### The command stream, decoded

Every packet the title sends is now named and read rather than counted. Over
about twenty seconds:

```
geometry: point list x16224  triangle strip x338  rectangle list x676
shaders:  1352 vertex, 1014 pixel, 41574 dwords of microcode
commands: DRAW_INDX_2 x16900   IM_LOAD_IMMEDIATE x2366   WAIT_REG_MEM x4060
          INVALIDATE_STATE x2031  EVENT_WRITE_SHD x1359  INDIRECT_BUFFER x1358
          INTERRUPT x1013  COND_WRITE x1024  IM_LOAD x676  DRAW_INDX x338
          SET_BIN_MASK_LO x1690  SET_BIN_SELECT_LO x338  REG_RMW x3
```

The decode was checked against raw bytes rather than trusted. A DRAW_INDX_2
packet reads `C0003600 00010081`: type 3, opcode 0x36, one payload word, which
gives a point list of one index with automatic indexing. The word after it is
the next packet's header, which confirms the length arithmetic too.

### The register file, read rather than guessed

The whole register file was dumped at the moment of a draw and matched against
things already known. Three independent agreements fix the numbering:

| Register | Value | Meaning |
| --- | --- | --- |
| 0x210F to 0x2112 | 520, 520, -312, 312 | a viewport for a 1040 by 624 target |
| 0x2082 | 0x02700410 | window scissor, 1040 by 624 |
| 0x2319 | 0x19CC0000 | the address VdSwap names as the front buffer |

A wrong map does not produce three agreements. From there the rest of the render
backend block reads straight off: surface pitch 1040 with two samples per pixel,
colour at EDRAM tile 0, depth at tile 1014, destination 1056 by 624 in the
8:8:8:8 format.

### EDRAM and the resolve

The console's GPU does not render into memory. It renders into ten megabytes of
embedded DRAM on the die, and a separate operation, the resolve, copies a region
of that into a normal texture. The title spells a resolve as a rectangle list
draw with the render backend in copy mode.

That path is now implemented end to end. Frames resolve into 0x19CC0000 and
0x19F60000 alternately, 1040 by 624, about fourteen times a second, and the
window presents them.

The destination is tiled, not row by row, and the tiling function is checked
before it is used: it has to be a permutation of the texels of the surface, and
the runtime says so on start up.

```
edram: the tiled address function maps 1056x624 one to one, using it
```

An earlier version of that function failed the same check with 40,128
collisions, which is exactly the sort of error that produces a scrambled picture
and no explanation.

### What the frame is made of

The shader microcode the title uploads is captured to disk as it goes past, and
`tools/XenosDis` reads it. At this point in start up there are five distinct
programs, and together they describe the whole frame:

```
vs_10e10ea9  vfetch r1 (position, 3 floats), vfetch r0 (colour, 4 floats)
             oPos = r1, oInterp0 = r0                     stride 7 dwords
ps_5d27834e  oC0 = r0                       the interpolated colour

vs_e5a81a7a  vfetch r2 (position), vfetch r1 (colour), vfetch r0 (coordinates)
             oPos = r2, oInterp0.xy = r0, oInterp1 = r1   stride 6 dwords
ps_37456d13  tfetch r0 from texture 0
             oC0 = r0 * r1                  texture times colour
```

So each frame is a full screen rectangle that clears, and one textured quad.
The rectangle's vertices are already in screen coordinates, -0.5 to 1039.5, and
its per vertex colour is zero: **the title clears to black**, which is why an
empty screen was the correct picture and not a failure.

Reading the microcode took three corrections, each forced by the programs
themselves rather than chosen:

- **Source operands** say which file they read from in the last word of the
  instruction, not the middle one. Reading the middle one made a vertex program
  export a constant as its position, which would collapse every triangle it
  draws to a point.
- **Whether an instruction is arithmetic or a memory fetch** is carried by the
  control flow instruction that runs it, two bits per instruction. Guessing it
  from the shape of the instruction word turned a texture read into an add.
- **The stride and the offset of a vertex fetch** share the last word, eight
  bits of stride then the offset. Reading the whole word as an offset put a
  three float position 768 dwords into its buffer, which no layout does.

### A frame on screen

Both draws now run, and the loading screen appears.

The renderer is an interpreter rather than a translator: it runs the title's
microcode directly, one instruction at a time. Each frame is a handful of
instructions over a few hundred thousand pixels, which needs no compiler and no
graphics API and keeps every step inspectable.

```
raster: sampling texture slot 0, 64x64, pitch 64, tiled, at 0x00924000
raster: 270 draws rasterised, 540 triangles, 88814205 pixels written
resolves: 135 into 0x19CC0000, 1040x624, 425169 texels not black
```

Two things had to be right for the geometry to land where it belongs.

**The viewport transform.** PA_CL_VTE_CNTL at 0x2206 says which parts of it
apply. Its two format bits say whether coordinates have already been divided by
w, and six enable bits say whether each scale and offset applies. The clear has
the format bits set and every enable clear, so its coordinates pass straight
through. The textured geometry has neither, and needs the whole transform: divide
by w, then scale by 520 and -312 and offset by 520 and 312, which is the
viewport for a 1040 by 624 target.

**The texture fetch constant.** Six dwords name everything about a surface, and
the numbering was confirmed against one whose answer was already known. The
constant VdSwap hands over for the front buffer reads back as pitch 1056, 1040
by 624, format 6, tiled, based at 0xB9CC0000, every one of which matches what
the resolve writes. The loading screen's own texture reads back as 64 by 64,
tiled, and lands on screen as the mark it is.

What is still missing from the renderer: depth testing and blending are ignored,
sampling is nearest with no filtering, only the four float formats and packed
bytes are decoded for vertices, only the plain 8:8:8:8 format for textures, and
indices in the packet itself are not handled. Each of those is counted and named
at run time rather than approximated, so what is missing stays visible.

### Past the first frame

With a picture on screen the question changed from "does anything draw" to
"why does it stay on the loading screen". Six more things were missing, each
found by measurement rather than reading code.

**The thread pointer was zero.** Guest code reads its own thread state through
r13 rather than asking the kernel, and this runtime never set it up. Two idioms
in the title's own code fix the layout beyond doubt: reading r13+0x100 then
+0x14C is how it gets its thread id, and writing r13+0x100 then +0x160 is how it
records its last error. With r13 zero, every one of those reads went to guest
address zero. Every thread believed it was thread zero and every timestamp read
this way was the same instant forever.

The first attempt at a fix took those blocks from the stack allocator, which put
the first one at the top of the main thread's own stack. The stack grew down
over it within microseconds.

**The audio callback was registered and never called.** Registering a render
driver client hands the console a callback that the hardware calls every 5333
microseconds so the title can produce the next frame of samples. Nothing was
calling it.

**Fences were counted, not written.** An EVENT_WRITE_SHD packet tells the GPU to
write a value into memory once it reaches that point in the stream, and the
graphics driver spins on that memory to know its work is done. Carrying those
out is what let the driver submit real work: the packet rate went from about
fifteen hundred a second to over forty thousand.

**The command processor and the display shared a thread.** That was fine while
nothing was drawn. Once draws were carried out, one busy buffer took longer than
a frame and the vertical blank stopped happening, which the title reads as a
display that has stopped. Giving them separate threads is also what the hardware
does. Cutting a buffer short instead does not work: the fence writes live at the
end of it, and the title went quiet after two hundred draws when that was tried.

**Asynchronous reads never completed.** A read that names a completion routine is
queued against the calling thread, returns STATUS_PENDING, and runs the routine
when that thread next waits. This runtime ignored the routine, so the title read
the first four hundred kilobyte block of its eight megabyte archive and waited
for a completion that was never coming. Implementing it took the load from 0.4
to 1.5 MB.

**Threads had no object in guest memory.** The Ke family of thread calls take a
pointer to the thread object, not a handle, and the title gets that pointer from
ObReferenceObjectByHandle. With no object to hand back, that call failed, the
title passed on whatever was in the uninitialised variable, and KeResumeThread
was asked to resume a stack address:

```
thread: resume by pointer 0x6FFFF2B0, NOT FOUND
```

Two threads were created suspended and never started. One of them was the audio
thread. Giving every kernel object and every thread a real body in guest memory
started them, and the title now runs eleven threads instead of nine.

The time base was wrong as well, though nothing visibly depended on it. Guest
code reads it with mftb and converts the differences to seconds using the
frequency the kernel reports, 49,875,000. XenonRecomp translates mftb as the
host's cycle counter, about seventy times faster, so every duration the title
measured came out seventy times too long.

### Getting the load moving

Four more things stood between the loading screen and the game's own content,
and every one of them was found the same way: measure, name what is waiting,
read the guest code at that address.

**Hardware thread numbers were all zero.** Guest code reads which of the
console's six hardware threads it is on out of the thread block, and uses it as
an index. This title starts one worker pinned with a mask of 0x10, and a barrier
elsewhere waits for exactly the byte that hardware thread four writes. With
every thread numbered zero they all wrote the same byte, the barrier could never
fill, and the worker that had to acknowledge the loading request span there
forever. The number is in the top byte of the creation flags, which this runtime
was throwing away.

**A pulse woke nobody.** NtPulseEvent releases every thread waiting at that
moment and then leaves the event clear. A signal state cannot express that:
setting it to one releases a single waiter, and setting it and clearing it
releases none. This runtime did the second, so four worker threads sat on their
job queue with no timeout while the two pulses meant to start them went
nowhere. A generation counter says it properly.

That one changed everything. The title went from reading 1.5 MB and stopping to
reading its way through the whole archive and opening what comes next:

```
D:\sp\global.cod      D:\sp\Global.wbk     D:\sp\bcen_can.wbk
D:\sp\bcen_pol.wbk    D:\sp\bcen_bri.wbk   D:\sp\bcen_usa.wbk
D:\sp\anim.cod        d:\config\ts_def.cfg  D:\sp\frontEnd.cod
```

`frontEnd.cod` is the menu package. Eighteen files, where there had been five.

**The thread pointer region ran out.** A megabyte was enough while the title was
stuck and ran nine threads. Once it started loading properly it ran out, handed
back a null thread pointer, and the guest read through it until an address
landed outside the address space altogether. Eight megabytes, and blocks are
given back when a thread ends.

**Demand commit was refused too easily.** Sixty four kilobytes at a time is the
right granularity, but a block that straddles something already mapped
differently is refused whole. Falling back to the single page turned a run that
ended somewhere different every time into one that ends in the same place every
time.

### Critical sections were not shared with the guest

A critical section on this console is its lock count. Free is minus one, and
taking it is an atomic increment: the thread that sees zero has it, and anyone
else has recorded that there is contention, which is how the holder knows to
wake somebody on the way out. Titles inline that fast path and only call the
kernel when it fails.

This runtime tracked ownership in a different field and never touched the lock
count at all, so the two sides were not sharing a lock. Both could be inside the
same critical section at once. It is now implemented as the console runs it,
with an atomic increment on guest memory, and the owner recorded the way the
guest records it: a pointer to the thread object, not a host thread id. Getting
that second part wrong deadlocks the title on its own lock, which is how it was
found.

### Six hardware threads, and taking turns on them

The console runs six hardware threads and every guest thread sits on one of
them. Two threads on the same one never run at the same instant. A title can
rely on that, and this one does: it hands memory out of a frame arena through a
single unlocked cursor from more than one thread, which is safe only if those
threads never overlap.

This runtime ran every guest thread on its own host core, so they did overlap,
and the frontend load died every time on a block that had been handed out again
underneath it. Three things were missing:

**A thread inherits its creator's hardware thread.** With no processor mask,
the console starts a thread where its creator is running. Every thread this
title creates without a mask therefore shares one hardware thread with the main
thread, and cannot race it.

**KeSetAffinityThread did nothing.** The title spreads its threads across the
hardware threads afterwards, and ignoring that left them all queueing behind
each other. It now moves the thread, and the thread picks the move up the next
time it takes or hands over its slot.

**Threads take turns.** Each hardware thread is a lock, held while a thread runs
guest code. Everything that can block releases it first, and a thread that spins
rather than blocks hands it over at any kernel call, so a thread that never
blocks cannot keep it. Nothing waits for a slot forever: after fifty
milliseconds a thread runs anyway, and the serialisation is merely imperfect
instead of the title stopping dead.

**ExTerminateThread let go of nothing.** It ended the host thread where it
stood, without signalling that the thread had finished, without giving back its
hardware thread, and without returning its thread pointer block. That cost twice
over: a thread waiting on this one's handle waited for a signal that never came,
and everything else assigned to that hardware thread fell back to running
unserialised after a timeout. Fixing it took those timeouts from over nineteen
hundred a run to under thirty, and waiting on a thread handle now works at all,
which it did not before: thread handles live in their own table and the wait
never found them.

### The menu draws

The deterministic crash is gone, and the frontend now loads and renders.

| | Before | Now |
| --- | --- | --- |
| Data read | 9.4 MB, then a crash | 23.1 MB and running |
| Non black pixels resolved | about 400 thousand | about 40 million |
| Hardware thread timeouts | 1918 | 16 to 27 |

### What it takes to draw a menu

Three more pieces went in, and each one was visible the moment it worked.

**Quad lists.** A quad is four corners in order and two triangles sharing the
diagonal. They were being counted and skipped, and they are most of what a
menu is made of: the triangle count went from five hundred a frame to nearly
three thousand.

**Compressed textures.** Everything the interface is drawn with is stored four
texels at a time: two colours at either end of a line and two bits a texel
saying where along it to sit, with the alpha either four bits a texel or the
same trick again with eight levels. The blocks are laid out with the same tiling
as everything else, counted in blocks rather than texels, and every pair of
bytes in them is stored the other way round from the way the rest of the world
writes them. That last part is easy to get wrong and hard to notice: reading the
index bits the wrong way round still produces plausible colours from the right
two endpoints, so it looks almost right and is not.

**Blending.** A menu is text and panels with transparent edges drawn over what
is already there. Writing only the source colour draws every letter inside a
black box. The equation is one register: a factor for the source, a factor for
what is behind it, and how to combine them, which here is the ordinary source
alpha and one minus source alpha.

Turning blending on made the whole background disappear, which turned out to be
a fourth thing rather than a mistake in the third. A packed vertex colour has
its first component in the least significant byte, and this runtime was reading
it from the most significant one. That put the alpha where the blue belongs, so
a background drawn with an alpha of nothing blended away to nothing, and the
blue that had been on screen before was the alpha channel being mistaken for a
colour. With the order right, the loading screen draws as it should: the text
and the mark over a dark background, with their edges soft rather than boxed.

What is still counted and skipped: single index point lists, which cover one
pixel each.

### Four things the runtime was doing to itself

None of these were the title's fault, and each one was hiding the next.

**The disc root would not open.** The title opens the device on its own, without
a file name, to ask about the disc rather than about a file on it. The path
resolver matched device prefixes only when a separator followed, and then
refused an empty relative path outright, so that open came back as no such
device. Letting it resolve to the game directory took the load from one archive
to four: the sound bank, the animations and the menu package all began to be
read where before only the first was.

**Failure tore the address space down.** A thread that could not continue called
the runtime's shutdown, which released the whole four gigabyte reservation while
ten other threads were still running inside it. Every one of them then faulted
on memory that was no longer there, each fault reported as one that could not be
committed, and the message that actually mattered scrolled away. A thread that
fails now reports and stops.

**The allocators disagreed about granularity.** The runtime's own small
allocators committed a page at a time inside a region the fault handler commits
sixty four kilobytes at a time, and the mismatch left parts of a block in a
state the handler could not commit. Both now commit the same way, and those
reports are gone entirely.

**The commit ceiling was the console's memory, not this runtime's.** Half a
gigabyte is what the console has, and it looked like the right limit until the
title started loading properly. Physical memory is written through four windows
here and each costs its own pages, so the real figure is several times what the
title thinks it is using.

An exception that this runtime does not recognise used to end a run with no
message at all: no code, no address, no stack. Every one is now named, with the
guest call chain that reached it.

### The command processor waits when it is told to

A WAIT_REG_MEM packet holds the command processor until a value satisfies a
condition. It is how a title keeps one piece of work behind another, and this
runtime was counting eight hundred of them a run and skipping every one, which
lets everything after them run early: a fence is written before the work it
stands for has happened, and the driver is told its work is done when it is not.

They are honoured now, and the bound on how long matters more than it looks.
Everything earlier in the stream has already happened by the time one of these
runs, so a wait on the GPU's own progress is satisfied at once. The ones that
are not are waiting on the guest, and holding the whole stream up for those
starves the title of frames: two milliseconds each took a run from eighteen
files back down to five. At two hundred microseconds nothing is lost and the
ordering is kept where it can be, which is most of the time.

```
waits: 853, of which 279 gave up waiting
```

### Dividing by zero is not fatal on this processor

A PowerPC integer divide by zero does not trap. The result is undefined and the
program carries on, so dividing by a count that happens to be zero is ordinary
code on this console and titles are written accordingly. XenonRecomp translates
those as a bare divide, which on the host raises a fatal exception instead.

Four hundred and fifty two divides in this title needed a guard, and the
difference it made is out of all proportion to the size of the change:

| | Before | After |
| --- | --- | --- |
| Files opened | 18 | 27 |
| Data read | 23.1 MB | 26.7 MB |
| How far it gets | the loading screen | opening its intro films |

`legal-us.wmv`, `ATVI.wmv`, `Treyarch.wmv` and `Attract.wmv`, each with its
audio track: the startup sequence that runs before the menu.

The same guard covers the one signed case that overflows rather than divides by
zero, which traps on the host for the same reason and is equally undefined here.
It was found because a run that was allowed to go on for four minutes rather
than one printed something new, and because an exception this runtime did not
recognise now names itself instead of ending the process in silence.

### The menu, sound and a player

The title reaches its own start screen. `CALL OF DUTY 3` over a photograph of a
street in Normandy, `PRESS START` under it, drawn from the game's own textures
at the game's own 1040 by 624 and put in the window at whatever size it is.

Five things were needed, and each was a real defect rather than a missing
feature:

**Two guest threads were running at once on one hardware thread.** The slot each
guest thread holds was only ever waited for with a fifty millisecond bound, and
a thread that gave up ran anyway. That happened fifty two times a run, and every
one of them was the exact overlap this scheme exists to prevent. At two seconds
it happens three times, and the failure that follows stopped moving around
between runs: the same address, the same call chain, every time.

**A mutant held by a thread that had ended was never given back.** The console
hands the lock to the next waiter and tells it the owner died. This did not, so
one run in three went quiet with four threads waiting on locks owned by a thread
that no longer existed. The report had been saying so for a while:

```
mutant ownership:
  0x00000110 held by thread 27544, depth 2
what each thread is blocked on right now:
  thread 31716 on 0x00000104 from 0x823494F8, no timeout
```

Thread 27544 is not in the second list because it was gone.

**Every texture was point sampled.** The fetch constant asks for linear, and the
title has never asked for anything else. At 1040 by 624 stretched to a window,
point sampling is what made the result look coarser than the console's.

**The picture was described with one size and filled with another.** The
presenter copied the front buffer at the size it read from the atomics, then
read those atomics again to describe the bitmap. The guest swaps buffers from
another thread, so when the size changed in between, the image slid up the
window and the rest wrapped around to the top. It is copied and described from
one reading now, drawn into a bitmap of its own, and put up in a single blit, so
a frame can never be caught half updated. The console's 16 by 9 is kept
whatever shape the window is.

**Nothing was ever played.** The title mixes its own audio and hands the console
a finished frame every 5.33 milliseconds: 256 samples for each of six channels,
big endian floats, one channel after another. Those frames were counted and
dropped. They are now folded to stereo and played, thirteen thousand of them in
a seventy five second run with nine dropped at startup while the device warmed
up. What is still silent is anything the title fed through the console's XMA
hardware, because that decoder is not here.

### A player can drive it

Enter or Space is Start, so the first screen can be got past without knowing it
wanted a pad. Clicking in the window pins the pointer and turns it into the
right stick; Escape gives it back. WASD is the left stick, the mouse buttons are
the triggers.

Two things had to be fixed for any of it to reach the game. A pad that was
plugged in and untouched silenced the keyboard completely, because the two were
an either-or rather than both. And the sticks and triggers were written to the
guest as zero whatever the player did, so the keyboard could press buttons and
nothing could move or aim.

### The intro films are opened and never read

`legal-us.wmv`, `ATVI.wmv`, `Treyarch.wmv` and `Attract.wmv`, each with its
`.wma` audio track, are all opened. Not one byte is read from any of them.

This was reported here once as a conclusion drawn from the log, and the log did
not support it: reads were printed only for the first eight of the whole run,
which all came from the first archive, so every later file looked unread. Bytes
are now counted per file, and the answer is the same one measured rather than
guessed:

```
file: read from D:\sp\frontEnd.cod 10.8 MB  D:\sp\global.cod 7.9 MB
      D:\sp\anim.cod 2.2 MB  D:\sp\Global.wbk 2.1 MB
```

No decoder needs writing. The title imports no media playback function at all,
so its WMV and WMA decoding is in its own code, already recompiled. It opens the
films, builds a player, and the player is one of the objects below.

### The films are read now, and a directory was the reason they were not

The title opens the disc root, `D:\`, the way it opens a file. Windows refuses
that unless the call asks for a directory, so the open failed, and the title
answered a disc it could not open by throwing. XenonRecomp translates no
exception handling at all, so the throw ran off through a jump buffer into
whatever happened to be there. The first sign of it was an indirect call to
0x825C6F68, past the end of the code, made with a structure full of stack
pointers: not an object with a missing vtable, but an unwind record.

One flag on one call. What it changed:

| | Before | After |
| --- | --- | --- |
| Files opened | 27 | 34 |
| Read from the films | nothing | `Attract.wma` 2.4 MB, `Treyarch.wma` 0.6 MB, `ATVI.wmv`, `Attract.wmv`, `Treyarch.wmv` |

So the decoder was there all along, exactly as the imports said: the title
carries its own, and it had never been given a disc it was willing to believe
in. Directory listing is implemented too, though this title never asks for one.

### The sound that comes out is silence, and the meter says so

Every frame the title mixes is now played, and every sample in every one of them
is zero:

```
audio: 13297 frames played, 9 dropped, loudest sample 0.0% of full scale
```

That is not the output path failing. It is the answer to where this title's
sound comes from: all of it goes through the console's XMA decoder, which is
hardware this runtime does not have. The contexts are created, enabled and
disabled — 258 of each — and every one of them is told its output is never
ready, so the mixer has nothing to mix. The path from the mixer to the speakers
works and is measured; what is missing is upstream of it.

### A frame rate, and a mouse that can work a menu

The rate is counted where the front buffer changes, not where the window
redraws, so it reports what the game produced rather than how often the window
was painted. It is drawn onto the finished picture before it goes to the screen,
so it cannot flicker separately from the frame under it.

The menus read the pad's cross as presses, not as a position, and there is no
way to ask the title what sits under the pointer — so pointing at an entry
cannot select it. What the mouse can do is what the pad does: moving it up or
down steps the highlight, one step per stretch of travel rather than one per
pixel, and the left button takes the entry. Taking the pointer for looking
around moved to F1 and the middle button, because the left one is needed for
what it means on screen.

### How far the film player gets, measured

The three films that run before the menu, and the sound that goes with them:

| File | On disc | Read |
| --- | --- | --- |
| `legal-us.wmv` | 0.31 MB | a preload |
| `atvi.wmv` | 3.37 MB | 0.1 MB |
| `treyarch.wmv` | 5.92 MB | 0.1 MB |
| `attract.wmv` | 31.1 MB | 0.1 MB |
| `attract.wma` | 1.18 MB | 1.18 MB, all of it |
| `treyarch.wma` | 0.30 MB | 0.30 MB, all of it |
| `atvi.wma` | 0.17 MB | 0.17 MB, all of it |

The player starts. It reads each sound track from end to end, which is what a
player does with a small track before it begins, and then reads one preload of
video and stops. The picture is where it stops, not the sound.

Two things were wrong with the decoder underneath it, and fixing them changed
the numbers above from nothing at all:

**The description of every context was thrown away.** `XMAInitializeContext`
takes a context and a structure describing it, and this cleared the context and
ignored the structure. Every context read back as sixty four zero bytes, because
the title had said where its buffers were and been ignored. What it actually
says, read back now:

```
xma: context 0x6F8EFFC0 described from 0x6F8FF910:
  B2F8D800 00000001 B2F8E000 00000001 00000020 B2F8F000 00000008 ...
```

Two input buffers, an output buffer of eight blocks at 0xB2F8F000, 48 kHz.

**The decoder never moved.** It answered that decoded audio was never ready, and
a player told that waits. It now goes through the motions: blocks become ready
at the rate the hardware would produce them, 128 samples at 48 kHz, and the
output buffer is left as the title allocated it, which is zeroed. Silence, but
silence that arrives on time. With it the sound tracks are read once rather than
read repeatedly while the player spins.

What no part of this does is decode. XMA is the console's own codec and there is
no implementation of it here, so every sample that reaches the speakers is a
zero and the meter says so: `loudest sample 0.0% of full scale`.

### The title says what is wrong with the films, in its own words

Buried in the debug output, printed by the game's own D3D driver:

```
The GPU is hung!  D3D version is 0.3424 , kernel is 0, frame is -1.
  o The GPU appears to have hung while executing the command buffer
    represented by the D3DCommandBuffer structure at address 0x0.
The GPU state doesn't match any recognized hangs.
Breaking into the debugger.  The GPU is hung and can't be recovered
without doing a cold boot.
```

That is why the picture never starts. The film player queues its work, the
driver watches the command processor, decides it has stopped, and gives up. It
is not the decoder and it is not the file: the title stops its own playback.

**The register window was sixty four megabytes wide and should have been a
quarter of one.** The aperture sits at 0x7FC80000 and the registers are a
sixteen bit index four bytes apart, so it ends at 0x7FCC0000. This runtime
claimed everything from 0x7C000000 for sixty four megabytes, on the reasoning
that a wider net would catch a driver using a nearby address. What it caught was
ordinary memory: reads at 0x7FFFA2C3, and writes that put a floating point 1.0
into the ring buffer's write pointer, which then read back as 1065353216. A
command processor whose write pointer is a float is one the driver is right to
call hung.

**A third of the command processor's waits are abandoned**, and the report now
says what for:

```
gpu: gave up waiting on memory 0x1A780002: it holds 0x00000000,
     the wait wants 0x00000004 under mask 0xFFFFFFFF, test 3
```

The low two bits of a PM4 memory address are an endian field rather than part of
the address, so this is a wait on 0x1A780000 for the value 4, and nothing ever
writes there. The fences this runtime does write go to 0x18160000 and
0x1A770000, which are the two write-back areas the title registered:

```
video: read pointer write back at 0x1816003C, block size 6
video: read pointer write back at 0x1A77003C, block size 6
```

0x1A780000 is neither of them, and no packet this command processor handles
targets it. Finding what should be writing there is where this goes next.

### The command stream asks for interrupts, and now gets them

A PM4 stream can ask for the CPU to be interrupted, and this runtime counted
those packets and threw them away: two hundred and thirty a run. The title's
driver learns that queued work has finished in the handler they raise, and a
driver never told anything finished decides the GPU has stopped.

They are raised now, where they appear in the stream, after everything queued
ahead of them has been carried out. Source one is the command processor; the
vertical blank on the other thread is source zero, which was the only one
anything had ever been raised with. Raising them needed the command thread to
be able to run guest code at all, so it has a context and a stack of its own
now, like the audio pump.

### The fences go to one block and the waits look in another

Fences are written in pairs, a counter and an address:

```
gpu: fence write of 0xB72FA554 to physical 0x1A770004
gpu: fence write of 0x00000007 to physical 0x1A770000
gpu: fence write of 0xB72FA584 to physical 0x1A770004
gpu: fence write of 0x00000009 to physical 0x1A770000
```

The waits that are abandoned look for exactly that pair, sixty four kilobytes
further on:

```
gpu: gave up waiting on memory 0x1A780002: it holds 0x00000000, wants 0x00000004
gpu: gave up waiting on memory 0x1A780006: it holds 0x00000000, wants 0x00000001
```

Same shape, same two offsets, different block. 0x1A770000 is the area the title
registered:

```
video: read pointer write back at 0x1A77003C, block size 6
```

0x1A780000 is not registered anywhere, and no packet this command processor
carries out targets it. Either a second write-back area exists that this runtime
never hears about, or something that should be writing there is a packet still
being dropped. That is the next thing to find, and it is the last thing between
here and the films.

### Where it stops now

The failure that killed every run in the frontend load is gone. What ends it now
is a different one, later, and the report says so itself:

```
guest call chain, innermost first: sub_824EF788 <- sub_824CF060 <- sub_82511168
  <- sub_82511B28 <- sub_82536DD0 <- sub_82400098 <- sub_82344D00
This is the function table being read for guest address 0x00000000.
The guest called through a null function pointer.
```

It is the same shape as the last one and in a different subsystem: a virtual
call on an object that has not been set up.

With the four faults above out of the way it no longer ends there. Three runs in
a row now load eighteen files and 23.1 MB, draw their loading screen, and are
still running when the harness stops them: no fault, no exception, no silent
death. What they do not do is finish the menu package, which is 56 MB and gets
about a quarter of the way.

What that looks like from inside is a title with nothing to do. Its worker
threads sit on their job queue and no job arrives. Its main thread is inside the
frontend load, waiting for the graphics driver, which is waiting for the GPU to
reach a point in the command buffer.

The GPU side of that is working. The ring is consumed as fast as it is filled,
fences are written, and the progress word the driver watches changes:

```
ring: write pointer 853, read pointer 835, size 8192 dwords
watched: 0xBA770000 -> 0000011D B757C356 00000000 00000000
```

The progress word is an address with a two bit tag below it, and the tag is
what moves: B757C354, B757C355, B757C356. The address does not, because the
title keeps submitting the same short command buffer, which is what a loading
screen is. The point the driver is waiting to see is in work that was never
sent.

So the two ends agree: nothing is stuck in this runtime, the title has simply
stopped scheduling the rest of the load. Why it stops is the open question, and
it is a question about the title's own job graph rather than about anything the
runtime does or does not provide.

The physical memory windows are still separate storage rather than one memory
seen four times. Making them genuinely the same was tried: releasing the single
reservation to make room for four mapped views left the address space in a state
the loader could not use, and the title stopped much earlier. It is a real gap,
and not the one that matters yet, because the guest reads back through the same
window it wrote to.

### What the runtime says while it runs

The tools built to find all of the above are worth more than any one of the
fixes, because the next failure will be found the same way:

- Guest threads are sampled as a histogram of innermost frames, so a thread
  making progress can be told from one going round.
- The last two dozen kernel calls per thread are kept, with the object each one
  was about, so a wait loop names what it is waiting for.
- Every object waited on but never signalled is reported by address.
- A fault names the guest function, walks the guest call chain, works out from
  the faulting address which guest address the function table was read for, and
  prints the state around it.
- A word of guest memory can be watched in hardware, so the code that changes a
  value is named rather than inferred. It self tests before it is trusted.

### Verification

The instructions this project added to the recompiler are now checked against
Xenia's PowerPC instruction tests. Each test is a short assembly routine with
its input registers and expected output registers written alongside it; the
routine is assembled, recompiled to C++ exactly as the game's code is, compiled
and run, and every register that comes out different is a miscompile.

```powershell
.\scripts\verify.ps1
```

**All 16 covered instructions pass.** The harness was confirmed to have teeth by
deliberately masking `vslh`'s shift count with `0x7` instead of `0xF`: the tests
reported the exact wrong values, and passed again once it was put back.

Getting there needed one tool. Xenia ships the test sources but not the
assembled objects, and the LLVM install here has no PowerPC target, so
`tools/PpcAsm` assembles them using the opcode tables XenonRecomp already
carries for its disassembler. The same table that says how to decode an
instruction says how to encode one. Every instruction it emits is disassembled
again and compared, so a wrong encoding cannot quietly become a wrong test
result. It handles 126 of Xenia's 167 test files; the other 41 use extended
mnemonics such as `sldi` and assembler macros it does not implement.

Two things this does not cover, and they should be said plainly. Eighteen of the
34 added instructions have no test in Xenia's suite at all: `vcmpgtsh`,
`vcmpgtsw`, `vcmpequh`, `vcmpgtuh`, `vaddsws`, `vaddsbs`, `vsububm`, `vsrab`,
`vrfip`, `vcfpuxws128`, `vsel128`, `mulhd`, `mulhdu`, `cror`, `crorc`, `bso`,
`bns` and `bsolr`. And `mulhd` and `mulhdu` do have tests, but they are among the
41 files the assembler cannot yet build.

Running the whole suite rather than the covered subset shows something worth
knowing about the recompiler itself: 61 distinct opcodes have no implementation,
affecting 388 test functions. Common ones such as `addc`, `subfze`, `eqv` and
`rlwnm` are among them. None of the 61 appear anywhere in Call of Duty 3, which
is why its recompilation is clean, but any other title would run into them
immediately.

### The kernel so far

| Area | State |
| --- | --- |
| Memory | `Nt*` virtual and `Mm*` physical allocation over a bump allocator, with demand commit underneath |
| Threads | Guest threads are host threads, each with its own processor context and guest stack |
| Synchronisation | Critical sections, events, semaphores and mutants block and wake for real, on one lock and one condition variable |
| Thread local storage | Per thread values, shared slot allocator |
| Files | Guest paths such as `game:\sp\global.cod` resolve under the installed folder; writes are redirected to a `saves` folder so the install is never modified |
| Profile and input | One local player, no controller connected, every system dialog answers as dismissed |
| Time, config, debug | Real clock and timebase, a small table of console settings, `DbgPrint` printed |

Everything above was written to be honest about what it does not do. The
interrupt level calls are bookkeeping because there are no interrupts to mask.
`MmGetPhysicalAddress` is the identity function because nothing outside the CPU
reads guest memory yet. Directory enumeration reports no files rather than
inventing a listing.

### First time setup

The game's own data cannot ship with the recompilation, so `CoD3.exe` asks for
it once. It reads Xbox 360 disc images directly, walking the XDVDFS filesystem;
XGD1, XGD2, XGD3 and stripped images are all recognised. It can also copy an
already extracted folder, or use one where it is without copying, which matters
when the disc is 5.77 GB.

Before writing anything it checks that the image holds a `default.xex` of the
same size this build was recompiled from, so the wrong disc or the wrong region
is caught in seconds rather than after a multi-gigabyte extraction.

The reader was checked against a real XGD2 image: 553 files, 5.77 GB, and both
a root file and a nested one extracted byte for byte identical to the
already-extracted copies.

### The executable

| | |
| --- | --- |
| File | `default.xex`, single player, 7,254,016 bytes on disk |
| Image base | `0x82000000`, 12.19 MB mapped |
| Entry point | `0x82344D00` |
| `.text` | `0x820A0000`, `0x4D8E8C` bytes, plus 7 smaller `.embsec_*` code sections |
| `.pdata` | 10,896 runtime function entries |
| Title update | none ships with this dump, so no XEXP patch is applied |

The multiplayer executable `codmp_xenonf.xex` is also in `private/` but has not
been processed. It is a separate build of the same engine and will need its own
config with its own addresses.

## What was needed to get here

Three problems had to be solved beyond pointing the tool at the file.

**Locating the required addresses.** `config/CoD3.toml` needs the eight register
save/restore helpers plus `setjmp` and `longjmp`, and XenonRecomp cannot find
them itself. `tools/CoD3Scan` locates the helpers by byte pattern; each matched
exactly once. `longjmp` was found through its `RtlUnwind` call at `0x8234EFC0`,
giving `0x8234EEC0`; `setjmp` is the leaf function at `0x82351CD4` that writes
the same `jmp_buf` layout and returns zero. The layout is `0x00` for f14-f31,
`0x90` for the stack pointer, `0x98` for r13-r31, `0x130` for CR, `0x134` for
LR, `0x138` for the unwind flag.

**Truncated functions.** The function analyzer ends a function at a jump-table
`bctr`, because the table data that follows does not decode as code. That
produced 1,360 switch cases pointing outside their own function. `CoD3Scan
--fixfuncs` reads those reports, follows every case body to its terminating
`blr` or branch, and takes the furthest end as the real boundary; overlapping
reports are merged, since a function with several jump tables gets reported once
per table. The resulting 56 explicit entries in the TOML bring the error count
to zero. Bounding by the next alignment padding instead looks simpler but
overshoots into the following function wherever two functions sit adjacent with
no gap, which silently fuses them.

**Instructions that vanished.** XenonRecomp warns about an instruction it cannot
translate and then emits nothing for it, and the caller discards that failure.
The code still compiles, and computes wrong results with no trace. 3,233
instructions across 34 opcodes were being dropped. Two changes fixed this:
unrecognized instructions now emit `PPC_UNIMPLEMENTED_INSN`, which traps by
default, so the gap is impossible to miss; and all 34 opcodes are now
implemented in `tools/XenonRecomp/XenonRecomp/recompiler.cpp`.

The implemented opcodes, by how often they appear: `vslh` (874), `vsrah` (541),
`vsubshs` (522), `vspltish` (330), `vmaxsh` (231), `vandc` (148), `vminsh` (88),
`vpkswss` (76), `vctuxs` (64), `vcmpgtsh` (54), `vcmpgtsw.` (49), `vaddsws`
(45), `vaddsbs` (26), `vcmpequh` (16), `vavguh` (16), `bso` (16), `vsel128`
(13), `vpkswus` (12), `vrfip` (11), `vsrh` (10), `vrlh` (9), `vcfpuxws128` (9),
`vpkshss` (7), `vsububm` (7), `vsrab` (6), `vpkuhus` (6), `mulhd` (6), `cror`
(2), `crorc` (2), `bns` (2), `mulhdu` (1), `bsolr` (1), and the condition-code
forms of `vcmpgtuh` and `vcmpequh`.

These follow the conventions the existing implementations use. Element-wise
operations need no endianness correction, because the recompiler reverses whole
16-byte vectors and a uniform per-lane operation commutes with that permutation.
Pack instructions take their sources in reverse order, matching the existing
`vpkshus`. Halfword shift counts come from byte `i * 2` of the reversed vector,
which is the low byte of halfword `i`. Condition-register updates use the
16-byte `movemask_epi8` form of `setFromMask`, which is exact at every lane
width because a vector compare writes all-ones or all-zeroes per lane.

Two of these deserve review by someone with hardware to test against.
`simde_mm_vctuxs` in `ppc_context.h` is a scalar loop, because SSE has no
float-to-unsigned conversion before AVX-512; it saturates NaN and negatives to
zero. And `bso` / `bns` / `bsolr` read the `so` field of a condition register,
which is a faithful translation of the instruction but only as good as the
summary-overflow tracking elsewhere in the recompiler, which is incomplete.

## What is missing

Every kernel import has an implementation, but implemented is not the same as
working. Three areas are deliberately hollow, and each is named where it sits in
the code:

- **Graphics.** The path runs end to end and the loading screen is on screen.
  What is missing is breadth rather than structure: depth testing, blending,
  texture filtering, compressed texture formats, most vertex formats, and
  indices carried in the packet. Each is counted and named at run time.
- **Audio.** The 21 `XMA*` and `XAudio*` calls accept contexts and submitted
  frames and discard them. XMA is a hardware codec that has to be decoded in
  software and mixed.
- **Exceptions.** `RtlRaiseException`, `RtlUnwind` and `__C_specific_handler`
  report and stop, because XenonRecomp does not translate exception handling at
  all. Anything that actually throws is unrecoverable.

Smaller gaps, each marked in place: `MmGetPhysicalAddress` is the identity
function because nothing outside the CPU reads guest memory; directory
enumeration reports no files rather than inventing a listing; floating point
arguments to the guest `printf` family are printed as `<float>` because they
travel in registers the variadic reader does not walk; `RtlImageXexHeaderField`
returns nothing because the loaded image's headers are not kept in guest memory.

And the recompilation itself is still unverified. Nothing has checked that the
translated instructions compute what the originals did.

## Next steps, in order

1. **The exception the title raises on some runs.** The code, the address and
   the guest call chain are printed. Whether it is the title reporting a problem
   or using exceptions as ordinary control flow decides what to do about it.
2. **Depth testing.** RB_DEPTHCONTROL is read and not applied. Nothing with
   overlapping geometry draws correctly without it.
3. **The rest of the texture formats.** The four byte and block compressed
   forms are decoded; the packed and sixteen bit ones are not, and each one
   names itself at run time.
4. **Texture filtering.** Sampling is nearest, so edges are hard where they
   should be smooth.
5. **Indices in the packet.** Source select 1 carries indices inline, and those
   draws are skipped.
6. **Audio.** The render callback now runs at the right rate, but the 21
   `XMA*` and `XAudio*` calls still decode and play nothing.
5. **Tests for the 18 added instructions Xenia does not cover**, and extending
   `tools/PpcAsm` to the extended mnemonics so the other 41 test files can run.
6. **Only then the optimization flags** in `config/CoD3.toml`. XenonRecomp's
   README is explicit that enabling them before the recompilation runs makes a
   miscompile impossible to attribute.

## Reproducing

Requires CMake 3.20+, Ninja, Clang (tested with the LLVM 21 install at
`C:\Program Files\LLVM`), and the Visual Studio Build Tools for the Windows SDK
headers. Clang is not optional: the generated code depends on Clang intrinsics
and the `alias` attribute used for function hooking.

```powershell
.\scripts\recompile.ps1 -BuildTools -BuildLib -BuildHost
```

`CoD3RecompLib/ppc/` is entirely generated. Never edit it; regenerate instead.

The game files are not part of this project and must come from your own disc
dump. `default.xex` sits in `CoD3RecompLib/private/`, which is excluded from
version control. Keep it that way: reverse-engineering a copy you own is one
thing, redistributing the game code is straightforward copyright infringement,
which is why the recomp projects ship tools and never data.
