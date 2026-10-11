// =============================================================================
//  COD3 Recomp - CRT heap fault tolerance
// =============================================================================
//
//  ## Why this exists
//
//  Boot stops in sub_823460D8 with an access violation at address 0x0:
//
//      0x823460F4  lhz   r11,2(r30)      ; region block count
//      0x823460F8  rotlwi r11,r11,4
//      0x823460FC  subf  r31,r11,r30     ; r31 = r30 - size*16
//      ...
//      lwz   r11,12(r31)                  ; faults: r31 is outside the region
//
//  The region header at 0x846A8000 carries a block count of 0x63, so the
//  backwards walk lands 0x630 bytes before the region it belongs to.
//
//  The underlying cause is still open. What is established: the CRT allocates
//  the page through NtAllocateVirtualMemory, which zeroes it, and the field is
//  not zero, so either the guest wrote it or the region was never initialised.
//  sub_823478F0 - the only writer of a block's +12 back pointer - runs six times,
//  at higher addresses.
//
//  ## What this does, and what it does not claim
//
//  This does NOT fix that. It makes one specific, diagnosed fault survivable so
//  that execution continues past it.
//
//  That matters because the fault is in CRT heap initialisation, which happens
//  long before the renderer runs. The graphics stack - where most of the
//  remaining work is - is unreachable while boot dies here, and every hour spent
//  characterising this one field has produced no forward movement. Making the
//  failure visible as "the allocator returned nothing" rather than "the process
//  died" lets the rest of the boot path be exercised and worked on.
//
//  ## How it works
//
//  sub_823460D8 returns a null block to its caller instead of dereferencing
//  memory the region does not own. Callers already handle a null result -
//  sub_82346D98 checks `mr. r11,r3 ; beq` after each call and bails - so this
//  degrades allocation failure rather than crashing, which is what the CRT is
//  written to expect.
//
//  ## Scope and honesty
//
//  This is opt-in. Without COD3_TOLERATE_HEAP_FAULT=1 the crash handler behaves
//  exactly as before, because a run that quietly limps past a broken heap is
//  worse than one that stops and says so. With it enabled, every skip is
//  reported with its address and count so it is never invisible.
//
//  A port that reaches the renderer this way is NOT a working port. The heap is
//  still wrong. What it buys is the ability to work on everything downstream
//  instead of everything upstream.
//
// =============================================================================

#include "ppc_recomp_shared.h"

#include "cod3_runtime.h"

#include <cstdio>
#include <cstdlib>

namespace {

bool tolerate()
{
    static const bool on =
        std::getenv("COD3_TOLERATE_HEAP_FAULT") != nullptr;
    return on;
}

unsigned long long skips = 0;

} // namespace

// -----------------------------------------------------------------------------
// sub_823460D8 - the function that faults
// -----------------------------------------------------------------------------
//
// Returns without touching the block list when the computed block address falls
// outside the region. The guest's own arithmetic is reproduced rather than
// re-derived, so the check cannot drift from the code it is guarding.
// -----------------------------------------------------------------------------
// The shared rule
// -----------------------------------------------------------------------------
//
// A region header is walked backwards from its end: the block count at +2 is
// scaled by 16 and subtracted from the region base. If the header does not
// describe a region the CRT set up, that arithmetic produces an address outside
// the region and every consumer of the list faults.
//
// The test is the same for every consumer, stated once:
//
//   - the region must lie inside the guest heap
//   - the block count must be non-zero
//   - the walk must stay inside the heap and at or above the region base
//
// It is deliberately conservative. Skipping is reported and recoverable;
// faulting is neither. When this is wrong it will skip work that should have
// run, and the skip count is printed at the end precisely so that is visible.
bool region_unwalkable(uint32_t region)
{
    if (!tolerate())
        return false;

    if (region < g_guest_heap_base ||
        region >= g_guest_heap_base + g_guest_heap_size)
        return true;

    const uint32_t count = (cod3_load_u32(region) >> 16) & 0xFFFFu;
    if (count == 0)
        return true;

    const uint32_t rotated = (count << 4) | (count >> 12);
    const uint32_t block = region - rotated;

    return block < region ||
           block < g_guest_heap_base ||
           block >= g_guest_heap_base + g_guest_heap_size;
}

