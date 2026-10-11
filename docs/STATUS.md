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

### Blocker 2 - CRT heap grow method installed; region headers still wrong (current frontier)

**The heap grow method is fixed.** This was loader work, established by search
rather than inference: no function in the recompiled guest writes the heap
descriptor's method fields.

| Function | Writes +1404..+1420? |
|---|---|
| `sub_82346D98` (creates descriptors) | no |
| `sub_823478F0` (heap core, builds blocks) | no — writes block `+12` |
| `sub_823478E8` (heap core sibling) | no |
| `sub_824C2640` | yes — but on *image* addresses, not heap descriptors |
| `sub_82347340`, `sub_823473D8`, `sub_82571678` | yes — none run during boot |

On the 360 the loader establishes these fields when it creates the initial heap.
The guest never does it because on hardware it never has to.

`+1412` now gets a trampoline rather than a self-pointer: `sub_82345E48` calls it
through `bctrl`, and a self-pointer would execute the descriptor. The trampoline
grows the heap by the ordinary carve **and writes the block header** the fallback
path omits. It is installed in `sub_82346D98` when the descriptor is created —
installing it lazily at the allocator was too late, because the consumers fault
before the allocator is entered.

#### What remains

The **region header** at `0x846A8000`, which is the first page the CRT allocator
hands out (it is exactly the end of the loader's 64 KiB private region). It
carries a block count of `0x63` at `+2`, so `sub_823460D8`'s backwards walk

```asm
lhz   r11,2(r30) ; rotlwi r11,r11,4 ; subf r31,r11,r30
```

lands 0x630 bytes **before** the region. `NtAllocateVirtualMemory` zeroes what it
returns, so the field is written by the guest — or the region was never set up
and the value came from somewhere else. Not established.

The consumer guards on `sub_823460D8`, `sub_823481D0` and `sub_823481D8` remain
for this, and work for it: allocations refused by the fault tolerance fell from
**5 to 2** once the grow method was fixed, so the allocator itself is now correct
and only the region walk is not.

They were removed once on the assumption that fixing the allocator fixed them
too. It does not — boot went straight back to `sub_823481D8 +1057` with them
gone.

#### Blocker 3 - the block walk runs; the fault moved upstream of it

**The block header endianness was wrong, in two places.** This is the most
concrete result so far and it is fixed.

`sub_823460D8` reads a block's size as:

```asm
lhz    r11,2(r30)      ; LOW half of the big-endian word at +0
rotlwi r11,r11,4       ; x16
subf   r31,r11,r30     ; r31 = r30 - size
```

The grow method wrote the 16-byte-unit count to the **high** half. The walk read
a size of zero, computed `r31 = r30 - 0`, and examined each block as its own
predecessor — not a subtle degradation, it walked the list in the wrong direction.

The fault-tolerance guard had the **identical** mistake (`>> 16`). That was
harmless while headers were uninitialised, because both halves read as garbage
either way. The moment the grow method began writing real headers, the guard
concluded every block had size zero and began **skipping blocks that were
perfectly walkable**. Boot got worse because of a fix meant to make it better.

Recorded because the generalisable part matters: *a guard that reads the same
field as the code it guards must agree with it on which half is which, and
agreeing is not automatic.* `note_skip` read the wrong half too, so the logged
counts described a different field than they claimed.

Measured after removing the probes:

```
walk #1: block=0x846A8760 units=0x0001 (16 B)    prev=0x846A8750
walk #2: block=0x846A8E40 units=0x0001 (16 B)    prev=0x846A8E30
walk #3: block=0x9C5B4000 units=0x0210 (8448 B)  prev=0x9C5B1F00
```

Every block reports a sane size and steps backwards by exactly that much.
**Skips under `COD3_TOLERATE_HEAP_FAULT` are zero** — the first time the heap
tolerance has had nothing to tolerate on this path.

##### The chain, established link by link

Every step below is measured, and the chain is closed:

1. **The heap is dispatched through a table at `0x8208C600`.** The heap
   functions are contiguous entries in it — including `sub_823460D8` (the walk)
   and `sub_823464E0` (the extend) as *adjacent* entries.

2. **`sub_82347340` and `sub_823473D8` install the descriptor's method slots.**
   They are the only writers that touch `+1408` and `+1412` together, and they do
   it by copying out of an owner structure:

   ```asm
   r10 = [r31+80]         ; the heap descriptor
   stw r21,1408(r10)
   r11 = [r31+164]        ; the grow method, loaded from a FIELD
   stw r11,1412(r10)
   ```

   So `+1412` is not a literal — it is a field the owner struct supplies.

3. **They have zero callers in the recompiled image.** A search for
   `__imp__sub_82347340(` and `__imp__sub_823473D8(` returns nothing. They are
   reached only through the vtable, where `sub_82347340` is entry `0x8208C748`.

4. **They never run during boot.** The entry trace shows `sub_823478F0` — a
   *consumer* of those slots — running six times, while neither installer appears
   at all.

5. **Therefore `+1412` is zero**, which is what `sub_82345E48` tests before
   falling back to raw allocation.

**That is the CRT heap fault, closed.** The loader's job on a 360 is to create
the kernel heap object and give the descriptor its vtable. The recompiled guest
never does that because on hardware it never has to — so it is loader work, and
loader work belongs in the runtime.

**A correction to the claim made in the previous commit.** That commit said
the method interface was "wider than four slots" and listed five offsets with
reader counts as though they were all methods. **That was wrong**, and checking it
took one disassembly.

`sub_82437108` reads `+1416`, `+1420` and `+1424` with **`lfs`**, not `lwz` —
floating-point loads. Those are float fields, not function pointers. And
`sub_823478F0`, the most frequent reader of `+1408`, uses it like this:

```asm
lwz r3,1408(r30)      ; loaded into r3, i.e. used as a POINTER ARGUMENT
```

So the actual layout at the tail of a heap descriptor is:

| offset | what it is | set by |
|---|---|---|
| `+1408` | a pointer, passed as `r3` to another call | `sub_82347340` from a register |
| `+1412` | the grow method — the only method | `sub_82347340` from `[r31+164]` |
| `+1416` | `float32` | — |
| `+1420` | `float32` | — |
| `+1424` | `float32` | — |

**`+1408` turned out to be a critical section**, found by tracing where the
value written into it comes from. `sub_82347340` stores `r21`, and the governing
assignment is eight instructions earlier:

```asm
mr  r3,r29
mr  r21,r29
bl  __imp__RtlInitializeCriticalSection      ; r3 = the critical section
stw r21,1408(r11)                           ; +1408 = that critical section
```

So `+1408` is a pointer to a lock guarding heap operations, and it is **null**
because `sub_82347340` never runs. A field loaded into `r3` and passed straight
through as an argument is exactly how a null-pointer fault at a small address
happens, and the observed fault address is small.

The runtime side of that link is sound: `RtlInitializeCriticalSection` is
hand-implemented against a real Win32 `CRITICAL_SECTION`, keyed by guest address.
The import is not the problem — the caller that would have invoked it is.

**And a correction.** The previous commit implied `+1408` was a second unfilled
method slot. It is not; it is a lock. So the shape of the fix is narrower than
that commit claimed: there is one method (`+1412`) and one lock (`+1408`), both
installed by the same never-running function.

#### The heap fault is fixed, and boot reached the GPU layer

`+1408` and `+1412` are both populated now, and the descriptor is chosen at the
point of use rather than at creation.

**What was wrong with the placement.** The methods were installed from a hook on
`sub_82346D98`, which returns the descriptor. That covers the descriptors *that
function creates* — `0x846AB000`, `0x9C5AD000` — and nothing else. The descriptor
in the faulting walk was **`0x846A8000`**, the first page `NtAllocateVirtualMemory`
carves, which is never returned by `sub_82346D98`. It had no methods at all.

Hooking creation was the wrong abstraction because creation is not one place. The
override on `sub_823460D8` already receives the descriptor in `r3`, so installing
there covers every descriptor the guest ever walks — strictly more general, and
it needs no list of creators. Guarded on `[+1412] == 0`, so it costs one load per
walk and nothing once the descriptor is set up.

**The lock is a real one.** `+1408` gets a genuine Win32 `CRITICAL_SECTION`
registered in the same `g_critsecs` map the `RtlEnterCriticalSection` /
`RtlLeaveCriticalSection` imports consult, addressed by real guest heap memory.
Not a stub that returns success — the enter and leave paths find it and lock it.

**Result, measured:**

| | before | after |
|---|---|---|
| distinct kernel APIs reached | 10 | **18** |
| heap allocations skipped | 2 | **0** |
| boot reaches | CRT heap walk | **GPU ring buffer** |

New APIs now reached include `VdInitializeEngines`, `VdInitializeRingBuffer`,
`VdSetGraphicsInterruptCallback`, `VdEnableRingBufferRPtrWriteBack`,
`VdShutdownEngines` and `VdSetSystemCommandBufferGpuIdentifierAddress`.

**The heap fault is no longer tolerated.** There are zero skips, so nothing on
this path is being worked around — the allocator, the method table and the lock
are all correct and the guest is using them for real.

That is the first time boot has left the CRT heap entirely and entered the
graphics stack. Current fault is `sub_822F1E68 +1563`, fault address
`0x7FC80714` — a host-range address, so it is the runtime or a stub rather than
the guest.

#### Boot is programming the Xenos GPU — the interception point is located

The current fault looked like a host pointer leak: `r11 = 0x7FC80000`, fault at
`r11 + 0x714`. **It is not a leak.** The instruction is

```asm
loc_822F1FE4:
    stw   r29,10820(r26)
    sync
    lis   r11,32712          ; r11 = 0x7FC80000
    stw   r29,1812(r11)      ; MMIO write to 0x7FC80000 + 0x714
    eieio
    sync
```

`lis` for the address, with `sync`/`eieio` fences bracketing the access, is
memory-mapped I/O — this is the guest writing the **Xenos GPU command
processor**. There is no such device on the PC, which is why it faults. Nothing
is wrong; this is where a port has to intercept.

**The job is smaller than feared.** Surveying every MMIO access in the recompiled
guest (`tools/mmio_survey.py`):

```
total MMIO accesses:  11
instruction forms:   PPC_MM_STORE_U32 x11   (no byte/half/dword loads at all)
distinct addresses:  9  (displacements 0x000, 0x714, 0x3214, 0x3408, 0x6110)
functions involved:  9
```

Eleven stores in nine functions, all 32-bit, all through `PPC_MM_STORE_U32`. The
displacements are Xenos command-processor register offsets, with `+0x0000` being
the command write port the ring buffer is pushed through.

**Why this matters for the 50% target.** The entire Xenos command stream reaches
the hardware through these eleven sites. Intercepting `PPC_MM_STORE_U32` when
the target falls in the `0x7FC80000` range captures the complete command
sequence as it is produced — which is exactly the stream that has to be
translated into D3D9 for anything to appear on screen.

That reframes shader translation too. The vertex and pixel programs travel as
*commands in this same stream*, not as separate blobs to be found. So the Xenos
bytecode to DXBC work is downstream of intercepting here, rather than an
independent task to go and locate the shaders in the image.

This is the first point in the project where the graphics work has a defined,
enumerable surface to attack rather than a guess about where rendering happens.

#### Three allocation-signature bugs found by refusing nonsense sizes

A guard added to `cod3_heap_alloc` refuses a byte count that lands inside the
guest's own address space, and names the caller. It immediately found three
distinct problems, which is the argument for having it: a guard that reports
what it refuses is worth more than one that silently succeeds.

**1. `ExAllocatePoolWithTag` / `ExAllocatePoolTypeWithTag` read the pool type as
the size.** The Xbox signatures are `(poolType, bytes, tag[, flags])`, so `r3` is
the type and `r4` is the size. Both read `r3`. `ExAllocatePool` was already
correct — which is why the inconsistency survived. This is the same class of
error as the `Vd*`/`Xam*` signatures: written from an assumed shape rather than
read off the call sites.

**2. The heap grow method read `r3` as a size.** It is a pointer. Passing it
through produced a 2,623,216,044-byte request, and that address lies inside the
very region the block walk was walking. The earlier one-page floor guarded only
small values; a large one sailed straight through. It now allocates one page and
says why.

**3. Thread stack allocation is receiving an address as `stackSize`.** Tag `STK`,
values `0x82738F00`, `0x82738F1C`, `0x82738F38`, `0x82738F54` — a 28-byte stride,
i.e. a table of stack descriptors being indexed — plus `0x9C5B19AC` from the heap.
Both `ExCreateThread` (`r8`) and `KeCreateThread` (`r5`) already read the
documented register, so **the signature is still wrong somewhere and has not been
established**. The next step is to read the register setup at the call sites, not
to assume a third shape.

That last one matters more than it looks: no guest thread can start until a stack
exists, and every `ExCreateThread` and `KeCreateThread` currently fails to
allocate one.

#### What to do next

Two things, in order.

**Done:** `+1408` is resolved. It is a critical section, not a method, and the
runtime implementation of `RtlInitializeCriticalSection` is already correct.

**Remaining, and it is the whole of it:** `sub_82347340` never runs. It is the
only thing that installs both `+1408` (the lock) and `+1412` (the grow method),
and it has no callers except through the vtable at `0x8208C600`, where it is
entry `0x8208C748`. Nothing holds a pointer to that entry and calls it, because
the kernel heap object that owns the descriptor is never constructed.

So the decision to make is narrow and concrete: **construct the owner object, or
populate the two fields directly.** The owner is at `r31` in `sub_82347340`
(`[r31+80]` is the descriptor, `[r31+164]` is the grow method), and its two
offsets are known. Populating them directly is a small, testable change; letting
the guest construct the object would be more faithful but requires finding who
was supposed to dispatch entry `0x8208C748`.

The open question is narrower than the previous commit claimed: not "which slice
of the table", but "what does `+1408` point at, and who was supposed to create
the object that owns the descriptor".

