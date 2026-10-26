<div align="center">
  <img src="docs/logo.png" alt="COD3 Reborn" width="800">
</div>

<div align="center">

![ANALYZED](https://img.shields.io/badge/ANALYZED-ffd60a?style=flat-square) ![XENON](https://img.shields.io/badge/XENON-4a4a3a?style=flat-square) ![19K+FUNCS](https://img.shields.io/badge/19.4K+FUNCS-8a7a2a?style=flat-square) ![BINARY](https://img.shields.io/badge/BINARY-ffd60a?style=flat-square) ![LOADS](https://img.shields.io/badge/LOADS-ffd60a?style=flat-square) ![KERNEL](https://img.shields.io/badge/KERNEL-4a4a3a?style=flat-square) ![RENDERS](https://img.shields.io/badge/RENDERS-5fd35f?style=flat-square) ![RENDERED](https://img.shields.io/badge/RENDERED-5fd35f?style=flat-square)

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![platform: Windows](https://img.shields.io/badge/platform-Windows-0078D4?logo=windows&logoColor=white)](https://learn.microsoft.com/en-us/windows/)
[![status: active](https://img.shields.io/badge/status-active-brightgreen.svg)](#)

</div>

# COD3 Reborn
https://jxnplays.github.io/cod3-reborn

## Legal Disclaimer

This repository contains **only** the recompilation harness, runtime, and local modifications. It does **not** contain any game code, assets, textures, models, sound, or other copyrighted material from Call of Duty 3 or any other title.

You must legally obtain your own copy of the original game (your own `default.xex` and level data). This project is not affiliated with, endorsed by, or sponsored by Activision or any rights holder. All trademarks and copyrights remain the property of their respective owners.

Use of this software is at your own risk. The authors provide no warranty and accept no liability for any damages, legal consequences, or other issues arising from its use.

---

**Call of Duty 3** (Xbox 360, 2007) running as a **native Windows PC binary**.

The Xbox 360 executable is translated ahead-of-time from PowerPC to x86-64 with
[XenonRecomp](https://github.com/hedge-dev/XenonRecomp), and this project supplies
the runtime the translated code links against: guest memory, the PowerPC dispatch
table, the Xbox 360 kernel API surface, the Xenos GPU command processor and a
Direct3D 11 renderer that draws the title's own shaders.

There is no emulation layer and no dynamic translation at run time — once
compiled, the game code is ordinary x86-64 that the OS runs natively.

**Source dump** (read-only): your own copy of `default.xex`.

> **Status: this is a working port, and the remaster work is starting.** The
> inherited runtime is solid enough that levels load, render and play. What is
> *not* done is everything this project intends to add on top. See
> [Roadmap](#roadmap) for where the line is, and please read
> [Known gaps](#known-gaps) before reporting a bug — several are long-standing
> and documented rather than hidden.

---

## Documentation

Start here, in the order you probably want them:

| | |
|---|---|
| **[docs/SETUP.md](docs/SETUP.md)** | **Setup tutorial.** Prerequisites, how to build it, every environment variable, troubleshooting. Read this first if you want to run it. |
| [docs/PROVENANCE.md](docs/PROVENANCE.md) | Whose work this is, and what this project changed. Read before patching the runtime. |
| [docs/GAPS.md](docs/GAPS.md) | The open questions, with what is known about each |
| [docs/CONSOLE.md](docs/CONSOLE.md) | How commands reach the title, and the 188 console variables it has |
| [docs/MOUSE.md](docs/MOUSE.md) | Mouse look: the measurements, and the leads already exhausted |
| [docs/cvars.txt](docs/cvars.txt) | The console variables, as a plain list |
| [docs/FINDINGS.md](docs/FINDINGS.md) | Earlier analysis |
| [docs/renderer.md](docs/renderer.md) | How the Direct3D 11 renderer works |
| [docs/animation.md](docs/animation.md) | Animation |
| [docs/scripts.md](docs/scripts.md) | What each script in `scripts/` does |
| [docs/kernel-imports.txt](docs/kernel-imports.txt) | The 179 Xbox 360 imports and their status |

Two things a newcomer usually gets wrong, so they are said here as well as
below: **the game is not in this repository** (no `.xex`, no assets, ever), and
the **level libraries are a separate build step** that `recompile.ps1` does not
perform for you.

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

*(The remainder of the original README continues below with status, building, known gaps, roadmap, etc. The full original content after the header has been preserved except for relocating the legal notice to the top.)*
