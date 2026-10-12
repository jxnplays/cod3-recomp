<div align="center">
  <img src="docs/logo.png" alt="COD3 Recomp" width="620">
</div>

<div align="center">

![ANALYZED](https://img.shields.io/badge/ANALYZED-ffd60a?style=flat-square) ![XENON](https://img.shields.io/badge/XENON-4a4a3a?style=flat-square) ![19K+FUNCS](https://img.shields.io/badge/19.4K+FUNCS-8a7a2a?style=flat-square) ![BINARY](https://img.shields.io/badge/BINARY-ffd60a?style=flat-square) ![LOADS](https://img.shields.io/badge/LOADS-ffd60a?style=flat-square) ![KERNEL](https://img.shields.io/badge/KERNEL-4a4a3a?style=flat-square) ![RENDERS](https://img.shields.io/badge/RENDERS-5fd35f?style=flat-square) ![RENDERED](https://img.shields.io/badge/RENDERED-5fd35f?style=flat-square)

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![platform: Windows](https://img.shields.io/badge/platform-Windows-0078D4?logo=windows&logoColor=white)](https://learn.microsoft.com/en-us/windows/)
[![status: active](https://img.shields.io/badge/status-active-brightgreen.svg)](#)

</div>

# COD3 Recomp

**Call of Duty 3** (Xbox 360, 2007) running as a **native Windows PC binary**.

The Xbox 360 executable is translated ahead-of-time from PowerPC to x86-64 with
[XenonRecomp](https://github.com/hedge-dev/XenonRecomp), and this project supplies
the runtime the translated code links against: guest memory, the PowerPC dispatch
table, the Xbox 360 kernel API surface, the Xenos GPU command processor and a
Direct3D 11 renderer that draws the title's own shaders.

There is no emulation layer and no dynamic translation at run time — once
compiled, the game code is ordinary x86-64 that the OS runs natively.

**Source dump** (read-only): your own copy of `default.xex`.

---

## Provenance — please read

This repository is a **continuation** of the work in
[`FrankHUN88453/CoD3Recomp`](https://github.com/FrankHUN88453/CoD3Recomp), which
reached a working, rendering port of Call of Duty 3 through static recompilation.
That project is the foundation here, and its runtime is what this one builds on.

The runtime — the kernel, the GPU command processor, the D3D11 renderer, the
Xenos shader translator, the audio and input paths — is his work, under the
**BSD 3-Clause** licence. That notice is retained in [LICENSE](LICENSE); it is a
condition of the licence and this repository keeps it.

What this project adds is the direction, the roadmap and the work on top. We are
not presenting his port as ours.

See [docs/PROVENANCE.md](docs/PROVENANCE.md) for the full picture, including
what this repository inherited and what it changed.

---

## What works

Verified on the v0.4.0 release running against a full game dump:

| | |
|---|---|
| **Boot** | Loads `default.xex`, maps 15 sections, enters guest code |
| **Kernel** | All 179 Xbox 360 imports implemented |
| **Recompilation** | 19,462 functions translated, every instruction |
| **GPU** | Xenos PM4 command processor decodes the register writes |
| **Rendering** | Direct3D 11, the title's own shaders via HLSL |
| **Assets** | Streams level archives from the game directory |
| **Audio** | XMA decoded through FFmpeg |
| **Input** | Keyboard, mouse and pad |
| **Settings** | Resolution, filtering, anisotropy, AA, FOV, FPS cap |

## Known gaps

Stated plainly, because a port that hides its holes is harder to help with:

- Depth, blend and stencil coverage is narrower than the hardware's
- Several texture and vertex formats are counted and named at run time, not
  implemented
- Texture sampling is nearest; edges are harder than they should be
- `RtlRaiseException` / `RtlUnwind` report and stop — anything that throws is
  unrecoverable
- The recompiled output has not been verified instruction-by-instruction against
  the original

[STATUS.md](STATUS.md) carries the detail and the measurements behind all of it.

---

## Building

Requires **CMake 3.20+**, **Ninja**, **Clang** (not optional — the generated code
depends on Clang intrinsics and the `alias` attribute used for function hooking),
and the Visual Studio Build Tools for the Windows SDK headers.

```powershell
# Recompile the game code from your own default.xex (hours - ~19.5k functions)
.\scripts\recompile.ps1 -BuildTools -BuildLib -BuildHost

# Every level (15 more DLLs, also hours)
.\scripts\recompile_level.ps1 -All
```

Put your `default.xex` in `CoD3RecompLib/private/` first.

The XenonRecomp submodule must be patched before the tools are built:

```powershell
git submodule update --init tools/XenonRecomp
git -C tools/XenonRecomp apply ..\patches\XenonRecomp.patch
```

`recompile.ps1` applies the project's coroutine patch afterwards; without it the
first level's scripts spin forever.

## Running

`CoD3.exe` looks for a `game` folder beside itself and offers to install from a
disc image (XGD1/XGD2/XGD3 all recognised), from a folder you already extracted,
or in place from a folder elsewhere.

```
CoD3.exe --iso  "D:\Call of Duty 3.iso"
CoD3.exe --game "D:\Call of Duty 3 (USA, Europe)"
CoD3.exe --help
```

| Key | |
|---|---|
| `F11` | settings menu |
| `F12` | screenshot, plus the next two frames' draws to `CoD3.log` |
| `` ` `` | the title's own console (`spmap forest`, `god`, `noclip`) |
| `Escape` | pause |

---

## Roadmap

This project exists to keep Call of Duty 3 playable on PC, and then to take it
further than the console version ever was.

**Now** — establish the working build as our own, then rebuild the gap list from
measurements rather than inherited notes.

**Then** — the console version, accurately: complete depth, blend, filtering and
the missing texture and vertex formats, so what runs is what shipped.

**Then** — *Call of Duty 3 Remastered*: modern settings the console never had,
higher resolutions and wider aspect ratios, uncapped frame rate, and new
features the design invites — custom servers, larger multiplayer matches, and
beyond.

## Layout

| Path | |
|---|---|
| `CoD3Host/` | the executable: kernel, renderer, GPU, audio, input |
| `CoD3RecompLib/config/` | recompiler configuration, every address documented |
| `scripts/` | the pipeline, analysis tools and frame comparison |
| `tools/` | XenonRecomp (patched submodule), XEX scanner, PowerPC assembler |
| `docs/` | renderer design, kernel import list, provenance, findings |

## Legal

This repository contains **no Activision or Treyarch assets**, no game code, no
game data and no game audio. It is build configuration, tooling and a host
runtime that target a separately supplied, legally owned copy of the game.

You provide your own disc or dump. Please do not ask where to obtain one.

## Licence

Our own work is MIT. The inherited runtime is BSD 3-Clause and retains its
notice. See [LICENSE](LICENSE).
