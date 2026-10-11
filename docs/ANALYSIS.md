# Source dump analysis

Everything below was derived directly from the dump at
`X:\AI\Recomp Projects\Call of Duty 3` using the tooling in `tools/`. No
conclusions here are taken from any other recompilation project.

## 1. Dump layout

A complete extracted Xbox 360 title, 553 files, 5.77 GB.

| Path | Files | Size | Notes |
|---|---|---|---|
| `default.xex` | 1 | 6.9 MB | Main game executable (single-player + core engine) |
| `codmp_xenonf.xex` | 1 | 6.8 MB | Multiplayer executable |
| `sp/` | 405 | 3.67 GB | Single-player level data |
| `mp/` | 91 | 1.47 GB | Multiplayer maps |
| `movies/` | 40 | 627 MB | Cinematics |
| `config/`, `media/`, `$SystemUpdate/` | 15 | 3.0 MB | System/loader data |

**Scope decision.** `default.xex` is the target. It contains the engine, the
renderer, and all single-player content, and it is the module the Xbox 360
launcher starts. `codmp_xenonf.xex` is a separate module that the title loads on
demand for multiplayer; it is a later milestone, not a prerequisite.

## 2. XEX container

Parsed with `tools/xex_info.py` (mirrors `XenonUtils/xex.h` structures).

```
magic            XEX2
module flags     0x00000001
header size      0x3000   (14 optional headers)
security offset  0x88
image size       0xC30000 (12,779,520 bytes)
encryption       NORMAL (AES-128-CBC)
compression      BASIC
image base       0x82000000
entry point      0x82344D00
TLS info         0x1558
default stack    0x20000
media id         3c287f44c297336affbad6f900e85d02
```

The encryption scheme is the standard retail one: the per-title AES key in the
security block is itself wrapped with the well-known retail key. `BASIC`
compression is a trivial `dataSize`/`zeroSize` block interleave, so the image can
be recovered with a few hundred lines of Python — see `tools/xex_extract.py`.
This is why no LZX decompressor was needed for the analysis; only the image
payload past the headers is encrypted, and the headers stay in the clear.

Extracted image: **11,829,248 bytes of real payload** in a 0xC30000 byte image
(the tail is zero fill that BASIC compression elides).

## 3. PE layout

The XEX wraps a PE32 image whose base matches the XEX base, so
`RVA == VA - 0x82000000` with no fixups to reason about.

| Section | RVA | Virtual size | Raw size | Flags |
|---|---|---|---|---|
| `.rdata` | 0x00001000 | 0x00080BC0 | 0x00080C00 | read-only data |
| `.pdata` | 0x00082000 | 0x00015480 | 0x00015600 | read-only data |
| `.text` | 0x000A0000 | 0x004D8E8C | 0x004D9000 | **code** |
| `.embsec_*` (x7) | 0x00579000+ | ~0x1000 each | ~0x1000 each | code |
| `.data` | 0x005A0000 | 0x0064AAFC | 0x00102800 | read/write |
| `.tls` | 0x00BEB000 | 0x1F | 0x200 | read/write |
| `.XEXID` | 0x00BEC000 | 0x4 | 0x200 | |
| `.idata` | 0x00BF0000 | 0x36A | 0x400 | |
| `.XBLD` | 0x00C00000 | 0xB0 | 0x200 | |
| `.reloc` | 0x00C01000 | 0x0004C208 | 0x0004C400 | discardable |

* `.text` is **5.0 MB** of PowerPC code — the whole engine.
* `.pdata` is 87 KB of unwind records, which is what lets XenonAnalyse derive
  most function boundaries without guessing.
* `.embsec_*` are encrypted code regions. They are present but small; they did
  not appear in the recompiled output, so they appear to be inactive on this
  retail image.

## 4. Register save/restore helpers

XenonRecomp requires the start address of eight PowerPC non-volatile
save/restore helpers, because the Xbox 360 compiler inlines them mid-function.
`tools/find_symbols.py` locates each by its documented prologue byte signature.
Every one produced **exactly one hit**, so these are unambiguous.

| Helper | Address | Signature |
|---|---|---|
| `__restgprlr_14` | `0x82350AB0` | `ld r14, -0x98(r1)` |
| `__savegprlr_14` | `0x82350A60` | `std r14, -0x98(r1)` |
| `__restfpr_14` | `0x8235121C` | `lfd f14, -0x90(r12)` |
| `__savefpr_14` | `0x823511D0` | `stfd f14, -0x90(r12)` |
| `__restvmx_14` | `0x82351508` | `li r11,-0x120; lvx v14` |
| `__savevmx_14` | `0x82351270` | `li r11,-0x120; stvx v14` |
| `__restvmx_64` | `0x8235159C` | `li r11,-0x400; lvx128 v64` |
| `__savevmx_64` | `0x82351304` | `li r11,-0x400; stvx128 v64` |

