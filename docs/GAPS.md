# Gaps

What is actually missing, measured rather than inherited.

`STATUS.md` predates this file and is **stale in three places** — it lists things
that are in fact implemented. Those are corrected below rather than quietly
dropped, because "the notes say it's broken" and "it is broken" are different
claims and only one of them was true.

Every entry says how it was established. Nothing here is inferred from reading a
TODO.

---

## Corrected: four claims in STATUS.md that are wrong

**Depth testing — implemented.** `STATUS.md` lists it as read-and-not-applied and
ranks it second in next steps. `render_pipeline.cpp:149` builds a full
`D3D11_DEPTH_STENCIL_DESC` from `RB_DEPTHCONTROL` — enable, write mask,
comparison function, and both stencil faces — and `render_d3d11.cpp:844` binds it
per draw. There was no work to do here.

**Blending — implemented.** Also on the missing list. `render_pipeline.cpp:108`
sets `IndependentBlendEnable`, and 117–120 set per-target `BlendEnable`,
`SrcBlend`, `DestBlend` and `BlendOp` through a `BlendFactor()` translation.

**Audio — working.** Listed as "the 21 XMA/XAudio calls accept contexts and
discard them". Measured on our own build:

```
xma:   1 contexts, 20066 enables, 1198 packets decoded, 0 refused, 80252 blocks written
audio: 21836 frames played, 2 dropped, loudest sample 39.3% of full scale
```

`avcodec-62.dll` loads, the XMA decoder opens, and it decodes. It needed the
FFmpeg headers the build had been reporting missing — see `scripts/get_ffmpeg_headers.ps1`.

---

## Measured over a ten minute run

Our build, from our own source, against a full game dump:

```
[ 629s] fps 60  frames 37741  files 39 (46.8 MB read)  threads 37
         allocated 489.0 MB  audio 117838  packets 15407101  draws 2112051

xma:   1 contexts, 127976 enables, 7670 packets decoded, 0 refused, 511820 blocks written
audio: 117836 frames played, 2 dropped, loudest sample 39.3% of full scale
heaps: physical 455.1 of 511.0 MB handed out, 0.0 MB free in 0 ranges
       virtual  16.0 of 752.0 MB, 0.0 MB free in 0 ranges
```

It reaches the **main menu** and holds **60 fps** there, decoding audio, for ten
minutes without degradation. A frame captured through the title's own screenshot
path shows the front end rendering from the game's own shaders.

### The heaps: not the emergency it looked like

Both heaps report `0.0 MB free in 0 ranges`, and the physical heap sits at 89% of
the 511 MB the title believes the console has. That reads like exhaustion, and
this file said so before the long run.

It is not, at least not yet. Across the run `allocated` held at **489.0 MB** and
the physical heap at **455.1 MB** — flat from the first minute to the tenth.
Nothing grows and nothing leaks.

What the zero does mean is that the allocator **never hands anything back**: zero
free bytes in zero ranges says reclamation does not happen, not that there is no
memory. So it is a standing ceiling rather than a fault, and it sits lower in the
order than it first appeared. It becomes urgent when a level allocates more than
the menu does — which is the next measurement worth taking, and it needs a long
run through an actual level rather than the front end.

---

## Real gaps, with what is known about each

### Texture filtering — implemented, fourth stale claim

Also on the missing list as "sampling is nearest". It is not.

`MakeSampler` (`render_pipeline.cpp:230`) translates the guest's sampler registers
in full: mag/min/mip filter, anisotropy, the three address modes, border colour,
and `MaxLOD` collapsed to 0 for a texture the title samples with no mip level —
"base map" — because its levels are not there to be used.

It also takes a filter override: Native, Bilinear, Trilinear or Anisotropic
(`RenderPipeline::Filter`). **Anisotropic filtering is already a user setting.**

The `g_pointSampler` at `render_d3d11.cpp:295` is real but is used for the
fullscreen copy and present passes, not for textured geometry. The earlier
reading that described it as "the" sampler was wrong: `BindSampler` binds
`texture.sampler` per draw, which comes from the guest state table.

### Texture and vertex formats — partially mapped

`render_resources.cpp` maps a useful set: `R8G8B8A8_UNORM`, `R10G10B10A2_UNORM`,
`R16G16B16A16_FLOAT`, `R16G16_UNORM`, `R16G16B16A16_UNORM`, `R16G16_FLOAT`, and
block-compressed forms below in the same switch.

Not established: which Xenos formats the title actually requests that fall through
the switch. The runtime counts and names them at run time, so **the right
measurement is a run, not a read** — collect the counted names and work from
that list.

### Indices carried in the packet — unverified

`STATUS.md` says source select 1 carries indices inline and those draws are
skipped. Not checked against the current code. The draw counters in the log would
answer it directly: skipped draws are counted per frame.

