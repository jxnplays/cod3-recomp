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

## Measured, and the clearest real constraint

**Both heaps are exhausted.**

```
heaps: physical 455.1 of 511.0 MB handed out, 0.0 MB free in 0 ranges
       virtual  16.0 of 752.0 MB, 0.0 MB free in 0 ranges
```

Zero free in **zero** ranges, which is a different statement from "almost full" —
there is no fragmentation slack to reclaim, and nothing is being handed back. The
physical heap is at 89% of the 511 MB the title believes the console has.

This is the first thing to investigate. It bounds everything else: a title that
cannot allocate cannot keep its textures resident, and the fix may be reclamation
rather than capacity. It also plausibly explains part of why our build reads
fewer files than the upstream prebuilt did in the same wall-clock time.

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

1. **Heap reclamation.** Both heaps at zero free with nothing returned. Everything
   else is bounded by this.
2. **Sampler binding for ordinary draws.** One grep to resolve; decides whether
   texture filtering is a real gap or a misreading.
3. **Format census from a run.** The runtime already counts them; collect the list
   instead of guessing which are missing.
4. **Verification for the 18 added instructions.** Expensive, and it is what makes
   every other visual fix trustworthy.
5. **Exception support.** Toolchain work; defer until the above are settled.

Nothing on this list is on the remaster feature roadmap yet — settings menu,
modern options, ultrawide. Those are additions on top of a correct base, and
items 1 and 4 are what make the base correct.
