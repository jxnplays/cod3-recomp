// =============================================================================
//  COD3 Recomp - the CRT heap grow method
// =============================================================================
//
//  ## Why this exists
//
//  Boot ends with the CRT walking a heap descriptor whose method table was never
//  populated. Measured:
//
//    heap=0x9C5AD000  [h+56]=0x846A8590  [h+1408]=0  [h+1412]=0 <- grow method
//    heap=0x9C7B6000  [h+1412]=0
//
//  sub_82345E48 then falls back to NtAllocateVirtualMemory and returns the raw
//  page as though it were an initialised block header. sub_823460D8 reads that
//  block's +12 field, gets zero, and faults.
//
//  ## How it was established that this is the loader's job
//
//  By search, not inference. No function in the recompiled guest writes the
//  descriptor tail fields:
//
//    sub_82346D98   creates descriptors; writes none of +1404..+1420
//    sub_823478F0   the heap core; builds blocks, writes block +12
//    sub_823478E8   the heap core's sibling; writes none
//
//  The only writers in the whole image are sub_824C2640 - observed operating on
//  image-resident addresses, 0x82A8E610 in and 0x82A90004 out, not these heap
//  descriptors - and three functions that never run during boot.
//
//  On the 360 the loader establishes these fields when it creates the initial
//  heap. The guest never does it because on hardware it never has to. So it is
//  loader work, and loader work belongs here.
//
//  ## What the fields are
//
//  From sub_824C2640, the only code in the image that writes them:
//
//      [r31+1404] = r30
//      [r31+1408] = r30
//      [r31+1412] = r30
//      [r31+1416] = r30
//
//  Four entries of a method table, each initialised to one object. +1412 is the
//  one sub_82345E48 tests before calling through it.
//
//  ## What this does NOT do
//
//  It does not fill the entries with a self-pointer. sub_82345E48 calls +1412 as
//  a function pointer, and a self-pointer would execute whatever bytes sit at the
//  descriptor. That is not a stub that "looks right"; it is a crash.
//
//  Instead +1412 gets a trampoline: a real function that grows the heap by the
//  ordinary carve and, crucially, WRITES A BLOCK HEADER onto the page it
//  returns. That initialisation is precisely what the fallback path omits, so
//  routing through here fixes the fault rather than hiding it.
//
//  ## Signature of the grow method
//
//  From sub_82345E48, at the bctrl:
//
//      r3 = a pointer into the region this block belongs to, NOT a size
//      r4 = out-parameter
//      r5 = the region's size limit, per the surrounding code
//      r6 = 0
//
//  and on return r3 is the block.
//
//  r3 being a pointer was measured, not assumed: passing it through as a size
//  produced a 2,623,216,044-byte allocation, and that value lies inside the very
//  region the block walk was walking. Reading it as a length was the bug.
//
//  The block header layout, from sub_823478F0 and sub_823460D8:
//
//      +0   size in 16-byte units   (lhz, then rotlwi 4 -> x16)
//      +5   flags byte
//      +8   next-block link
//      +12  previous-block link
//      +20  in-use flag
//
// =============================================================================

#include "ppc_recomp_shared.h"

#include "cod3_runtime.h"

#include <cstdio>

namespace {

constexpr uint32_t kBlockSizeUnitsOffset = 0;    // size / 16
constexpr uint32_t kBlockSizeBytesOffset = 0;    // same word, byte view
constexpr uint32_t kBlockFlagsOffset     = 5;
constexpr uint32_t kBlockFlagsWordOffset = 4;
constexpr uint32_t kBlockNextOffset      = 8;
constexpr uint32_t kBlockPrevOffset      = 12;
constexpr uint32_t kBlockInUseOffset     = 20;

// The header is 16 bytes on the 360; the payload is whatever was asked for,
// rounded up to a page.
constexpr uint32_t kBlockHeaderBytes = 16;

} // namespace