### Exceptions — unrecoverable, and unchanged

`RtlRaiseException`, `RtlUnwind` and `__C_specific_handler` report and stop.
XenonRecomp does not translate exception handling at all, so anything that
actually throws is fatal. This is a toolchain limit rather than a runtime gap,
which makes it expensive: it needs recompiler work, not host work.

### Recompilation unverified — the deepest gap

Nothing checks that the translated instructions compute what the originals did.
`scripts/verify.ps1` checks the recompiler against Xenia's instruction tests, and
`tools/patches/XenonRecomp.patch` adds instructions Xenia does not cover — those
added instructions have no tests at all.

Everything visual above sits on top of that. A miscompile in a patch would look
like a rendering bug, and we currently have no way to tell the two apart.

### The world stops rendering while the HUD does — Blkbrn, vehicles

Found on `blkbrn` by playtesting, not by a test. Being shot while in the
convoy tank — an RPG, or an enemy tank — blacks the screen for five to ten
seconds, then it comes back by itself. It stops when the player dies.

**It is not a black screen.** The HUD keeps drawing throughout. Captured
with F12 during the event, the vehicle compass, the red damage chevron, the
grenade icon and the crosshair are all present and correct. Everything above
the HUD is empty.

Measured on that capture: 44 sample points across the upper two thirds of
the frame are **pure black, zero lit**, while 168 sample points inside the
compass are lit. The file is 121 KB against 1.2–2.7 MB for a normal frame.
The world is not dark or miscoloured; nothing is written there at all.

**What it is not**, each checked rather than assumed:

- **Not dropped draws.** `COD3_SKIPLOG=1`, which fires only when the skip
  counts change between frames, never fired during the event.
- **Not a GPU stall.** "workers waited 0.00 s for the GPU" throughout.
- **Not a crash or an exception.** No `RtlRaiseException`, no device
  removal, no watchdog reset. The game survived and carried on.
- **Not the renderer falling over.** 2,945 draws and 1.18M triangles a frame
  at ~100 fps, steady, across the event.
- **Not a render target or viewport mismatch.** The captured frame's targets
  and viewports match a normal frame closely — scene pass 520/520/-312/312
  into 3600×2492 in both.
- **Not a shader set difference.** Six vertex shaders appear only in the
  black frame, but they account for 32,337 of 1,448,591 indices — 2.2%. That
  cannot empty the screen.
- **Not a near-empty draw list.** The captured frame submits 1.45M indices,
  against 2.29M in a normal one.

So the world is submitted, with correct targets, viewports and depth state,
at full rate — and does not appear. The HUD, which draws afterwards in screen
space, is unaffected.

**Most likely, still unproven:** the pass that resolves the rendered scene
into the backbuffer runs wrong for those frames. `resolves` is counted (22 a
frame in normal play) but the frame dump does not expose which surface was
resolved, so this is inference from the symptom rather than a measurement.

**To attack it next,** capture the frames either side of the transition.
`COD3_PICTURES=<second>` logs the frames after that second in full and
pictures their draws one by one, but it needs the second the event actually
happens on. Firing it at a guessed second missed the event once, because the
game had not reached the convoy yet. Someone reproducing it should note the
time and say so.

Also worth noting: the dead giveaway is that this is **vehicle-specific** and
Blkbrn is one of the fourteen levels nobody had tested. The playtesting
earned its keep by finding a bug the test suite could not have.

---

## Performance

Not a correctness gap, but measured:

- 60 fps at the main menu, 2.45M packets, 335k draws
- `8 distinct programs`, all from the disk cache, 0 compiled at run time
- Render thread cost is not the bottleneck at the menu; `[ 100s]` shows 60 fps
  sustained

The upstream author reported 30–60 in levels and 55–75 on maxed settings, so
there is headroom to measure before optimising. The heap exhaustion above is the
more likely limiter.

---

## Ordered by what unblocks the most

1. **Verification for the 18 added instructions.** Expensive, and it is what makes
   every other visual fix trustworthy. Everything visual rests on it.
2. **Format census from a run.** The runtime already counts formats it does not
   handle; collect the list instead of guessing which are missing.
3. **A run through an actual level, not the menu.** The heap ceiling is flat at
   the front end; whether it holds under a level's allocations decides how urgent
   heap reclamation is.
4. **Exception support.** Toolchain work rather than host work; defer.

The first two measured items turned out not to be gaps at all — depth, blending,
filtering and audio are all implemented and working. That is worth saying plainly:
the inherited notes understated this codebase, and the next corrections may
understate it further.

Nothing on this list is on the remaster feature roadmap yet — settings menu,
modern options, ultrawide. Those are additions on top of a correct base, and
items 1 and 4 are what make the base correct.
