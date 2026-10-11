# Agent Instructions — COD3 Recomp

You are working in the private repo: https://github.com/jxnplays/cod3-recomp

## Absolute Rules
- This is a clean, standalone project.
- Do not reference, copy, or import anything from mws-and-box, NX1, IW4L, or any other prior work.
- Source ISO dump is read-only at: `X:\AI\Recomp Projects\Call of Duty 3`
- Goal is a static recomp (XenonRecomp or RexGlue preferred) to a native PC binary.
- Minimize conversation. Complete the recomp task.

## Success Criteria
- Repo contains a working toolchain setup.
- A PC binary that launches the recompiled COD3.
- Basic graphics modernization hooks (resolution, etc.) noted or stubbed.
- All work committed to this repo.

## Workflow
1. Clone the repo if not already local.
2. Analyze the dump.
3. Configure and run the recompiler.
4. Integrate runtime/glue as needed.
5. Build and verify.
6. Commit and push.

Stop only when a playable PC binary exists or a clear blocker is documented in `docs/`.
