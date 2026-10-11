# Agent Instructions — COD3 Recomp

You are working in the private repo: https://github.com/jxnplays/cod3-recomp

## Absolute Rules
- This is a clean, standalone project.
- Source ISO dump is read-only, and is not in the repository. Ask the user for
  the path to their own `default.xex` rather than assuming one.
- Goal is a static recomp (XenonRecomp or RexGlue preferred) to a native PC binary.
- Minimize conversation. Complete the recomp task.

## Referencing other projects

Public and private reference projects may be consulted, including mws-and-box,
NX1, IW4L, Fable 2 recomp, and any other Xbox 360 recompilation. Take
architectural ideas from them.

Do not vendor their code, tables, or configuration into this repository, and do
not push to their repositories. Anything that lands here has to be derived from
this game's own binary or written from scratch, so that the project stands on
its own.

## Success Criteria
- Repo contains a working toolchain setup.
- A PC binary that launches the recompiled COD3 and asks for the game's own
  files when it cannot find them.
- Basic graphics modernization hooks (resolution, etc.) noted or stubbed.
- All work committed to this repo.

## Workflow
1. Clone the repo if not already local.
2. Analyze the dump.
3. Configure and run the recompiler.
4. Integrate runtime/glue as needed.
5. Build and verify.
6. Commit and push.

Stop only when a playable PC binary exists or a clear blocker is documented in
`docs/`.

## Reporting

State what was verified and what was not. If something is blocked, say what
blocks it and what the next concrete step is, rather than implying progress was
made. If an earlier conclusion turns out to be wrong, correct it in the docs and
say so — do not quietly rewrite history.