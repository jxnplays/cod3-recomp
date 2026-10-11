![COD3 Recomp Logo](docs/logo.png)

<div align="center">
  <img src="docs/status.svg?v=1" width="900">
</div>

# COD3 Recomp

Static recompilation of **Call of Duty 3** (Xbox 360) to a native Windows PC
binary.

The Xbox 360 executable is translated ahead-of-time from PowerPC to x86-64 with
[XenonRecomp](https://github.com/hedge-dev/XenonRecomp), and this project
supplies the runtime that the translated code links against: guest memory, the
PowerPC dispatch table, and an implementation of the Xbox 360 kernel API
surface the title imports.

There is no emulation layer and no dynamic translation at run time — once
compiled, the game code is ordinary x86-64 that the OS runs natively.

**Source dump** (read-only): `X:\AI\Recomp Projects\Call of Duty 3`

---

## Current state

See **[docs/STATUS.md](docs/STATUS.md)** for the honest, detailed breakdown. In
short:

| Stage | Status |
|---|---|
| Dump analysed (XEX, PE, imports, jump tables) | Done |
| XenonRecomp toolchain built from source | Done |
| `default.xex` translated (29,731 functions, 98 MB of C++) | Done |
| Compiled to a native `COD3.exe` | Done |
| Host loads the guest image and enters the entry point | Done |
| Xbox 360 kernel API layer (179 imports; 29 hand-written, 150 stubs) | Partial — see STATUS |
| **Renders the game** | **Not yet — blocked on Xenos GPU command processor emulation** |

The port loads the real recompiled executable and transfers control to its
original entry point at `0x82344D00`. It does not yet draw. The reason is
concrete and not a limitation of the recompiler: Call of Duty 3 imports **no
D3D functions at all**, because the Xbox 360 graphics stack is statically linked
into the executable, so there is no graphics API boundary to implement or wrap.
See **[docs/GRAPHICS.md](docs/GRAPHICS.md)**.

---

## Quick start

```powershell
# 1. Toolchain: clone + build XenonRecomp (~2 min)
powershell -ExecutionPolicy Bypass -File tools\setup.ps1

# 2. Full pipeline: analyse, recompile, compile the port
powershell -ExecutionPolicy Bypass -File tools\build.ps1

# 3. Run
build\host\COD3.exe --xex "X:\AI\Recomp Projects\Call of Duty 3\default.xex"
```

Prerequisites: VS 2022 Build Tools, LLVM 18+ (20.x used here), Python 3.11.
Full details in **[docs/SETUP.md](docs/SETUP.md)**.

---

## How it works

```
default.xex
    |
    |  XenonAnalyse            detect 323 jump tables
    v
COD3_switch_tables.toml
    |
    |  XenonRecomp             translate PowerPC -> C++ (needs no runtime)
    v
build/ppc/*.cpp               29,731 functions, 3.1M lines
    |
    |  gen_import_stubs.py     generate the kernel import table from the
    |                         recompiler's own symbol resolution
    v
build/runtime/*.cpp
    |
    |  clang-cl + MSVC
    v
build/host/COD3.exe           native x64 binary
```

Each recompiled PowerPC function becomes
`void fn(PPCContext& ctx, uint8_t* base)`, where `base` points at the loaded
guest image. The runtime owns that image, resolves the 179 imported kernel
APIs, and calls the entry point.

Two design decisions worth calling out:

**The runtime owns the XEX loading, via the recompiler's own parser.**
`cod3_load_image()` calls `Xex2LoadImage()` from XenonUtils, the same code
XenonRecomp used. That guarantees the bytes being executed are provably the
bytes that were translated — no risk of the runtime and the translator
disagreeing about decryption or import thunk patching.

**The import stub table is generated, not hand-listed.** The set of required
symbols is derived from `ppc_recomp_shared.h`, the recompiler's own output, so
the two can never drift apart.

---

## Ground rules

This is a clean, standalone project.

* No code, assets, configuration or patterns were taken from any prior
  recompilation project. The runtime, the analysis tooling and the graphics plan
  were written for this repository.
* XenonRecomp is used strictly as a tool — the upstream CLI, built from its own
  source. Its `XenonUtils`/`ppc_context.h` supply the translation contract, not
  a game runtime; supplying the runtime is this project's job.
* Only `hedge-dev/XenonRecomp` is used. Other recompilation projects surfaced
  by the same search were deliberately not consulted.
* The source dump is treated as read-only.

---

## Documentation

| Document | Contents |
|---|---|
| **[docs/ANALYSIS.md](docs/ANALYSIS.md)** | Dump layout, XEX/PE structure, every measured address, imports, translator gaps |
| **[docs/SETUP.md](docs/SETUP.md)** | Prerequisites, build steps, running, repo layout |
| **[docs/GRAPHICS.md](docs/GRAPHICS.md)** | Why there is no D3D boundary, the three hook levels, option-by-option plan |
| **[docs/STATUS.md](docs/STATUS.md)** | What works, what does not, and the ordered path to a playable build |
| docs/AGENT_INSTRUCTIONS.md | Original task brief |

Private project. Do not publish.
