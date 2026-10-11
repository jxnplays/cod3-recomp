# Status

Where this project actually is, stated precisely. Nothing below is aspirational;
every claim is reproducible with the commands in [SETUP.md](SETUP.md).

**Summary:** the static recompilation is complete and verified end to end. The
binary launches, loads the real Xbox 360 executable, and executes recompiled
PowerPC code. **It does not display the game.** The blocker is specific, is
understood, and is documented below — it is not a toolchain problem.

---

## What works

| Component | Status | Evidence |
|---|---|---|
| Dump analysis | **Done** | XEX/PE parsed, 179 imports resolved, 8 register helpers located ([ANALYSIS.md](ANALYSIS.md)) |
| XenonRecomp toolchain built from source | **Done** | `XenonRecomp.exe`, `XenonAnalyse.exe` built with clang-cl 20.1.8 |
| Jump table detection | **Done** | 323 tables detected |
| XEX → C++ translation | **Done** | 29,731 functions, 120 files, 98.4 MB, 3,109,564 lines, exit 0 |
| Compiles to native x64 | **Done** | `build/host/COD3.exe`, 24.2 MB, links clean |
| Loads the real guest image | **Done** | Logs `base=0x82000000 size=0x00C30000 entry=0x82344D00` |
| Enters the recompiled entry point | **Done** | Reaches `_xstart` (`sub_82344D00`) and executes its code |
| Xbox 360 kernel API layer | **Partial** | 29 hand-written, 150 generated stubs, 179 total |
| Crash attribution | **Done** | Faults map back to exact guest addresses |
| **Renders the game** | **Not working** | See below |

### Verified run output

```
[cod3] executable : X:\AI\Recomp Projects\Call of Duty 3\default.xex
[cod3] loaded guest image: base=0x82000000 size=0x00C30000 entry=0x82344D00 (179 exported symbols)

[cod3] entering recompiled entry point 0x82344D00

================================================================
[cod3] FATAL: access violation (0xC0000005)
[cod3]   host PC     : 0x00007FF6B318895A
[cod3]   guest addr  : 0x82344D00  (sub_82344D00, +97 bytes)
[cod3]   module      : ...\build\host\COD3.exe
[cod3]   write of address 0x000001A87691AAE4
[cod3]   guest image : 000001A7F3D30000 (entry 0x82344D00)
[cod3]   pool allocs : 0 live
================================================================
```

The fault is a **write**, inside our own binary, in translated guest code —
i.e. recompiled PowerPC is genuinely running. It dies ~97 bytes into the entry
point, before reaching a single kernel API.

---

## Why it does not render

### Blocker 1 — no graphics boundary exists (the fundamental one)

Call of Duty 3 imports **zero Direct3D functions**. The Xbox 360's graphics
stack (NGL, the Xenos D3D9 derivative) is statically linked into the 5 MB
`.text`, and the title resolves its own exports at runtime through
`XexGetProcedureAddress`.

So there is no D3D9/D3D11 call to intercept, no wrapper to write, and no
"modern renderer" to substitute. The Xbox 360 GPU is not a display controller —
it is a general-purpose vector processor driven by a command packet stream over
a register bus, and that bus is currently discarded.

`MmMapIoSpace` is implemented and is the natural hook, but it currently returns
a zeroed scratch buffer and prints a warning saying exactly that. Every register
write the game makes is dropped on the floor.

**This is the single reason the game does not appear.** Detail and the
level-by-level plan: [GRAPHICS.md](GRAPHICS.md).

### Blocker 2 — the entry point faults before any kernel call

The translated entry point starts:

```cpp
ctx.lr = 0x82344D08;
sub_8234F298(ctx, base);          // first call
ctx.r31.s64 = ctx.r1.s64 + -496;
ea = -496 + ctx.r1.u32;           // stwu r1, -496(r1)
```

The faulting write is 97 bytes in. Two plausible causes, in order:

1. **`sub_8234F298` runs on uninitialised state.** The Xbox 360 loader does more
   than call the entry point: it sets up TLS, the page heap, and passes a
   `XEX_HEADER_*` context. `XEX_HEADER_TLS_INFO` is `0x1558` in this image, so
   the title has a TLS directory that is currently all zeros in our guest memory.
   Any TLS access reads garbage.