void note_skip(const char* fn, uint32_t region)
{
    if (++skips <= 16 || (skips % 1000) == 0)
    {
        const uint32_t count = (cod3_load_u32(region) >> 16) & 0xFFFFu;
        std::fprintf(stderr,
            "[cod3] heap: skipping %s #%llu (region 0x%08X, count 0x%04X)\n",
            fn, skips, region, count);
        std::fflush(stderr);
    }
}

extern "C" void __imp__sub_823460D8(PPCContext& __restrict ctx, uint8_t* base);

PPC_FUNC(sub_823460D8)
{
    PPC_FUNC_PROLOGUE();

    if (tolerate())
    {
        const uint32_t r30 = ctx.r30.u32;
        const uint32_t half = (cod3_load_u32(r30) >> 16) & 0xFFFFu;
        const uint32_t rotated = (half << 4) | (half >> 12);
        const uint32_t r31 = r30 - rotated;

        // A first attempt guarded only r31, the walked block. That was too narrow:
        // the fault moved to a different register and to address 0x2, a small
        // integer rather than a heap address. While the region header is wrong,
        // anything derived from it can be wrong, so guard the whole call on the
        // header itself rather than on one computed value.
        //
        // Plausibility, checked in this order:
        //
        //   - r30 must be inside the guest heap. It is a region the CRT carved.
        //   - the block count must be non-zero, or the region is empty and there
        //     is nothing to walk.
        //   - the walk must stay inside the region: a block below the region base
        //     is outside memory the region owns.
        //
        // The region extent is not known here - the caller allocated it and this
        // function only sees the header - so "inside the region" is approximated
        // by "inside the heap and above the region". That is deliberately
        // conservative: it skips more rather than less, because a skip is
        // reported and a crash is not.
        const bool r30Ok = r30 >= g_guest_heap_base &&
                           r30 < g_guest_heap_base + g_guest_heap_size;
        const bool walkOk = rotated != 0 && r31 >= r30 &&
                            r31 >= g_guest_heap_base &&
                            r31 < g_guest_heap_base + g_guest_heap_size;

        if (!r30Ok || !walkOk)
        {
            if (++skips <= 16 || (skips % 1000) == 0)
            {
                std::fprintf(stderr,
                    "[cod3] heap: skipping sub_823460D8 #%llu"
                    " (region 0x%08X r30Ok=%d, block 0x%08X walkOk=%d,"
                    " count=0x%04X)\n",
                    skips, r30, r30Ok ? 1 : 0, r31, walkOk ? 1 : 0, half);
                std::fflush(stderr);
            }

            // Report failure the way the CRT expects: a null block. Callers
            // already test for it.
            ctx.r3.u64 = 0;
            return;
        }
    }

    __imp__sub_823460D8(ctx, base);
}

// Called by the crash reporter so a run that limped to its end still says how
// many allocations were skipped.
void cod3_report_heap_tolerance()
{
    if (skips == 0)
        return;

    std::fprintf(stderr,
        "[cod3] heap: %llu allocation(s) skipped via COD3_TOLERATE_HEAP_FAULT."
        " The CRT heap is still not correct; boot completed only because those"
        " failures were non-fatal.\n", skips);
}

// -----------------------------------------------------------------------------
// sub_823481D0 and sub_823481D8 - the other consumers of the block list
// -----------------------------------------------------------------------------
//
// These walk the same list as sub_823460D8 and fault the same way when a region
// header does not describe the region it belongs to.
//
// They were removed once on the assumption that installing the grow method fixed
// them. It does not: that fixes the ALLOCATOR, not the region headers. Boot went
// from sub_823460D8 +72 straight back to sub_823481D8 +1057 with the guards
// gone. Restored, with the distinction recorded so it is not re-derived.
extern "C" void __imp__sub_823481D0(PPCContext& __restrict ctx, uint8_t* base);

PPC_FUNC(sub_823481D0)
{
    PPC_FUNC_PROLOGUE();

    if (region_unwalkable(ctx.r30.u32))
    {
        note_skip("sub_823481D0", ctx.r30.u32);
        ctx.r3.u64 = 0;
        return;
    }

    __imp__sub_823481D0(ctx, base);
}

extern "C" void __imp__sub_823481D8(PPCContext& __restrict ctx, uint8_t* base);

PPC_FUNC(sub_823481D8)
{
    PPC_FUNC_PROLOGUE();

    if (region_unwalkable(ctx.r30.u32))
    {
        note_skip("sub_823481D8", ctx.r30.u32);
        ctx.r3.u64 = 0;
        return;
    }

    __imp__sub_823481D8(ctx, base);
}
