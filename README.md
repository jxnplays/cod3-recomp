# cod3-recomp

Static recompilation of **Call of Duty 3** (Xbox 360) to native PC.

**Goal:** One-shot static recomp using XenonRecomp / RexGlue (or best available tool). Produce a playable PC port with enhanced/modernized graphics options. No overlap with other projects.

**Source:** Dumped Xbox 360 ISO located at:
```
X:\AI\Recomp Projects\Call of Duty 3
```

## Constraints
- Clean repo. Do not import code, assets, or patterns from mws-and-box, NX1, or any other prior project.
- Static recompilation only (no dynamic emulation layer as the primary path).
- Target: native Windows PC binary.
- Graphics: add modern options (resolution scaling, higher FOV, improved lighting/post if feasible without breaking authenticity).
- One primary task: get a working recomp. Minimize back-and-forth.

## Recommended Tools
1. **XenonRecomp** — Primary static recompiler for Xbox 360 titles.
2. **RexGlue** — Alternative/complement for glue code and runtime.
3. Supporting: Xenia (reference only for behavior), standard Xbox 360 ISO extraction tools already used for the dump.

## Initial Steps for the Agent
1. Clone this repo.
2. Inspect the ISO dump at the path above (read-only).
3. Identify the best entry point with XenonRecomp or RexGlue.
4. Set up the recomp pipeline in this repo.
5. Produce the initial PC build.
6. Document any graphics enhancement hooks added.
7. Commit progress directly to `main` or a single working branch. Keep history clean.

## Repo Layout (to be expanded)
- `README.md` — this file
- `docs/` — notes, tool versions, known issues
- `tools/` — scripts or configs for the recomp toolchain
- `src/` — generated or hand-written glue/runtime code
- `build/` — output binaries (gitignored)

## Status
- [ ] ISO analyzed
- [ ] Toolchain selected and configured
- [ ] Initial recomp generated
- [ ] PC binary builds
- [ ] Basic playability confirmed
- [ ] Graphics options stubbed

Private project. Do not publish.