All eight sit within ~3 KB of each other at the start of the runtime
support code, which is the expected layout.

## 5. Jump tables

`XenonAnalyse` detected **323** jump tables (111 KB of TOML). Many of these are
mis-detections — the analysis is pattern-based and the Xbox 360 compiler's jump
table shapes changed over the years. See [STATUS.md](STATUS.md) for the effect
this has on the generated code.

## 6. Imports

179 distinct kernel APIs are imported, resolved by XenonRecomp from the XEX
ordinal tables. Counted by prefix:

| Group | Count | Role |
|---|---|---|
| `Xam*` | 33 | Title services: licensing, storage, invites, networking |
| `Ke*` | 26 | Kernel executive: threads, events, IRQL, sync objects |
| `Rtl*` | 15 | Runtime: critical sections, string/memory helpers |
| `Mm*` | 6 | Physical memory descriptors and MMIO mapping |
| `XAudio*` | 5 | Audio |
| `Vd*` / `XGetVideoMode` | 8 | Display mode query and configuration |
| `Ob*` | 4 | Object manager |
| `Ex*` | 4 | Executive (pool, threads, termination callbacks) |
| `Dbg*`, `NetDll*`, misc | 78 | Debug output, Winsock shims, misc |

### The graphics finding that shapes this project

**There is not a single D3D import.** The title links no GPU API by name. What
it actually does is:

* The Xbox 360 graphics stack (**NGL** — the Xenos D3D9 derivative) is
  **statically linked into the 5 MB `.text`**, complete with its own debug
  strings:
  * `nglPresent`, `nglBackBuffer`
  * `"D3D: GPU initialization (HSIO training) has failed so no graphics will render."`
  * `"ERR[D3D]: Unanticipated CPU_INTERRUPT. Sign of a corrupt command buffer?"`
  * `"The GPU is hung! D3D version is %i.%i %s, kernel is %i, frame is %i."`
  * `"Microsoft (R) Xbox 360 Shader Compiler 2.0.x"`
* The title resolves its own exports at runtime through
  `XexGetProcedureAddress` / `XexGetModuleHandle`.
* The only display APIs that cross the kernel boundary are the eight `Vd*` /
  `XGetVideoMode` calls, which are mode query/set only.

This means resolution scaling and the other graphics options **cannot** be
implemented as a D3D9 wrapper — there is no D3D9 boundary to wrap. They have to
be applied inside the recompiled NGL layer. See [GRAPHICS.md](GRAPHICS.md) for
the concrete hook points this implies.

## 7. Audio

Five `XAudio*` imports (voice category volume and change mask). The XAudio2
command processor is likewise not imported, so audio is in the same position as
graphics: implemented inside the recompiled code.

## 8. What the translator could not handle

`XenonRecomp` translated `default.xex` to completion (29,731 functions), but
reported gaps. These are recorded rather than papered over:

**VMX/vector instructions the translator does not implement** — 33 distinct
mnemonics, dominated by:

| Mnemonic | Hits | Mnemonic | Hits |
|---|---|---|---|
| `vslh` | 874 | `vspltish` | 330 |
| `vsrah` | 541 | `vmaxsh` | 231 |
| `vsubshs` | 522 | `vandc` | 148 |
| `vminsh` | 88 | `vpkswss` | 76 |
| `vctuxs` | 64 | `vcmpgtsh` | 54 |
| `vavguh` | 16 | `vsel128` | 13 |
| `vrfip` | 11 | `vsububm` | 7 |

Plus scalar/branch cases: `bso`, `bns`, `bsolr`, `cror`, `crorc`, `mulhd`,
`mulhdu`, `vcfpuxws128`, `vpkuhus`, `vpkswus`, `vpkshss`, `vrlh`, `vsrh`,
`vsrab`, `vcmpequh`, `vcmpgtsh.`, `vcmpgtsw.`, `vcmpequh.`, `vspltish`.

Crucially, XenonRecomp emits each unimplemented opcode **once** as a helper
function, so the 3,000-odd raw hits collapse to just **14
`__builtin_debugtrap()` sites** in the generated code. Hand-writing those 14
helpers is a tractable, well-defined task.

**Jump table mis-detections.** XenonRecomp emitted ~700 errors of the form:

```
ERROR: Switch case at 82140150 is trying to jump outside function: 82140654
```

These are cases where a detected "jump table" label resolves outside the
function that owns it. The affected code is emitted as a fallthrough/goto that
cannot execute correctly. The hot spots are at `0x82140150`, `0x821D4958`,
`0x821D447C`, `0x821FBE1C` and `0x82194FA8`.

Both categories are tracked in [STATUS.md](STATUS.md).