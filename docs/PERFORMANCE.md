# Performance, measured

The first version of this file would have been a list of things that felt
slow. This one is numbers, and how they were got.

## There was no instrument

`RenderStats::Tick()` prints the whole picture: frame rate, milliseconds a
frame, CPU and GPU time, draws, state switches, uploads, and a per-section
breakdown of where a draw's CPU time goes. It had **no callers anywhere in
the tree**.

So `COD3_RENDER_STATS=1` and `COD3_RENDER_PROFILE=1` did nothing. Not "printed
nothing useful" - nothing at all. Every performance claim before this file
was an argument from reading code. `RenderStats::Tick` is now called at the
swap, throttled to a second, by `TickStats()` in `render_d3d11.cpp`.

Two traps in the same area, both of which cost an hour:

- The gate is **`COD3_RENDER_STATS`**, not `COD3_RENDER_PROFILE`. The profile
  only adds its sections on top of a line that `COD3_RENDER_STATS` starts.
  Setting `COD3_RENDER_PROFILE` alone looks like it should work and does not.
- `COD3_RENDER_PROFILE` alone also times the sections, so the two together
  give both the frame line and the breakdown.

## The measurement

Saint Lo, `COD3_NOFILMS=1 COD3_MAP="saint_lo,20"`, one second of frames:

| | frame | CPU | GPU | fps | draws | indices us/draw | fingerprint KB/frame |
|---|---|---|---|---|---|---|---|
| watch off | 14.11 ms | 9.71 ms | 9.69 ms | 70.8 | 915 | 5.46 | 22861 |
| watch on | 6.88 ms | 2.66 ms | 6.68 ms | 145.1 | 918 | 0.16 | 2381 |

Per draw, the profile went from

    8.60 us: state 0.09 targets 0.08 pipeline 0.06 textures 1.37
             buffers 0.85 constants 0.28 bind 0.38 indices 5.46 draw 0.03

to

    1.86 us: state 0.09 targets 0.08 pipeline 0.05 textures 0.27
             buffers 0.49 constants 0.29 bind 0.37 indices 0.16 draw 0.06

**Index conversion was 62% of all CPU draw time.** 22.9 MB of indices a
frame, byte-swapped out of guest memory into the index ring on every single
frame, for bytes that mostly had not changed. That is 5.4 us x ~915 draws,
about 5 ms a frame, spent redoing identical work.

## Why the fix exists at all

`WriteWatch` protects the guest's pages read-only and catches the fault on a
write. A resource is kept converted only while its pages report untouched,
which is a far cheaper question than hashing 22.9 MB every frame.

The profile line said **0 found the same by their pages**, so this path had
never once hit - and with `Tick` uncalled, nobody could have seen that. With
it on: 1552 hits a frame, 132 fingerprints instead of 1658.

## What was NOT verified, carefully

Frame-by-frame image equality between the two settings **was not
established, and the attempt is worth recording as a failure.**

`COD3_SHOT_EVERY` and `COD3_SHOT_AT` key on the host's present counter, not
on the guest's simulated time. The host runs at 145 fps with the watch on and
70 with it off, so "frame 3000" is a different moment of the level in each -
the village's opening flyover is on a timer, and the two runs end up in
different scenes entirely. An index-by-index diff then reported a mean
difference of 33 and a "67% of pixels changed", which is two different
scenes, not a rendering fault. `scripts/imgdiff.ps1` and
`scripts/imgnearest.ps1` were written to settle it and could not, because the
runs are not comparable by construction.

What was done instead, and it is weaker:

- **210 s soak with the watch on**: 29 frames, **0 guest faults, 0
  exceptions**, 140 fps sustained at the end.
- Frames across the soak inspected by eye, including deep gameplay (compass,
  grass, smoke, soldier models, all present and correct). A missed
  invalidation shows as stale or frozen geometry, and none was seen.

So: no image difference was ever measured, and the case for the default rests
on the numbers plus a clean soak and an eyeball. A human playing it is still
worth more than either.

## What is left, in order of what the numbers say

1. **1657 texture switches a frame against 928 draws** - about 1.8 binds per
   draw. The switch count is larger than the draw count, which is the thing
   to explain.
2. **1820 constant blocks a frame** against 928 draws, for ~1 KB a frame of
   streamed constants.
3. **`0 mergeable draws`** - the profile counts draws that could have joined
   the one before and finds none. Either the merging is not implemented or
   its test is too strict; it has never reported a non-zero.
4. **GPU is now the wall.** 6.68 ms of GPU against 2.66 ms of CPU, so from
   here the frame is set by the console's own shaders and resolution. This is
   what SSAO, SMAA and FSR will move.

`COD3_RENDER_STATS=1 COD3_RENDER_PROFILE=1 COD3_GPU_PROFILE=1` is the command
that produces all of the above. It works now.
