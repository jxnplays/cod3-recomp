// =============================================================================
//  COD3 Recomp - MmAllocatePhysicalMemoryEx
// =============================================================================
//
//  ## Why it matters
//
//  The generated stub returned 0 without allocating anything, and the guest calls
//  this 60 times during boot. Its only caller on that path, sub_82344EE8, checks
//  the result immediately:
//
//      0x82344F44  bl    MmAllocatePhysicalMemoryEx
//      0x82344F48  mr.   r31,r3
//      0x82344F4C  bne   0x82344f54      non-zero -> success
//      0x82344F50  li    r3,8            zero -> error
//
//  All 60 calls took the failure path, so whatever the caller was building was
//  never built. The fault that ends boot is consistent with that: sub_824D5D60
//  loads r4 from [[0x8268DEEC]+4] and passes it to sub_820B0410, which does
//
//      mr   r26,r4
//      lbz  r11,101(r26)
//
//  With r4 zero the load faults at 101 decimal = 0x65, which is exactly the
//  reported fault address. So this import is directly upstream of the crash.
//
//  ## Signature
//
//  Taken from the call sites rather than assumed, and the first reading was wrong.
//
//  sub_82344EE8 rearranges its own arguments before calling:
//
//      r8 = r5_in        r5 = r6_in
//      if [0x8291F6B4]:  r5 &= ~0x20000000
//      if r4_in == -1:   r6 = 0,  r7 = -1
//      else:             r6 = r4_in, r7 = (r3_in + r4_in) - 1
//      r4 = r3_in
//      r3 = 0
//
//  Its own caller sub_823439B8 supplies (r3 = size, r4 = -1, r5 = 1 << scale,
//  r6 = a table value), so the arguments that actually reach this function are:
//
//      r3 = 0   Reserved
//      r4 = size
//      r5 = table value
//      r6 = 0 or the caller's old r4
//      r7 = -1 or (size + old r4) - 1
//      r8 = scale
//
//  The size is therefore in r4. Reading it from r6 - which is what the obvious
//  "TotalBytes is the third pointer argument" guess suggests - yields zero every
//  time, because r6 is 0 on the common path. That mistake was made and caught:
//  every one of the 60 calls reported a zero-byte request.
//
//  So the parameters are:
//
//      Reserved          r3
//      TotalBytes        r4
//      ???               r5   a page-table-derived value, not used here
//      LowestAddress     r6   0 = no constraint
//      HighestAddress    r7   -1 = no constraint
//      Flags             r8   a power-of-two scale
//
//  On the 360 the result register r3 carries the allocated base, and zero means
//  failure - the caller turns that into the error code 8. So this returns a guest
//  virtual address in r3, not a status.
//
//  ## Implementation
//
//  There is no physical memory on this port, so "physical" allocation is a request
//  for guest virtual memory. It carves from the same region as
//  NtAllocateVirtualMemory: the guest is a single address space here, so the two
//  must not hand out overlapping ranges.
//
//  The in/out parameters are honoured rather than ignored. The CRT checks the
//  returned base against the range it asked for, so returning an address outside
//  LowestAddress..HighestAddress would be rejected by the caller even though it
//  looks like a successful allocation.
//
// =============================================================================

#include "ppc_recomp_shared.h"

#include "cod3_runtime.h"

#include <cstdio>

namespace {

// The carve point, shared with NtAllocateVirtualMemory so the two allocators
// cannot hand out the same bytes. The guest is one address space on this port, so
// if these used separate bump pointers they would eventually overlap and hand the
// same bytes out twice.
uint32_t& vm_brk()
{
    return *g_vm_brk_slot();
}

} // namespace

