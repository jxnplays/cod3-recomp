# Findings

Analysis of Call of Duty 3 specifically. None of this is in the inherited
documentation, and all of it cost real time to establish. It is kept because the
next person to need it would otherwise pay for it again.

Everything here was measured. Where something is inferred it says so.

---

## The heap is vtable-dispatched from `0x8208C6xx`

The CRT heap is not called directly. Its functions are contiguous entries in a
dispatch table, reached through a function pointer.

```
0x8208C708  ->  sub_82345E48   the allocator
0x8208C710  ->  sub_823460D8   the block walk
0x8208C718  ->  sub_823464E0   the extend
0x8208C738  ->  sub_82346D98   heap initialisation
0x8208C748  ->  sub_82347340   method installer  <- never runs
0x8208C758  ->  sub_823478F0   the heap core
0x8208C768  ->  sub_823481D8   a consumer
```

Consequences worth remembering:

- Faults here are not "a function does the wrong thing" but **"a dispatched
  method received the wrong argument"**
- The walk and the extend are *adjacent entries* — two methods of one object
- `sub_82347340` installs the descriptor's method slots and **has no callers
  except through this table**. It never runs during boot, which is why `+1412`
  (the grow method) is zero and `+1408` (a lock) is null.

## The block header's size halfword

`sub_823460D8` reads a block's size as:

```asm
lhz    r11,2(r30)      ; LOW half of the big-endian word at +0
rotlwi r11,r11,4       ; x16
subf   r31,r11,r30     ; r31 = r30 - size
```

**Low** half. Writing the count to the high half makes the walk read size zero,
compute `r31 = r30 - 0`, and treat every block as its own predecessor — it walks
the list in the wrong direction entirely.

The lesson generalises past this bug: a guard that reads the same field as the
code it guards must agree with it about which half is which, and agreeing is not
automatic.

## Guest MMIO: eleven sites, all 32-bit

```
total MMIO accesses:  11
instruction forms:   PPC_MM_STORE_U32 x11 — and nothing else
distinct addresses:  9   (displacements 0x000, 0x714, 0x3214, 0x3408, 0x6110)
functions involved:  9
```

`0x7FC80000` is the **Xenos GPU command processor**. The accesses are bracketed
by `sync`/`eieio`, which is memory-mapped I/O.

The whole Xenos command stream reaches the hardware through those eleven sites.
Vertex and pixel programs travel *inside* that stream as commands, not as blobs
to be found in the image — so shader translation is downstream of intercepting
here rather than an independent hunt.

## The unresolved stubs are dead weight

`0x82000000`-based recompilation emits **9,660** functions as a single
`.long 0x0` — four bytes of data, not code.

Scanning the loaded image for pointers to those addresses finds **none of them**.
Verified with a positive control: the same scan finds real function pointers at
expected offsets, so the negative is real.

They are dead weight, not holes in the port. An unresolved-branch count is not
the explanation for any fault.

## Process notes, from getting that wrong

Three mistakes worth not repeating, all the same shape — a confident reading of
code that had not been watched running:

1. **`ExAllocatePoolWithTag` read the pool type as the size.** The Xbox signature
   is `(poolType, bytes, tag)`, so `r3` is the type and `r4` is the size. It read
   `r3`. `ExAllocatePool` was already correct, which is why the inconsistency
   survived.
2. **A grow method read `r3` as a length when it is a pointer.** Passing it
   through asked for 2,623,216,044 bytes, and the address lay inside the very
   region the walk was walking.
3. **A counter reported 9,854 unresolved branches** when the honest figure was
   far lower — nearly all of them were the floating-point save/restore helper
   table, which *is* implemented. The filter only accepted `if (`, `goto` or
   `ctx.lr = ` as evidence.

The instrument that found most of these was a guard that **reports what it
refuses**: refusing a byte count inside the guest's own address space found three
distinct signature bugs in a single run.

## Open, and unresolved

- **Depth testing is implemented**, contrary to the inherited next-steps list,
  which is stale on this point. `render_pipeline.cpp` builds a full
  `D3D11_DEPTH_STENCIL_DESC` from `RB_DEPTHCONTROL` and binds it per draw.
  Something else is making frames look wrong.
- **Stencil op field ordering is unconfirmed.** The mapping reads
  `FailOp(11-13) → PassOp(14-16) → DepthFailOp(17-19)`. The Xenos layout may have
  ZFail and Pass the other way round. *Not verified* — it needs Xenia's
  `RB_DEPTHCONTROL` definition, which is the first thing to check.
- **The recompiled output is unverified.** Nothing checks that the translated
  instructions compute what the originals did. `scripts/verify.ps1` checks the
  recompiler against Xenia's instruction tests, which is the closest thing to it.
