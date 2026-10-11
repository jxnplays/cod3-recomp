# Status

Where this project actually is, stated precisely. Nothing below is aspirational;
every claim is reproducible with the commands in [SETUP.md](SETUP.md).

**Summary:** the static recompilation is complete and verified end to end. The
binary launches, loads the real Xbox 360 executable, and executes recompiled
PowerPC code. **It does not display the game.** The blocker is specific, is
understood, and is documented below ÃƒÂ¢-â€šÂ¬-â‚¬Â it is not a toolchain problem.

---

## What works

| Component | Status | Evidence |
|---|---|---|
| Dump analysis | **Done** | XEX/PE parsed, 179 imports resolved, 8 register helpers located ([ANALYSIS.md](ANALYSIS.md)) |
| XenonRecomp toolchain built from source | **Done** | `XenonRecomp.exe`, `XenonAnalyse.exe` built with clang-cl 20.1.8 |
| Jump table detection | **Done** | 323 tables detected |
| XEX ÃƒÂ¢-â‚¬Â -â‚¬â„¢ C++ translation | **Done** | 29,731 functions, 120 files, 98.4 MB, 3,109,564 lines, exit 0 |
| Compiles to native x64 | **Done** | `build/host/COD3.exe`, 24.2 MB, links clean |
| Guest memory model | **Done** | Image mapped at its real VA, indirect call table built, byte order correct |
| Runs the title's boot path | **Done** | Reaches CRT main init (`sub_82349A98`) and its callees |
| Virtual memory manager | **Done** | `NtAllocateVirtualMemory` / query / free |
| XEX header + loader globals | **Done** | `RtlImageXexHeaderField`, loader context chain published |
| Thread local storage | **Done** | `KeTlsAlloc`/`Get`/`Set`/`Free` with per-thread slots |
| Xbox 360 kernel API layer | **Partial** | 37 hand-written, 142 generated stubs, 179 total |
| Crash + call-chain attribution | **Done** | Faults map to guest addresses, and the host stack maps to a guest call chain |
| CRT vtable dispatch | **In progress** | Current frontier ÃƒÂ¢-â€šÂ¬-â‚¬Â see below |
| **Renders the game** | **Not working** | See below |

## Boot path progress

Getting the title to run at all required five non-obvious fixes. Each was found
from evidence rather than guesswork, and each is a prerequisite for the next.

1. **Guest memory model.** The recompiled code forms every reference as
   `base + guest_va`, where `guest_va` is a full 32-bit Xbox 360 virtual address
   (PowerPC registers hold real VAs). Mapping the image at an arbitrary address
   makes every access compute `base + 2 GB` and land outside the mapping. The
   image is now mapped at `0x82000000` so `base == 0`.

2. **Image sizing.** The allocation must cover the PE `SizeOfImage`
   (`0xC98000`), not the XEX `imageSize` (`0xC30000`); `.reloc` ends at
   `0xC4D208` and the title touches memory past the XEX figure.

3. **Guest byte order.** The guest is big-endian and XenonRecomp's generated code
   byte-swaps every access. The runtime is not generated code, so it must do the
   same. This was silently wrong everywhere: `0x83CA0000` came back as
   `0x0000CA83`, which presented as a wild pointer far from the real cause.

4. **The indirect call table.** PowerPC has no direct calls. XenonRecomp resolves
   every indirect call through a perfect-hash table:

   ```
   *(PPCFunc**)(base + PPC_IMAGE_BASE + PPC_IMAGE_SIZE
                + (uint32_t(guest) - PPC_CODE_BASE) * 2)
   ```

   With `base == 0` that table occupies `0x82C30000`ÃƒÂ¢-â€šÂ¬-â‚¬Å“`0x83626370`, about 10 MiB
   *outside* the image. Without it, every indirect call in the game faults. This
   is not a detail; it is on the hottest path in the entire port.

5. **Argument decoding.** `NtAllocateVirtualMemory`'s `r3` is a pointer to the
   caller's `BaseAddress` slot, not a pointer to a pointer. The extra
   dereference followed an unset slot into address 0.

With those in place the port progresses through the CRT: loader globals ÃƒÂ¢-â‚¬Â -â‚¬â„¢
`RtlImageXexHeaderField` ÃƒÂ¢-â‚¬Â -â‚¬â„¢ heap setup ÃƒÂ¢-â‚¬Â -â‚¬â„¢ `NtAllocateVirtualMemory` (twice,
distinct regions) ÃƒÂ¢-â‚¬Â -â‚¬â„¢ `KeTlsAlloc` ÃƒÂ¢-â‚¬Â -â‚¬â„¢ CRT main init.

