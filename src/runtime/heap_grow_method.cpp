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
//      r3 = the allocation key
//      r4 = out-parameter
//      r5 = the region's size limit, per the surrounding code
//      r6 = 0
//
//  and on return r3 is the block. The block header layout, from sub_823478F0
//  and sub_823460D8:
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

    uint32_t want = ctx.r3.u32;

    // A floor rather than a floor-and-trust: a block smaller than one page makes
    // the header arithmetic in sub_823460D8 walk off the front of the region,
    // which is the exact fault this path exists to prevent.
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

    // +0: size in 16-byte units, as a halfword. There is no 16-bit store helper
    // and the word helper is big-endian, so the value is placed in the upper half
    // where a big-endian halfword at offset 0 lives.
    const uint32_t units = total >> 4;
    cod3_store_u32(block + kBlockSizeUnitsOffset,
                   (units << 16) & 0xFFFF0000u);

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
// cod3_install_heap_grow_method
// -----------------------------------------------------------------------------
//
// Writes the method entry onto a heap descriptor the CRT has created, so the
// guest takes its real grow path instead of the fallback.
//
// Only +1412 is written. The other entries were set by whatever created the
// descriptor and overwriting them would discard state that may already be
// correct; +1412 is the one known to be required for the allocator to work.
void cod3_install_heap_grow_method(uint32_t heap)
{
    if (heap < g_guest_heap_base ||
        heap >= g_guest_heap_base + g_guest_heap_size)
    {
        return;
    }

    // The guest calls this through `bctrl`, a PPC indirect call, so the address
    // stored here has to be a slot the indirect table resolves - not a host
    // pointer, and not a self-pointer.
    const uint32_t slot = cod3_heap_grow_service_address();
    if (slot == 0)
    {
        return;
    }

    cod3_store_u32(heap + 1412, slot);

    std::fprintf(stderr,
        "[cod3] heap: installed grow method 0x%08X into descriptor 0x%08X\n",
        slot, heap);
}