#pragma once

#include <cstdint>

// Which pages of physical memory the CPU has written, told by the pages
// themselves rather than by hashing what is in them (COD3_WRITEWATCH=1).
//
// A resource the renderer has found unchanged for a while is armed: its
// pages are made read only in all three windows physical memory is seen
// through, and the first write to one of them faults; the fault handler
// notes the page as written at the next sequence number and makes it
// writable again. A resource whose pages are all still armed and none
// written since it was armed is unchanged, without a byte of it read.
//
// Writes the CPU does not make itself (a file read into guest memory, which
// the operating system does and which a read only page would fail) are
// announced with Forget, as are the pages the allocator frees and gives out
// again, whose protection it changes itself.
namespace WriteWatch
{
    bool Enabled();

    // The sequence the next write will be past.
    uint64_t Sequence();

    // The pages of [physical, physical + bytes) made read only. False when
    // some could not be (not committed): the range is not watched then.
    bool Arm(uint32_t physical, uint32_t bytes);

    // Whether every page of the range is armed and none has been written
    // since `since` (a Sequence() taken before Arm).
    bool Clean(uint32_t physical, uint32_t bytes, uint64_t since);

    // The range is about to be written, or has changed hands: its pages are
    // no longer watched and count as written now.
    void Forget(uint32_t guestAddress, uint32_t bytes);

    // From the fault handler: a write to an armed page, which is noted and
    // made writable. False when the fault is none of this.
    bool HandleFault(const void* hostAddress);
}