### Verified run output
### Verified run output

```
[cod3] PE SizeOfImage 0xC98000 exceeds XEX imageSize 0xC30000; extending the mapping
[cod3] indirect call table at 0x82CA0000, 0x9F6378 bytes, 29728 functions mapped
[cod3] runtime services at 0x82A96200 (table slots 1305664..1305665)
[cod3] NULL dispatch trap mapped at 0x17EB60000, 0x100000 bytes
[cod3] captured 14 XEX optional headers for the title to query
[cod3] loader globals published: [0x82001288] -> 0x84696378 -> 0x84697378

[cod3] entering recompiled entry point 0x82344D00
[cod3] RtlImageXexHeaderField(0x00020401) -> 0x85696378
[cod3] NtAllocateVirtualMemory: flags=0x60002000 size=0x1000 -> 0x83CA0000
[cod3] NtAllocateVirtualMemory: flags=0x60001000 size=0x1000 -> 0x83CA1000
[cod3] KeTlsAlloc -> slot 0
```

Guest call chain at the point of failure, recovered by the crash handler:

```
#6  sub_821C7400    <- faulting function
#7  sub_82349A98    <- CRT main init
#8  sub_82344D00    <- XEX entry point (_xstart)
```

---


Call of Duty 3 imports **zero Direct3D functions**. The Xbox 360's graphics
stack (NGL, the Xenos D3D9 derivative) is statically linked into the 5 MB
`.text`, and the title resolves its own exports at runtime through
`XexGetProcedureAddress`.

So there is no D3D9/D3D11 call to intercept, no wrapper to write, and no
"modern renderer" to substitute. The Xbox 360 GPU is not a display controller ÃƒÂ¢-â€šÂ¬-â‚¬Â
it is a general-purpose vector processor driven by a command packet stream over
a register bus, and that bus is currently discarded.

`MmMapIoSpace` is implemented and is the natural hook, but it currently returns
a zeroed scratch buffer and prints a warning saying exactly that. Every register
write the game makes is dropped on the floor.

**This is the single reason the game does not appear.** Detail and the
level-by-level plan: [GRAPHICS.md](GRAPHICS.md).

### Blocker 2 - the boot path walks an uninitialised heap descriptor (current frontier)

Boot reaches thread creation and stops in the heap allocator. Measured by entry
override:

```
sub_82345E48 #1 -> 0x9C5B4000   handle r4=0x9C5AD000
  [h+56]   = 0x846A8590    (a valid first block)
  [h+1408] = 0x00000000
  [h+1412] = 0x00000000    <- the grow method

first VM page 0x846A8000:
  [h+56]   = 0x00000610
  [h+1408] = 0x846A8610    <- POPULATED, pointing into its own page
  [h+1412] = 0x00000000
```

`0x846A8610 - 0x846A8000 = 0x610`, which is also `[page+56]`: the descriptor stores
the offset of its own sub-structure, so `0x846A8000` is a correctly initialised
heap descriptor.

**The chain.** `sub_82345E48` tests `[heap+1412]`. Zero means "no grow method", so
it falls back to `NtAllocateVirtualMemory` and returns the raw page as though it
were an initialised block header. `sub_823460D8` then reads that block's `+12`
field — zero — and faults at `0x0`.

**Two descriptors exist.** `0x846A8000` is correctly initialised. `0x9C5AD000` —
the one the boot path walks — has both fields zero. Either something passes the
second where the first is wanted, or the second is expected to be initialised by a
path that never runs. Both readings fit every measurement so far; neither is
established.

#### Ruled out by measurement

- Not a missing free-list seed — `sub_82357BF0` runs once, `sub_8235B570` 14 times.
- Not the VM stub — `NtAllocateVirtualMemory` succeeds, addresses are consistent.
- Not `MmAllocatePhysicalMemoryEx` — implemented, requests succeed.
- Not `r13` — the CRT thread block is built and intact.

#### The correction that cost the most

`sub_82347340` was identified as the heap-setup routine and chased on that basis
for a while. **It is not.** The boot path calls it as:

```asm
li  r3,2        ; pool type
li  r4,0        ; flags
li  r6,4096     ; size
li  r7,0
bl  sub_82347340
```

That is `ExAllocatePool`-shaped. It returns an allocated address. An earlier trace
appeared to show it installing methods because `r3` was read *after* the call, so
the return value was mistaken for the descriptor that had been passed in; reading
the argument before the call gives `0x00000002`.

Every search that followed it was therefore searching the wrong function, and the
"sole writer of `+1408`/`+1412`" claim was wrong.