PPC_FUNC(__imp__MmAllocatePhysicalMemoryEx)
{
    // See the header comment for how this was arrived at. In short: the size is
    // in r4, not r6. Reading it from r6 gives zero on every call, because r6 is
    // the LowestAddress slot and is 0 on the common path.
    const uint32_t reserved = ctx.r3.u32;
    const uint32_t bytes = ctx.r4.u32;
    const uint32_t lowest = ctx.r6.u32;   // 0 = no constraint
    const uint32_t highest = ctx.r7.u32;  // 0xFFFFFFFF = no constraint
    const uint32_t flags = ctx.r8.u32;    // a power-of-two scale

    // A zero-length request is a caller bug rather than a legitimate ask. The
    // guest treats a zero return as failure and reports error 8, which is a
    // truthful outcome.
    static unsigned zeroRequests = 0;
    if (bytes == 0)
    {
        if (zeroRequests++ < 4)
        {
            std::fprintf(stderr,
                "[cod3] MmAllocatePhysicalMemoryEx #%u: zero-byte request;"
                " r3=0x%08X r4=0x%08X r5=0x%08X r6=0x%08X r7=0x%08X r8=0x%08X\n",
                zeroRequests, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32,
                ctx.r6.u32, ctx.r7.u32, ctx.r8.u32);
        }
        ctx.r3.u64 = 0;
        return;
    }

    // Log the first few requests in full. The parameter order was wrong once
    // already - the size was read from r6 instead of r4, which reported every
    // call as zero - and that was only caught because the numbers looked wrong.
    static unsigned logged = 0;
    if (logged++ < 8)
    {
        std::fprintf(stderr,
            "[cod3] MmAllocatePhysicalMemoryEx #%u: r3=0x%08X bytes=0x%08X"
            " r5=0x%08X lowest=0x%08X highest=0x%08X flags=0x%08X\n",
            logged, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32,
            ctx.r7.u32, ctx.r8.u32);
    }

    // Round up to a page. Everything in this port is page granular, and handing
    // back a partially mapped range would be worse than rounding up.
    const uint32_t kPageSize = 0x1000;
    const uint32_t pageAligned = (bytes + kPageSize - 1) & ~(kPageSize - 1);

    uint32_t& brk = vm_brk();
    if (brk == 0)
        brk = g_guest_heap_base + kLoaderRegionBytes;

    // Honour an explicit lowest address when the caller supplies one. 0 means
    // "no constraint", which is the common case.
    if (lowest != 0 && brk < lowest)
        brk = (lowest + kPageSize - 1) & ~(kPageSize - 1);

    if (brk + pageAligned > g_guest_heap_base + g_guest_heap_size)
    {
        static unsigned over = 0;
        if (over++ < 4)
        {
            std::fprintf(stderr,
                "[cod3] MmAllocatePhysicalMemoryEx #%u: %u (0x%X) bytes exceeds"
                " the guest heap; brk=0x%08X heap=0x%08X..0x%08X\n",
                over, bytes, bytes, brk,
                g_guest_heap_base,
                g_guest_heap_base + g_guest_heap_size);
        }
        ctx.r3.u64 = 0;
        return;
    }

    const uint32_t result = brk;
    brk += pageAligned;

    // Respect an explicit upper bound if one was given and we have overrun it.
    // Returning an address outside the requested range would be rejected by the
    // caller even though it looks like a success.
    if (highest != 0xFFFFFFFFu && result >= highest)
    {
        std::fprintf(stderr,
            "[cod3] MmAllocatePhysicalMemoryEx: no room below the requested"
            " ceiling 0x%08X\n", highest);
        ctx.r3.u64 = 0;
        return;
    }

    // The memory has to be zeroed: the caller is getting fresh pages from its
    // point of view, and the region is reused guest heap that may hold the
    // previous run's leftovers.
    uint8_t* mem = reinterpret_cast<uint8_t*>(result);
    __builtin_memset(mem, 0, pageAligned);

    if (!cod3_vm_quiet())
    {
        std::fprintf(stderr,
            "[cod3] MmAllocatePhysicalMemoryEx: %u bytes -> 0x%08X"
            " (reserved=0x%08X lowest=0x%08X highest=0x%08X flags=0x%08X)\n",
            pageAligned, result, reserved, lowest, highest, flags);
    }

    // On the 360 r3 is the base address, and zero means failure.
    ctx.r3.u64 = result;
}