// -----------------------------------------------------------------------------
// cod3_heap_grow
// -----------------------------------------------------------------------------
//
// Installed into a heap descriptor's +1412 by cod3_install_heap_grow_method.
// Reached through the guest's own `bctrl`, so it takes the guest calling
// convention and may change the live context.
void cod3_heap_grow(PPCContext& __restrict ctx, uint8_t* base)
{
    (void)base;

    // r3 is NOT the requested size. It is an address - a pointer into the heap
    // region this block belongs to. The guest passed 0x9C5B19AC and the runtime
    // tried to allocate 2,623,216,044 bytes, because the value was read as a
    // length. That address is inside the same region the block walk was walking.
    //
    // So the size cannot be taken from r3 at all. One page is used, which is
    // always enough for the header this function writes and always small enough
    // to succeed. Guessing a size from an address that is not one would be a
    // worse failure than not guessing at all.
    uint32_t want = 0x1000;

    // Guards retained deliberately. They are not dead code: they make the
    // behaviour explicit if this is ever changed to trust r3 again, and the
    // ceiling is what stops a pointer from outside the heap reappearing as an
    // unbounded request.
    if (want < 0x1000)
        want = 0x1000;

    const uint32_t payload = (want + 0xFFF) & ~0x1000u;
    const uint32_t total = payload + kBlockHeaderBytes;

    void* mem = cod3_heap_alloc(total, 'CRGB');
    const uint32_t block =
        static_cast<uint32_t>(reinterpret_cast<uintptr_t>(mem));

    if (block == 0)
    {
        // Report failure the way the CRT expects: a null block. Callers test for
        // it, and returning an address we cannot back would be worse.
        ctx.r3.u64 = 0;
        return;
    }

    // The block header - the initialisation the fallback path omits.

    // Size in 16-byte units, read by sub_823460D8 as `lhz r11,2(r30)`.
    //
    // That is the LOW half of the big-endian word at +0, not the high one. The
    // word helper is big-endian, so the value goes in bits 0..15 and the upper
    // half is left alone - it belongs to whatever else lives in that word.
    //
    // This was wrong at first: the units were written to the high half, so the
    // walk read a size of zero, computed r31 = r30 - 0, and examined the block
    // header as though it were its own predecessor. That is not a subtle
    // degradation - it walks the list in the wrong direction entirely.
    const uint32_t units = total >> 4;
    const uint32_t sizeWord = cod3_load_u32(block + kBlockSizeUnitsOffset);
    cod3_store_u32(block + kBlockSizeUnitsOffset,
                   (sizeWord & 0xFFFF0000u) | (units & 0xFFFFu));

    // +5: a flags byte - bit 0 "in use", bit 4 set by sub_823478F0 on free. A
    // freshly carved block is in use and not free. Byte 5 is byte index 1 of the
    // big-endian word at +4, so it goes in bits 16..23.
    const uint32_t word = cod3_load_u32(block + kBlockFlagsWordOffset);
    cod3_store_u32(block + kBlockFlagsWordOffset,
                   (word & 0xFF00FFFFu) | 0x00010000u);

    cod3_store_u32(block + kBlockNextOffset, 0);
    cod3_store_u32(block + kBlockPrevOffset, 0);
    cod3_store_u32(block + kBlockInUseOffset, 1);

    static unsigned long long grows = 0;
    if (++grows <= 8 || (grows % 1000) == 0)
    {
        std::fprintf(stderr,
            "[cod3] heap grow #%llu: %u bytes -> block 0x%08X\n",
            grows, total, block);
        std::fflush(stderr);
    }

    ctx.r3.u64 = block;
}

// -----------------------------------------------------------------------------
// cod3_install_heap_methods
// -----------------------------------------------------------------------------
//
// Populates the two fields of a heap descriptor that sub_82347340 would have
// installed on hardware, and installs neither here because that function is
// reached only through the heap dispatch table at 0x8208C600 - it is entry
// 0x8208C748 - and the kernel heap object owning the descriptor is never
// constructed. On a 360 the loader constructs it before the CRT starts.
//
//     +1408   a critical section guarding heap operations
//     +1412   the grow method sub_82345E48 tests before falling back to raw
//             allocation
//
// Only +1412 was being filled before. +1408 was left null, and it is loaded into
// r3 and passed straight on as an argument, which is how a fault at a small
// address happens.
//
// Only these two are written. +1416, +1420 and +1424 look like neighbouring
// fields but are float32 values read with `lfs` - an earlier reading of this
// area took them for method slots, which was wrong.
void cod3_install_heap_methods(uint32_t heap)
{
    if (heap < g_guest_heap_base ||
        heap >= g_guest_heap_base + g_guest_heap_size)
    {
        return;
    }

    // The grow method. The guest calls [heap+1412] through `bctrl`, so the
    // address has to be a slot the indirect table resolves - not a host pointer,
    // and not a self-pointer, which would execute the descriptor.
    const uint32_t slot = cod3_heap_grow_service_address();
    if (slot == 0)
    {
        return;
    }

    cod3_store_u32(heap + 1412, slot);

    // The lock. A real Win32 CRITICAL_SECTION, registered where the Rtl* enter
    // and leave imports will find it.
    cod3_heap_install_lock(heap);

    std::fprintf(stderr,
        "[cod3] heap: installed methods into descriptor 0x%08X "
        "(+1408 lock, +1412 grow = 0x%08X)\n",
        heap, slot);
    std::fflush(stderr);
}
