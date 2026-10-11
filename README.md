
---
![COD3 Recomp Logo](docs/logo.png)

<div align="center">
  <img src="docs/status.svg?v=2" width="900">
</div>

# COD3 Recomp

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-Windows-lightgrey)]()
[![Status](https://img.shields.io/badge/status-active-brightgreen)]()

Static recompilation of **Call of Duty 3** (Xbox 360) to a native Windows PC
binary.

The Xbox 360 executable is translated ahead-of-time from PowerPC to x86-64 with
[XenonRecomp](https://github.com/hedge-dev/XenonRecomp), and this project
supplies the runtime that the translated code links against: guest memory, the
PowerPC dispatch table, and an implementation of the Xbox 360 kernel API
surface the title imports.

There is no emulation layer and no dynamic translation at run time - once
compiled, the game code is ordinary x86-64 that the OS runs natively.

**Source dump** (read-only): your own copy of `default.xex`

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
| Guest memory model (image at real VA, byte order, indirect call table) | Done |
| Xbox 360 kernel API layer (179 imports; 37 hand-written, 142 stubs) | Partial - see STATUS |
| **Renders the game** | **Not yet** - title does not fully boot; graphics interception not started |

The port loads the real recompiled executable and transfers control to its
original entry point at `0x82344D00`, then runs the CRT through heap setup, XEX
header queries, virtual memory and TLS. It stops in CRT initialisation.

It does not yet draw, for two independent reasons: the title does not fully boot,
and Call of Duty 3 imports **no D3D functions** because the Xbox 360 graphics
stack (NGL) is statically linked into the executable. There is no *import*
boundary, but there is a *vtable* boundary: 211 COM-style method tables were
measured in `.rdata`. The plan is to intercept NGL's flush points and drive a real
`IDirect3DDevice9` rather than emulating the Xenos command processor.
See **[docs/GRAPHICS.md](docs/GRAPHICS.md)**.

---

## Quick start

You need your own copy of Call of Duty 3 for Xbox 360. It is not included here
and cannot be - this project recompiles the original executable, so it needs the
original `default.xex` from a dump.

```powershell
# 1. Toolchain: clone + build XenonRecomp (~2 min)
powershell -ExecutionPolicy Bypass -File tools\setup.ps1

# 2. Full pipeline: analyse, recompile, compile the port
powershell -ExecutionPolicy Bypass -File tools\build.ps1 -Xex "C:\path\to\Call of Duty 3\default.xex"

# 3. Run
build\host\COD3.exe
```

Step 3 needs no arguments. The executable looks for the game next to itself and
in the folder you used previously, and if it finds neither it asks for it - a
folder picker when you launched it from a console, a clear message otherwise -
then remembers the answer. `--xex`, `--game-dir` and `--multiplayer` override the
search if you want to be explicit.

Prerequisites: VS 2022 Build Tools, LLVM 18+ (20.x used here), Python 3.11.
Full details in **[docs/SETUP.md](docs/SETUP.md)**.

Incremental rebuilds, if you are only changing the runtime:

```
cmd /c tools\build_cod3.bat
```

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
bytes that were translated - no risk of the runtime and the translator
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
* XenonRecomp is used strictly as a tool - the upstream CLI, built from its own
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

---

## Legal Disclaimer

This repository contains **only** the recompilation harness, runtime glue, and supporting tooling. It does **not** contain any game code, assets, textures, models, sound, or other copyrighted material from Call of Duty 3 or any other title.

You must legally obtain your own dump of the original game. This project is not affiliated with, endorsed by, or sponsored by Activision, Treyarch, or any rights holder. All trademarks and copyrights remain the property of their respective owners.

Use of this software is at your own risk. The authors provide no warranty and accept no liability for any damages, legal consequences, or other issues arising from its use.

---

## License

The recompilation harness and local modifications in this repository are licensed under the [MIT License](LICENSE).

Game code and assets are **not** included and are not licensed by this project.

See also: [Code of Conduct](CODE_OF_CONDUCT.md) · [Security Policy](SECURITY.md) · [Contributing](CONTRIBUTING.md)
