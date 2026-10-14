# Roadmap

Written 2026-10-14 against the feature spec in `cod3-reborn-features.md`, which
this project is built to. That file is the goal; this is where we are against
it and what the next pieces are.

The ordering principle is not enthusiasm, it is **what the present path can
carry**. Everything in the first group happens after the frame is drawn, in one
shader, with no change to how the title is emulated. That is the cheapest,
safest, highest-yield work available, and it is most of the top of the spec's
priority list.

## Where we are

| Spec priority | State |
|---|---|
| 1. Ultrawide + HUD scaling | **built, default off, never seen filled** - needs a wide monitor |
| 2. FSR/XeSS + CAS | CAS **measured on a level: +6.9% detail, no ringing**; FSR/XeSS not started |
| 3. SSAO/GTAO | not started |
| 4. Soft shadow filtering | not started |
| 5. Colour grading / LUT | **measured on a level: saturation +122%, detail unchanged** |
| 6. SMAA | not started |
| 7. FOV to 180 | **done**; viewmodel scaling not done |
| 8-10. Sky, weather, DX12/Vulkan | not started |
| 11. Modern controls | not started |
| 12-13. HUD feedback, photo mode | not started |
| stretch. Multiplayer | relaunch fixed; blocked on the call table |

## Group 1 - the present path

One shader, after the frame is resolved, before it reaches the window. Nothing
here can break emulation, because nothing here is emulation.

**CAS / RCAS sharpening** (priority 2). **BUILT.** A third program in the
choice `PresentLocked` already makes between the present and FXAA shaders. It
sharpens by pushing away from a 1-2-1 weighted 3x3 box, clamped to the local
extreme, so it cannot invent a value the neighbourhood did not already contain.
In a flat region the box equals the centre and nothing happens, which is what
makes it contrast adaptive without a separate contrast measurement.

It is **not** AMD's CAS: same idea, its own coefficients, untuned against the
real thing. It replaces FXAA rather than following it, because they work on
edges in opposite directions, and it says so in the log when both are selected.

Not FSR or XeSS. Those need an upscaler and a scaling chain, which is a larger
piece than a present-path pass.

**Colour grading** (priority 5). **BUILT.** Brightness, contrast and
temperature are a 256-entry table; saturation is in the shader. All four
default to the identity.

Saturation cannot live in the table, which the self-check proved rather than
assumed: a per-channel ramp cannot mix a pixel toward its own luma, so the
first version was a no-op at every slider position.

The table is `R8G8B8A8`, not the `R10G10B10A2` the display table beside it
uses. A UNORM sample decodes as n / (2^bits - 1), so 8-bit values in 10-bit
fields scale the whole picture by 255/1023. That shipped in the first version,
made everything four times too dark, and passed the original check - see below.

**Vignette and film grain** (priority 5, optional items). Same shader, same
pass. Not started.

## Group 2 - needs the frame's depth and normal

**SSAO/GTAO** (priority 3) needs a depth buffer and a reconstructed normal. The
renderer has the depth target - the log shows `depth target at tile 1014` on
every frame - so this is reachable, but it is a real renderer feature rather
than a post pass, and it is a multi-night piece.

**Soft shadow filtering** (priority 4) is inside the shader translation rather
than the present path, because shadow sampling happens where the title's own
pixel shader runs.

## Group 3 - input and feel

**ADS sensitivity multiplier, toggle vs hold ADS** (priority 11). All in
`input.cpp`. Note the standing constraint: mouse work was reverted once already,
so this needs the same adversarial review as everything else before it lands.

## The rule for every item in this list

1. Default is the identity. A feature that changes the picture when it is off
   is a regression, not a feature.
2. `scripts/selfcheck.ps1` passes, including its build check.
3. An adversarial reviewer has been through it, told in advance that its default
   hypothesis is that the change is wrong.
4. `MEASURED:` in the commit message quotes the run. `REASONED:` is allowed and
   says so.
5. The 15-level sweep still passes. Ultrawide and sensitivity both changed
   shared code and both were caught only by being played.

## What "the default is the identity" has to mean to be worth anything

Group 1 rests on rule 1, and the first attempt at it produced three defects
that reading the code did not catch. They are written down because the next
feature in this list will hit the same three.

