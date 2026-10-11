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

#### The heap is vtable-dispatched from 0x8208C600

**This is the most structurally useful thing found so far**, and it came from a
positive control rather than from reasoning.

The unresolved-stub work produced a tool that scans the image for pointers to
stub addresses. It found **none** — 9,660 stubs, zero references. A negative
result is worthless on its own, so it was checked against addresses that are
certainly referenced. The control passed: real function addresses were found at
expected offsets. The negative is therefore real, and it is useful:

**None of the 9,660 unresolved stubs is ever called.** They are dead weight, not
holes in the port. An unresolved-branch count is *not* the explanation for any
fault seen so far, which removes a whole category of hypothesis.

The control addresses then turned out to be adjacent, which is what mattered:

```
0x8208C600  ->  0x82344A88   sub_82344A88
0x8208C608  ->  0x82344B30   sub_82344B30
...
0x8208C708  ->  0x82345E48   sub_82345E48
0x8208C710  ->  0x823460D8   sub_823460D8    <- the faulting walk
0x8208C718  ->  0x823464E0   sub_823464E0    <- the "extend", probed earlier
0x8208C720  ->  0x82346618   sub_82346618
0x8208C738  ->  0x82346D98   sub_82346D98    <- heap init
0x8208C748  ->  0x82347340   sub_82347340
0x8208C758  ->  0x823478F0   sub_823478F0    <- the heap core
0x8208C768  ->  0x823481D8   sub_823481D8    <- a guarded consumer
```

That is the Xbox 360 heap dispatch table. **Every function involved in the CRT
heap fault is an entry in it**, called through a function pointer rather than
directly. That reframes the whole investigation:

- The faults are not "a function does the wrong thing" — they are
  "a dispatched method received the wrong argument".
- `sub_823460D8` and `sub_823464E0` are *adjacent entries*, so the walk and the
  extend are two methods of the same object.
- This is almost certainly what the `+1404/+1408/+1412/+1416` fields on the heap
  descriptor are. `sub_824C2640` stores one value into all four, which is
  exactly how a sub-interface is built: point four consecutive slots at four
  consecutive table entries.

**Not yet acted on.** The obvious next step is to stop installing a custom
trampoline at `+1412` and instead point the descriptor's method slots at the
correct four-entry slice of this table — the table already contains the real
"extend" implementation (`sub_823464E0`), so a hand-written grow method may have
been solving a problem the guest's own dispatch already solves. That is a
concrete, testable change, and it follows from measurement rather than from a
reading of the code.

#### Two implementations built on inference, both reverted

Worth recording because the failure mode is the project's most persistent one.

`XamGetSystemVersion` and `XamLoaderGetLaunchData` are generated no-op stubs, and
both are reached during boot. Both were implemented from a reading of their call
sites. **Both attempts made boot worse — kernel APIs reached fell from 10 to 4 —
and both were reverted.**

What went wrong, precisely. The call site in `sub_82343C08` moves six registers
into callee-saved registers, which reads as six out-pointers, and the caller
then compares part of the return value against a constant:

```asm
rlwinm r11,r3,0,8,23      ; bits 8..23 of the version
lis   r10,8               ; r10 = 0x00080000
ori   r10,r10,41216       ; r10 = 0x0008A100
cmplw cr6,r11,r10
```

That was read as a minimum-kernel-version gate, with a stub returning zero
"failing" it. It is not a gate. `r11` is `0x000008A1` for any real version and
`r10` is `0x0008A100` — three decimal orders of magnitude apart, and a masked
value can never reach an unmasked one. The comparison cannot succeed whatever is
returned.

So the premise underneath was wrong as well: those six registers are evidently
not six out-pointers, and writing through them is what cost the four APIs.

The generated stubs stay. Returning zero is *wrong*, but it is wrong in a way
that does not corrupt memory, and nothing established here is good enough to
replace it. Reverting was correct after the first failure; the second attempt
should not have been made at all.

The generalisable lesson is already stated in this document after the heap
endianness work, and this is the same lesson again: **implement what the guest
demonstrates, leave alone what it does not.** Twice in this session a confident
reading of a call site turned out to be wrong, and both times the only evidence
was my own reasoning.

#### An attempt that was abandoned

The obvious follow-up to the above is to stop the crash handler printing a
meaningless offset for a stub. It should report *"inside sub_823460D8's
prologue"* rather than *"+1081"*, which is measured from four bytes of
`.long 0x0`.

**This was attempted and reverted.** The implementation scanned the generated
`ppc_recomp.*.cpp` for function bodies consisting of a single `.long 0x0`. The
logic was verified correct in isolation — a faithful reimplementation finds
**9,660** stubs — and it still found zero at runtime. The discrepancy was never
located: file discovery was confirmed working (`FindFirstFileA` finds them), and
the parse was confirmed working (the same code finds all 9,660 under Python).

Rather than leave a feature in the tree that reports nothing, it was reverted.
It is recorded here so it is not re-attempted from scratch, and because the
honest conclusion is that the runtime scan has a defect nobody has found yet —
not that the approach is wrong.

If it is picked up again, the useful first step is to print the *count and the
first few body strings* from inside the C++ and compare them side by side with
the Python reference in the same run. Doing that here produced a misleading
reading, because the diagnostic printed embedded newlines and the output was
truncated to the first eight lines — which made a correct `mnemonics=37` look
like a failure.

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

