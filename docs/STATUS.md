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

#### Current frontier: an unresolved 4-byte thunk

The fault is now reported at `sub_823460D4 +1081`, and that offset is **not
trustworthy**. `sub_823460D4` is a 4-byte stub containing only `.long 0x0`, and
`sub_823460D8` is a weak alias immediately after it at +12. The registers at the
crash are:

```
r3  = 0x846A8000     lr  = 0x823460E0
r12 = 0x82346E34
```

`lr = 0x823460E0` is the instruction immediately after `bl 0x8234f28c` in
`sub_823460D8`'s prologue, and `r12 = 0x82346E34` is the call site inside
`sub_82346D98` — the same call previously identified as
`bl 0x823460d8; lr = 0x82346E34`. So execution is in the **prologue of the real
function**, being attributed to a stub 12 bytes earlier.

Two readings, neither established:

1. An indirect call genuinely lands at `0x823460D4`, i.e. 12 bytes before the
   real entry. That would be a mis-resolved function pointer.
2. The crash handler is attributing an in-prologue fault to the nearest preceding
   symbol. The `+1081` is then meaningless, exactly as it is for any stub.

Supporting (2): a search for `0x823460D4` in the loaded image as a pointer value
returns **zero** occurrences, and no code in the image branches to it. If it were
a real call target it would be referenced from somewhere.

Not resolved. The cheap next step is to make the crash handler refuse to compute
an offset inside a stub — report "inside `sub_823460D8` prologue" instead of a
number computed from a 4-byte `.long`. That is a diagnostic-honesty fix rather
than a port fix, but it stops this particular address being re-derived.

### Heap fault tolerance (opt-in, `COD3_TOLERATE_HEAP_FAULT=1`)

**This does not fix the region headers.** It makes the diagnosed faults
survivable so the rest of the boot path can be worked on. Every skip is reported
with its region, count and running total, and the total prints on any crash.
Without the environment variable nothing changes.

A port that reaches the renderer this way is not a working port. What it buys is
the ability to work on everything downstream instead of everything upstream.