**Assert what comes out, not what goes in.** The first identity check compared
the bytes packed into the table against the values the CPU meant to write. It
passed while the picture was four times too dark, because a texture format
decodes between the write and the read. Every assertion now goes through
`Grading::Decode`, which mirrors what the GPU does.

**Test the shipped function, not a paraphrase of it.** An earlier version of
this repository's own logic check pasted a paraphrase into a here-string and
ran that, so reverting the bug it guarded changed nothing and every check
still passed. `scripts/grading_identity.cpp` is a real file and includes the
real header.

**A check that cannot fail is worse than no check.** Each check here has been
run with a deliberate defect injected and confirmed to go red, naming it: a 1%
contrast pivot error, and the whole 10-bit format mistake.

The second of those is the interesting one, because the first version of the
check did *not* catch it and I initially believed that it had. Injecting the
bug by changing the packing *and* the decode together changed nothing that the
check could see, because they were consistent with each other. Only the
texture format - which lives in a different file and is not mentioned anywhere
in `grading.h` - was the actual defect, and the check was blind to it by
construction. It now reads `render_d3d11.cpp` and compares the format created
there against the constant the decode is derived from.

That is the general shape of this mistake: two things that are each correct in
isolation, and wrong together, with the test covering one of them.

**And the arithmetic is not exact where it looks exact.**
`l + (t - l) * 1.0f != t` for a great many 8-bit colours in IEEE - false for
greys, true for roughly one colour in six. The shader takes a branch at the
default instead of relying on it, and the self-check measures that
inexactness rather than asserting it in prose, so deleting the branch becomes
a visible loss instead of a silent one. A claim this load-bearing should be
exact by construction, not by argument.

## What is measured about these two, and what is not

**Measured, on a real level.** Saint Lo, booted directly with `COD3_MAP`, the
picture captured by `scripts/capture.ps1` and compared with `scripts/ab.ps1`.

The comparison that matters is the alternating one. Two separate runs cannot be
compared - the camera is somewhere different each time, and the first attempt
to measure sharpening that way said it had *lowered* detail, which was the
scene talking and not the feature. `COD3_SHOT_ALT` makes consecutive captures
alternate between the player's settings and a forced set, flipping on exactly
the capture frames, so each pair is the same scene one frame apart.

| | sharpening 0.6 | grading 1.15 / 1.25 / 1.5 |
|---|---|---|
| pairs compared | 18 | 19 |
| high frequency energy | **+6.93%** | −1.33% |
| mean brightness | +0.11% | −8.94% |
| saturation | −3.35% | **+122%** |

That is what each control should do. Sharpening raises detail and leaves
brightness alone. Grading raises saturation, moves brightness about mid grey,
and leaves detail alone - which also confirms grading is not quietly
sharpening. At zero guest faults throughout.

**Seen, not inferred.** The pictures were looked at. At the defaults the game
renders correctly - which is the strongest single piece of evidence that the
10-bit format bug is gone, since that bug darkened everything by four times and
would have been impossible to miss. Sharpening at 0.6 is clean, with no ringing
or halos around the white text or the sky/buildings edge. Grading is
unmistakable: at contrast 1.3 / saturation 1.4 / temperature 3 the cold blue
fog of Saint Lo goes hard sepia, which is a lot, and at 1.15 / 1.25 / 1.5 it is
a clear but usable lift.

**Not measured.** Nobody has pressed Apply in the menu mid-game and watched the
picture change. The settings reach the present path every frame and the ini
round trip proves the value arrives, but the button itself has not been pushed
by anything but the self-check.

## Known gaps that are not features

- CI cannot compile `CoD3Host`: it needs the generated headers under
  `CoD3RecompLib/ppc`, which are XenonRecomp's output and need the player's
  own `default.xex`. So "does it still compile" is caught by
  `scripts/selfcheck.ps1` locally only.
- Three checks in the self-check are pattern matches rather than proofs, and
  `docs/PROCESS.md` names them: the `Render::VideoMode` correspondence, the
  `fprintf` conversion count outside `%d`/`%s`/`%f`, and the documentation
  overclaim detector.
- Ultrawide has never been run filled. This machine is 16:9, so the wide branch
  is compiled and arithmetic-checked but unseen. It needs a wide monitor before
  it can be called working.
- The button prompts do not switch. `ps2` is not the switch; the mechanism is a
  key-name to glyph-cell lookup in one font. Three routes are written up in
  `docs/RESUME.md`, cheapest first.