**Method note.** Three separate diagnostics in this investigation caused faults of
their own — reading a field at the wrong base, reading through a handle validated
against the wrong region, and reading a computed guess that landed on the loader's
own structures instead of the heap. Each produced a fault address that looked
exactly like the bug under investigation. All guest-field reads are now guarded,
and `COD3_TRACE_*` output names the region a handle is expected to lie in.

### Previously: CRT thread-local pointer unset

Boot no longer ends in stack exhaustion. `KeTlsSetValue` was returning 0
unconditionally, and `sub_82352998` gates all CRT heap initialisation on it:

```asm
0x823529F8  bl    sub_82352998 -> KeTlsSetValue
0x823529FC  cmpwi r3,0
0x82352A00  beq   0x82352abc      ; bail out when r3 == 0
0x82352A04  bl    sub_82352628
0x82352A08  bl    sub_82357BF0    ; seeds the 36 free-list slots
```

With the bail-out always taken, `sub_82357BF0` never ran, every size class in
the CRT free list stayed empty, and `sub_82357E48` recursed forever against an
empty list — ~6,420 levels deep, exhausting the host stack. Returning the stored
value from `KeTlsSetValue` breaks that; the guest uses the result as a
yes/no continue flag, never as a status code.

Boot now runs past heap init and stops at:

```
[cod3]   guest addr  : 0x823438C8  (sub_823438C8, +8 bytes)
[cod3]   fault addr  : 0x100
```

```asm
sub_823438C8:
  lwz  r11,256(r13)
  lwz  r3,332(r11)     ; faults: r11 == 0
```

`[r13+256]` reads as zero, so the second load dereferences `0x100`. **r13 is the
CRT per-thread pointer and it is unset.** The next step is to find what is
supposed to set it — the thread-start path, not this function.

### Previously: CRT vtable dispatch

The boot path now reaches the CRT's main init routine (`sub_82349A98`) and stops
in a dispatch loop that walks a loader-populated structure:

```asm
; sub_82349CC0 region
lwz   r11,0(r31)      ; r11 = [r31]      a function pointer in a descriptor
cmplwi r11,0
beq   skip            ; 0 means "not set"
cmpwi r11,-1
beq   skip            ; -1 means "not set"
mtctr r11
bctrl                 ; <-- dispatch
```

Exact call sites, recovered from the runtime's indirect-call diagnostics:
`0x82349B50`, `0x82349B04`, `0x82349AC8`, `0x82349BC8`.

**What populates it.** The Xbox 360 XEX loader publishes a block of globals in
the title's `.rdata` before the entry point runs. On hardware the CRT never
touches an unset slot because the loader fills it. In the raw XEX that block is
all zeros, and this port has to reconstruct enough of it for the CRT to proceed.

The chain has been recovered from the guest's own code
(`sub_82349CC0`, `build/ppc/ppc_recomp.55.cpp`):

```
[0x82001288]  ->  A          (loader context slot, published by cod3_populate_loader_globals)
[A + 0x00]    ->  B
[B + 0x58]    ->  image handle
RtlImageXexHeaderField(handle, XEX_HEADER_DEFAULT_HEAP_SIZE) -> pointer; [pointer] must be 0
```

That part works. What does not yet work is the wider descriptor graph the CRT
walks afterwards: it builds a 48-byte vtable (`[0] = 48`, the rest zero) and
dispatches through its entries, which the loader would have populated.

**Diagnostics that make this tractable.** Three tools were added to avoid guessing:

* The unresolved-indirect-call trap reports `ctx.lr`, the guest return address,
  which names the exact call site. Looking that up in `build/ppc` shows the load
  that produced the bad pointer.
* The crash handler walks the **host** call stack and maps each frame back to a
  guest function, producing a full guest call chain at the point of failure.
* A scan of the generated code finds every address the guest reads relative to a
  kernel base, yielding the 1,115 global addresses the CRT consults.

A NULL dispatch is handled specially: it does not land in the table at all, but
resolves to `0x17EB60000`, so that region is mapped and filled with the same
trap. Dispatching through an unset vtable entry therefore reports itself instead
of faulting opaquely.

### Known platform constraint

Mapping a low "null page" so that small bogus pointers land somewhere harmless
**cannot work on 64-bit Windows**: the first 64 KB is permanently reserved and
any mapping below `0x10000` is rejected with `ERROR_INVALID_PARAMETER` (87).
Guest addresses below that boundary can never be made valid. This is a property
of the host OS, not a runtime choice, and it is why the CRT's unset descriptor
must be fixed at the source rather than masked in memory.