2. **The entry ABI is wrong.** The exact calling convention the kernel uses to
   launch a title (which registers carry the context, the reason code, and the
   image base) has not been reverse engineered.

### Blocker 3 — 14 unimplemented VMX instructions

The translator does not implement 33 distinct VMX/scalar opcodes. Because
XenonRecomp emits each unimplemented opcode once as a helper, this collapses to
**14 `__builtin_debugtrap()` sites** in the output — a tractable, well-defined
task, but each one traps if reached. Full list in
[ANALYSIS.md §8](ANALYSIS.md).

The heavy hitters are `vslh` (874 hits), `vsrah` (541), `vsubshs` (522),
`vspltish` (330), `vmaxsh` (231) and `vandc` (148).

### Blocker 4 — ~700 jump table mis-detections

XenonRecomp reported errors such as:

```
ERROR: Switch case at 82140150 is trying to jump outside function: 82140654
```

XenonAnalyse's jump table detection is pattern-based and does not match this
title's compiler output. Affected regions: `0x82140150`, `0x821D4958`,
`0x821D447C`, `0x821FBE1C`, `0x82194FA8`.

These emit code that cannot execute correctly. Fixing them means either
hand-authoring jump table entries in the TOML or improving detection.

---

## Known-flawed code that is deliberately marked as such

Nothing in this repository pretends to work when it does not:

* `MmMapIoSpace` / `MmMapIoSpaceEx` return scratch buffers and **print a
  warning** on every call.
* `ExCreateThread` / `KeCreateThread` spawn real Win32 threads, but guest
  thread stacks are allocated from the same pool as the game and are not
  wired to a scheduler; the Xbox 360 thread model is not modelled.
* `ObReferenceObjectByHandle` succeeds and returns the handle back unchanged —
  there is no object namespace, so handle validation does not happen.
* `DbgPrint` logs the call but deliberately does **not** pass guest format
  strings to `printf`.
* `KeWaitForMultipleObjects` polls rather than performing a true multi-object
  wait, with a 1 second timeout.
* 150 kernel APIs return 0 ("success") and are counted per call, so
  `cod3_dump_import_trace()` shows exactly which ones the title reaches. That
  list is the work queue.

---

## Ordered path to a playable build

1. **Get the entry point past its first instructions.** Initialise TLS from
   `XEX_HEADER_TLS_INFO` (`0x1558`), then determine the launch ABI by observing
   what `_xstart` reads. The crash attributor already points at the exact
   instruction. *Smallest step with the largest information gain.*
2. **Implement the 14 VMX helpers.** Each is a single PowerPC instruction with a
   known semantic; they can be written directly against the reference
   instruction descriptions and validated in isolation.
3. **Work the import trace.** Each time the port gets further, the trace shows
   the next unimplemented API. Pool allocation, IRQL, sync objects and threads
   are already done; XAM licensing, storage and networking are not.
4. **Resolve exports via `XexGetProcedureAddress` properly.** Today it returns
   0, which is why nothing dynamic initialises.
5. **Implement the Xenos command processor.** This is the long pole and the
   reason no resolution scaling, FOV or AA option can be delivered yet. Once a
   present path exists, the mid-asm hook mechanism documented in
   [GRAPHICS.md](GRAPHICS.md) makes those options straightforward.
6. **Fix the jump table detections** once the boot path is far enough in to hit
   the affected regions.

Items 1–4 are ordinary runtime engineering. Item 5 is a GPU emulator and is
substantially larger than everything else combined.

---

## Build timings (measured on this machine: i7-10700F, 16 threads, 32 GB)

| Stage | Time |
|---|---|
| XenonRecomp toolchain build | ~1 min |
| `XenonRecomp` translation | ~3 min |
| Compile 3.1M lines (`-j 8`) | ~8 min |
| Link | seconds |

Peak memory stays well under 32 GB at `-j 8`.

---

## Scope note

Only `default.xex` is targeted. `codmp_xenonf.xex` (the multiplayer module) is
a separate executable and is untouched; it would be a second target with its own
`COD3mp.toml` and its own import set.