### Blocker 3 - 14 unimplemented VMX instructions

The translator does not implement 33 distinct VMX/scalar opcodes. Because
XenonRecomp emits each unimplemented opcode once as a helper, this collapses to
**14 `__builtin_debugtrap()` sites** in the output ÃƒÂ¢-â€šÂ¬-â‚¬Â a tractable, well-defined
task, but each one traps if reached. Full list in
[ANALYSIS.md Ãƒâ€šÃ‚Â§8](ANALYSIS.md).

The heavy hitters are `vslh` (874 hits), `vsrah` (541), `vsubshs` (522),
`vspltish` (330), `vmaxsh` (231) and `vandc` (148).

### Blocker 4 - ~700 jump table mis-detections

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
* `ObReferenceObjectByHandle` succeeds and returns the handle back unchanged ÃƒÂ¢-â€šÂ¬-â‚¬Â
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

1. **Break the `sub_82357E48` / `sub_82357D08` recursion.** This is where boot
   ends now: a stack overflow, exhaustively. The pair alternates down the guest
   stack, confirmed by the call stack repeating those two frames to the bottom.

   What is known:

   - `sub_823478F0`, the page heap allocator, is entered exactly **three** times
     (sizes 0x80, 0x580, 0x800, descriptor 0x84698000) and is not part of the
     loop. `COD3_TRACE_HEAP_ALLOC=1` reports it.
   - The cycle does not pass through the list-seeding code. Hooks at
     `0x82357D44`, `0x82357D98`, `0x8235B570` and `0x82357DD8` all stay silent
     while the control hook at `0x823478F0` fires, so their silence is a real
     absence and not a broken hook.
   - The CRT's thread-local slot is now reserved at load
     (`cod3_reserve_tls_slot`), which removed the `0x48` fault. Before that,
     `0x825C6E38` read `0xFFFFFFFF`, so `KeTlsGetValue` always failed and the
     CRT never had a per-thread error record.

   The next step is to establish what `sub_82357D08` does on the path it is
   actually taking, since the paths already examined are not being executed.

2. **The remaining 184 unresolved branches.** Down from 9,270. The single
   largest cause was the `savefpr_14`/`restfpr_14` helper addresses in
   `config/recomp/COD3.toml`, which were wrong: `0x82350A60`, declared as
   `savegprlr_14`, decodes as `addi r11,r0,4`. The real helpers are

   ```
   restfpr_14 = 0x8234F260   lfd  f14,-152(r1) .. lfd  f31,-16(r1), blr
   savefpr_14 = 0x8234F2B0   stfd f14,-152(r1) .. stfd f31,-16(r1), blr
   ```

   and fixing those two lines took the count from 9,270 to 184. Measured
   conventions for this title: the link register is **r12** (`mflr r12` occurs
   10,518 times, `mflr r0` twice), and there is **no `lvx`/`stvx`** at all - the
   vector helpers are spelled `lfdup`/`stfdup`, so the VMX entries are unset.

3. **Finish the loader context graph.** The CRT walks a descriptor chain the
   Xbox 360 XEX loader populates before the entry point. The first links are
   published; the 48-byte vtable and its dispatch entries are not. The
   diagnostics above identify the exact call sites, and `cod3_populate_loader_globals`
   is the single place to extend. *Smallest step with the largest gain.*
4. **Implement the 14 VMX helpers.** Each is a single PowerPC instruction with a
   known semantic; they can be written directly against the reference
   instruction descriptions and validated in isolation.
5. **Work the import trace.** Each time the port gets further, the trace shows
   the next unimplemented API. Pool allocation, IRQL, sync objects, threads, TLS,
   virtual memory and XEX headers are done; XAM licensing, storage, input and
   networking are not.
6. **Intercept NGL and drive real D3D9.** This replaces an earlier assumption that
   the Xenos command processor had to be emulated. It does not: the title calls
   into the graphics stack through COM-style method tables (211 vtable-like tables
   were measured in `.rdata`), above the point where anything becomes Xenos-
   specific. Intercepting the flush points - `Draw*`, `Clear`, `Resolve`, present -
   and translating the guest's state onto a real `IDirect3DDevice9` avoids the
   command processor entirely. See [GRAPHICS.md](GRAPHICS.md).
7. **Solve shader translation.** Xenos bytecode to DXBC. This is the dominant
   remaining graphics cost, but it is a bounded compiler problem rather than a
   hardware emulator.
8. **Fix the jump table detections** once the boot path is far enough in to hit
   the affected regions.

Items 2-5 are ordinary runtime engineering. Items 6-7 are substantial but
bounded, and notably not a GPU emulator.

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