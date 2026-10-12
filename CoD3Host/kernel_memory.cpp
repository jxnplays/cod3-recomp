// Xbox 360 kernel imports: memory, and the critical sections built on it.
//
// Guest memory is one 4 GB reservation with pages committed on first touch, so
// an allocator here only has to hand out addresses; the fault handler makes
// them usable. Two heaps. The physical one is a bump pointer with a free
// list in front of it: what the title frees is given out again, first fit,
// since a level's textures and buffers come and go with the level and the
// heap is half a gigabyte. The virtual one stays a bump pointer: the title
// reserves ranges there and commits and releases pieces inside them at
// addresses of its choosing, and a piece given out again under such a
// range was read through by a stream and ran off the end of memory.

#include "kernel.h"
#include "write_watch.h"
#include "render.h"
#include <atomic>

#include <cstdio>
#include <cstdlib>
#include <map>
#include <vector>
#include <mutex>

#include <Windows.h>

namespace
{
    // Allocation type bits the title passes to NtAllocateVirtualMemory.
    constexpr uint32_t X_MEM_COMMIT      = 0x00001000;
    constexpr uint32_t X_MEM_RESERVE     = 0x00002000;
    constexpr uint32_t X_MEM_DECOMMIT    = 0x00004000;
    constexpr uint32_t X_MEM_RELEASE     = 0x00008000;
    constexpr uint32_t X_MEM_LARGE_PAGES = 0x20000000;
    constexpr uint32_t X_MEM_16MB_PAGES  = 0x80000000;

    constexpr uint32_t X_STATUS_SUCCESS               = 0x00000000;
    constexpr uint32_t X_STATUS_NO_MEMORY             = 0xC0000017;
    constexpr uint32_t X_STATUS_INVALID_PARAMETER     = 0xC000000D;

    constexpr uint32_t SmallPage = 4u << 10;
    constexpr uint32_t LargePage = 64u << 10;

    uint32_t Align(uint32_t value, uint32_t to) { return (value + to - 1) & ~(to - 1); }

    struct Region { uint32_t size; bool physical; };

    // Every live allocation, so a free can report a size and the address
    // space does not silently leak into a wrong answer later.
    // Allocation happens on every guest thread, so the table and the bump
    // pointers need a lock of their own.
    std::mutex g_allocatorMutex;

    std::map<uint32_t, Region>& Regions()
    {
        static std::map<uint32_t, Region> regions;
        return regions;
    }

    // What happened to the heaps' regions, oldest first: each allocation
    // (fresh or out of a freed range) and each free, with the caller. When
    // the title reads memory it freed, the report says whose the memory was
    // and when it changed hands. The last few thousand only.
    struct Event { uint32_t address, size, from; char kind; };
    std::vector<Event>& History()
    {
        static std::vector<Event> events;
        return events;
    }
    void Note(char kind, uint32_t address, uint32_t size, uint32_t from)
    {
        std::vector<Event>& events = History();
        if (events.size() >= 16384) events.erase(events.begin(), events.begin() + 4096);
        events.push_back({ address, size, from, kind });
    }

    uint32_t g_virtualNext = Guest::VirtualHeapBase;
    uint32_t g_physicalNext = Guest::PhysicalHeapBase;

    // What has been freed and not given out again, by address, the
    // neighbours joined: one list for each heap. Beside it, when each range
    // was freed (for a joined range, its latest part).
    std::map<uint32_t, uint32_t>& FreeList(bool physical)
    {
        static std::map<uint32_t, uint32_t> lists[2];
        return lists[physical ? 1 : 0];
    }
    std::map<uint32_t, uint64_t>& FreedAt(bool physical)
    {
        static std::map<uint32_t, uint64_t> times[2];
        return times[physical ? 1 : 0];
    }

    // COD3_QUARANTINE=ms: a freed range is not handed out again for that
    // long while anything else will do (the memory never handed out, then a
    // range freed long enough ago); only then a range freed lately, the one
    // freed longest ago first. The title walks some of what it frees at a
    // level's teardown after freeing it (a list whose nodes it has just
    // given back, a table of names in a block already returned), and with
    // the lowest free address handed out first that is exactly the memory
    // just freed: the walk finds the next load's data in it and follows it
    // anywhere. With the quarantine those reads find the old contents, as
    // they would on the console. Off by default: the mission restart still
    // comes back wrong with it, and it changes where everything is placed.
    uint64_t QuarantineMs()
    {
        static const uint64_t ms = []() { const char* t = getenv("COD3_QUARANTINE"); return t ? uint64_t(strtoull(t, nullptr, 10)) : 0ull; }();
        return ms;
    }

    // A freed region back on the list, joined with the ones either side.
    // Its pages are made inaccessible, as the console's are once freed: a
    // stale pointer into it faults, and the fault handler names the reader
    // (Kernel::FreedMemoryTouched) instead of the garbage being read.
    void Release(uint32_t address, uint32_t size, bool physical)
    {
        if (size == 0) return;
        WriteWatch::Forget(address, size);
        DWORD was = 0;
        VirtualProtect(Guest::Ptr(address), size, PAGE_NOACCESS, &was);
        std::map<uint32_t, uint32_t>& list = FreeList(physical);
        std::map<uint32_t, uint64_t>& times = FreedAt(physical);
        const uint64_t now = GetTickCount64();
        auto next = list.lower_bound(address);
        if (next != list.end() && next->first == address + size)
        {
            size += next->second;
            times.erase(next->first);
            next = list.erase(next);
        }
        if (next != list.begin())
        {
            auto previous = std::prev(next);
            if (previous->first + previous->second == address)
            {
                previous->second += size;
                times[previous->first] = now;
                return;
            }
        }
        list[address] = size;
        times[address] = now;
    }

    // A region out of the free range at 'it', the rest of the range left on
    // the list with the time it had.
    uint32_t TakeFrom(std::map<uint32_t, uint32_t>::iterator it, uint32_t start, uint32_t size, bool physical)
    {
        std::map<uint32_t, uint32_t>& list = FreeList(physical);
        std::map<uint32_t, uint64_t>& times = FreedAt(physical);
        const uint32_t rangeStart = it->first, rangeSize = it->second;
        const uint64_t freed = times[rangeStart];
        list.erase(it);
        times.erase(rangeStart);
        if (start > rangeStart) { list[rangeStart] = start - rangeStart; times[rangeStart] = freed; }
        if (start + size < rangeStart + rangeSize)
        {
            list[start + size] = rangeStart + rangeSize - (start + size);
            times[start + size] = freed;
        }
        Regions()[start] = { size, physical };
        WriteWatch::Forget(start, size);
        DWORD was = 0;
        VirtualProtect(Guest::Ptr(start), size, PAGE_READWRITE, &was);
        Kernel::Stats().bytesAllocated.fetch_add(size, std::memory_order_relaxed);
        // The first few reuses, for telling a reuse problem from another.
        static int announced = 0;
        if (announced++ < 8)
        {
            printf("memory: %s 0x%08X, %u KB, given out again from a free range of %u KB freed %llu ms before\n",
                physical ? "physical" : "virtual", start, size >> 10, rangeSize >> 10, (unsigned long long)(GetTickCount64() - freed));
            fflush(stdout);
        }
        return start;
    }

    // Where a region comes from, in order: a range freed long enough ago
    // (first fit), the memory never handed out yet, and only then a range
    // freed lately, the one freed longest ago first.
    uint32_t Take(uint32_t size, uint32_t alignment, bool physical)
    {
        // COD3_NOREUSE=1: the bump pointer alone, for telling a reuse
        // problem from another.
        static const bool noReuse = getenv("COD3_NOREUSE") != nullptr;
        std::map<uint32_t, uint32_t>& list = FreeList(physical);
        std::map<uint32_t, uint64_t>& times = FreedAt(physical);
        const uint64_t now = GetTickCount64();
        auto fits = [&](std::map<uint32_t, uint32_t>::iterator it, uint32_t& start) {
            const uint32_t rangeStart = it->first, rangeSize = it->second;
            start = Align(rangeStart, alignment);
            return !(start < rangeStart || start + size < start || start + size > rangeStart + rangeSize);
        };
        if (!noReuse)
        {
            for (auto it = list.begin(); it != list.end(); ++it)
            {
                uint32_t start;
                if (now - times[it->first] >= QuarantineMs() && fits(it, start)) return TakeFrom(it, start, size, physical);
            }
        }

        uint32_t& next = physical ? g_physicalNext : g_virtualNext;
        const uint32_t limit = physical ? Guest::PhysicalHeapLimit : Guest::VirtualHeapLimit;
        const uint32_t address = Align(next, alignment);
        if (!(address < next || address + size < address || address + size > limit))
        {
            next = address + size;
            Regions()[address] = { size, physical };
            Kernel::Stats().bytesAllocated.fetch_add(size, std::memory_order_relaxed);
            return address;
        }

        if (noReuse) return 0;
        auto oldest = list.end();
        uint32_t oldestStart = 0;
        for (auto it = list.begin(); it != list.end(); ++it)
        {
            uint32_t start;
            if (fits(it, start) && (oldest == list.end() || times[it->first] < times[oldest->first]))
            {
                oldest = it;
                oldestStart = start;
            }
        }
        if (oldest == list.end()) return 0;
        return TakeFrom(oldest, oldestStart, size, physical);
    }

    // Whether an address lies in a region handed out and not freed. The
    // pages of a freed region stay committed on the host, so this is what
    // tells a live region from a dead one.
    bool InLiveRegion(uint32_t address)
    {
        auto it = Regions().upper_bound(address);
        if (it == Regions().begin()) return false;
        --it;
        return address >= it->first && address - it->first < it->second.size;
    }

    // What is free, for the report: bytes and ranges.
    void FreeSpace(bool physical, uint64_t& bytes, size_t& ranges)
    {
        bytes = 0;
        for (const auto& entry : FreeList(physical)) bytes += entry.second;
        ranges = FreeList(physical).size();
    }
}

// Physical memory for this runtime's own use: the same allocator the title's
// physical allocations come from, so nothing here can land on top of one of
// those, committed and cleared.
uint32_t Guest::AllocatePhysical(uint32_t size)
{
    std::lock_guard<std::mutex> lock(g_allocatorMutex);
    const uint32_t address = Take(Align(size, LargePage), LargePage, true);
    if (address == 0) return 0;
    if (VirtualAlloc(Guest::Ptr(address), Align(size, LargePage), MEM_COMMIT, PAGE_READWRITE) != nullptr)
        memset(Guest::Ptr(address), 0, Align(size, LargePage));
    return address;
}

// NTSTATUS NtAllocateVirtualMemory(PVOID* base, SIZE_T* size, ULONG type,
//                                  ULONG protect, ULONG unknown)
PPC_FUNC(__imp__NtAllocateVirtualMemory)
{
    Kernel::CountImport("NtAllocateVirtualMemory");
    struct Refused
    {
        PPCContext& ctx;
        uint32_t out;
        ~Refused()
        {
            if (ctx.r3.u32 == 0) return;   // success is zero
            printf("memory: allocation refused, status 0x%08X, from 0x%08X\n",
                ctx.r3.u32, uint32_t(ctx.lr));
            fflush(stdout);
        }
    } refused{ ctx, 0 };
    std::lock_guard<std::mutex> lock(g_allocatorMutex);

    const uint32_t baseAddressPtr = ctx.r3.u32;
    const uint32_t sizePtr = ctx.r4.u32;
    const uint32_t allocationType = ctx.r5.u32;

    if (baseAddressPtr == 0 || sizePtr == 0)
    {
        ctx.r3.u32 = X_STATUS_INVALID_PARAMETER;
        return;
    }

    const uint32_t requested = Guest::Read32(base, baseAddressPtr);
    const uint32_t requestedSize = Guest::Read32(base, sizePtr);

    const uint32_t pageSize =
        (allocationType & (X_MEM_LARGE_PAGES | X_MEM_16MB_PAGES)) ? LargePage : SmallPage;
    const uint32_t size = Align(requestedSize, pageSize);
    if (size == 0)
    {
        ctx.r3.u32 = X_STATUS_INVALID_PARAMETER;
        return;
    }

    uint32_t address;
    if (requested != 0)
    {
        // The title picked an address. Everything is already reserved, so
        // honour it and record the region.
        address = requested & ~(pageSize - 1);
        Regions()[address] = { size, false };
    }
    else
    {
        address = Take(size, pageSize, false);
        if (address == 0)
        {
            fprintf(stderr, "NtAllocateVirtualMemory: out of guest address space "
                            "for %u bytes\n", size);
            ctx.r3.u32 = X_STATUS_NO_MEMORY;
            return;
        }
    }

    // MEM_COMMIT means the title expects to touch it now, and expects zeroes.
    // The fault handler would commit lazily, but zeroing here matches what the
    // console guarantees and turns a use-before-write into a predictable zero
    // rather than whatever the last user of the page left behind.
    if (allocationType & X_MEM_COMMIT)
    {
        if (VirtualAlloc(Guest::Ptr(address), size, MEM_COMMIT, PAGE_READWRITE) != nullptr)
            memset(Guest::Ptr(address), 0, size);
    }

    Guest::Write32(base, baseAddressPtr, address);
    Guest::Write32(base, sizePtr, size);
    ctx.r3.u32 = X_STATUS_SUCCESS;
}

// NTSTATUS NtFreeVirtualMemory(PVOID* base, SIZE_T* size, ULONG type, ULONG)
PPC_FUNC(__imp__NtFreeVirtualMemory)
{
    Kernel::CountImport("NtFreeVirtualMemory");
    std::lock_guard<std::mutex> lock(g_allocatorMutex);

    const uint32_t baseAddressPtr = ctx.r3.u32;
    const uint32_t sizePtr = ctx.r4.u32;
    const uint32_t freeType = ctx.r5.u32;

    if (baseAddressPtr == 0)
    {
        ctx.r3.u32 = X_STATUS_INVALID_PARAMETER;
        return;
    }

    const uint32_t address = Guest::Read32(base, baseAddressPtr);
    auto found = Regions().find(address);
    const uint32_t size = found != Regions().end() ? found->second.size : 0;

    if (freeType & X_MEM_RELEASE)
    {
        // Virtual address space is never handed back, so a release only
        // forgets the region. Decommitting would return the memory but also
        // make a stale guest pointer fault in the demand handler and be
        // silently recommitted; and reusing the range is unsafe, see above.
        if (found != Regions().end())
            Regions().erase(found);
    }
    else if ((freeType & X_MEM_DECOMMIT) && size != 0)
    {
        // Zeroing is what a decommit looks like from the guest's side, but it
        // is also the one place this runtime erases memory it was not asked to
        // erase if the recorded size is wrong.
        static std::atomic<int> announced{ 0 };
        if (announced.fetch_add(1) < 4)
        {
            printf("memory: decommit zeroes 0x%08X to 0x%08X\n",
                address, address + size);
            fflush(stdout);
        }
        memset(Guest::Ptr(address), 0, size);
    }

    if (sizePtr != 0)
        Guest::Write32(base, sizePtr, size);
    ctx.r3.u32 = X_STATUS_SUCCESS;
}

// PVOID MmAllocatePhysicalMemoryEx(ULONG flags, SIZE_T size, ULONG protect,
//                                  ULONG minAddress, ULONG maxAddress,
//                                  ULONG alignment)
PPC_FUNC(__imp__MmAllocatePhysicalMemoryEx)
{
    Kernel::CountImport("MmAllocatePhysicalMemoryEx");
    std::lock_guard<std::mutex> lock(g_allocatorMutex);

    const uint32_t size = Align(ctx.r4.u32, LargePage);
    uint32_t alignment = ctx.r8.u32;
    if (alignment == 0 || (alignment & (alignment - 1)) != 0)
        alignment = LargePage;

    const uint32_t reusedBefore = uint32_t(FreeList(true).size());
    const uint32_t address = Take(size, alignment, true);
    if (address != 0) Note(address < g_physicalNext - size || reusedBefore != FreeList(true).size() ? 'r' : 'a', address, size, uint32_t(ctx.lr));
    if (address == 0)
    {
        fprintf(stderr, "MmAllocatePhysicalMemoryEx: out of physical space for %u bytes\n", size);
        ctx.r3.u32 = 0;
        return;
    }

    if (VirtualAlloc(Guest::Ptr(address), size, MEM_COMMIT, PAGE_READWRITE) != nullptr)
        memset(Guest::Ptr(address), 0, size);

    // COD3_TRACEPHYS=1: every physical region handed out and freed, with
    // the caller, to compare a level's layout at its first load and at a
    // reload.
    static const bool tracePhysical = getenv("COD3_TRACEPHYS") != nullptr;
    if (tracePhysical)
        printf("phys: give 0x%08X..0x%08X (%u KB) align %u from %08X\n", address, address + size, size >> 10, alignment, uint32_t(ctx.lr));

    ctx.r3.u32 = address;
}

// VOID MmFreePhysicalMemory(ULONG type, PVOID address)
PPC_FUNC(__imp__MmFreePhysicalMemory)
{
    Kernel::CountImport("MmFreePhysicalMemory");
    std::lock_guard<std::mutex> lock(g_allocatorMutex);

    // Back on the free list, and the renderer told: what it uploaded from
    // there is not the title's any more.
    const uint32_t address = ctx.r4.u32;
    auto found = Regions().find(address);
    {
        static std::atomic<int> announced{ 0 };
        if (announced.fetch_add(1) < 8)
        {
            printf("memory: physical free of 0x%08X (%u KB), from 0x%08X\n",
                address, found != Regions().end() ? found->second.size >> 10 : 0u, uint32_t(ctx.lr));
            fflush(stdout);
        }
    }
    static const bool tracePhysical = getenv("COD3_TRACEPHYS") != nullptr;
    if (tracePhysical)
        printf("phys: free 0x%08X (%u KB) from %08X\n", address, found != Regions().end() ? found->second.size >> 10 : 0u, uint32_t(ctx.lr));
    if (found != Regions().end())
    {
        const uint32_t size = found->second.size;
        const bool physical = found->second.physical;
        Regions().erase(found);
        Note('f', address, size, uint32_t(ctx.lr));
        Release(address, size, physical);
        Render::MemoryFreed(address, size);
    }
}

bool Kernel::FreedMemoryTouched(uint32_t address)
{
    std::lock_guard<std::mutex> lock(g_allocatorMutex);
    const bool inHeap = (address >= Guest::PhysicalHeapBase && address < Guest::PhysicalHeapLimit) ||
                        (address >= Guest::VirtualHeapBase && address < Guest::VirtualHeapLimit);
    if (!inHeap || InLiveRegion(address)) return false;
    // The first few: whose the memory was, from the history.
    static int told = 0;
    if (told < 4)
    {
        told++;
        printf("memory: 0x%08X, what the regions holding it went through, oldest first:", address);
        for (const Event& event : History())
            if (address >= event.address && address - event.address < event.size)
                printf(" %s 0x%08X+%uK by %08X;", event.kind == 'f' ? "freed" : event.kind == 'r' ? "given again" : "given",
                    event.address, event.size >> 10, event.from);
        printf("\n");
    }
    // Let the access through, at the page: the run goes on, as it did when
    // freed memory kept its contents. A page that was never committed has
    // no protection to change, and saying it had been let through then sent
    // the access straight back to fault again, for ever: the hangs at a
    // level's teardown and reload were threads stopped on one load. Such a
    // page is left to the demand commit, which hands it out as zeros, as
    // the console hands out a fresh page.
    DWORD was = 0;
    return VirtualProtect(Guest::Ptr(address & ~0xFFFu), 0x1000, PAGE_READWRITE, &was) != FALSE;
}

void Kernel::ReportHeaps()
{
    std::lock_guard<std::mutex> lock(g_allocatorMutex);
    uint64_t physicalFree, virtualFree;
    size_t physicalRanges, virtualRanges;
    FreeSpace(true, physicalFree, physicalRanges);
    FreeSpace(false, virtualFree, virtualRanges);
    printf("heaps: physical %.1f of %.1f MB handed out, %.1f MB free in %zu ranges; virtual %.1f of %.1f MB, %.1f MB free in %zu ranges\n",
        (g_physicalNext - Guest::PhysicalHeapBase) / 1048576.0, (Guest::PhysicalHeapLimit - Guest::PhysicalHeapBase) / 1048576.0, physicalFree / 1048576.0, physicalRanges,
        (g_virtualNext - Guest::VirtualHeapBase) / 1048576.0, (Guest::VirtualHeapLimit - Guest::VirtualHeapBase) / 1048576.0, virtualFree / 1048576.0, virtualRanges);
}

// ULONG MmGetPhysicalAddress(PVOID address)
PPC_FUNC(__imp__MmGetPhysicalAddress)
{
    Kernel::CountImport("MmGetPhysicalAddress");
    // There is no MMU here and no GPU to hand an address to, so a guest
    // pointer is its own physical address. This has to change the moment
    // anything outside the CPU reads guest memory.
    ctx.r3.u32 = ctx.r3.u32;
}

// ULONG MmQueryAllocationSize(PVOID address)
PPC_FUNC(__imp__MmQueryAllocationSize)
{
    Kernel::CountImport("MmQueryAllocationSize");
    std::lock_guard<std::mutex> lock(g_allocatorMutex);

    auto found = Regions().find(ctx.r3.u32);
    ctx.r3.u32 = found != Regions().end() ? found->second.size : 0;
}

// ULONG MmQueryAddressProtect(PVOID address)
PPC_FUNC(__imp__MmQueryAddressProtect)
{
    Kernel::CountImport("MmQueryAddressProtect");
    // This has to tell the truth. The title walks the address space 64 KB at a
    // time asking about each block and stops when one comes back inaccessible,
    // so a runtime that answers "writable" for everything sends it round that
    // loop forever. The host already knows which pages are committed.
    const uint32_t address = ctx.r3.u32;

    MEMORY_BASIC_INFORMATION information{};
    if (VirtualQuery(Guest::Ptr(address), &information, sizeof(information)) == 0 ||
        information.State != MEM_COMMIT)
    {
        ctx.r3.u32 = 0;   // nothing mapped here
        return;
    }

    // Inside the heaps, a freed region's pages are still committed on the
    // host but are not the title's: the console would say they are not
    // mapped, and the film player's walk over its memory stops on that
    // answer. (With them reported writable it walked on and never came
    // back, at the second level's briefing.)
    const bool inHeap = (address >= Guest::PhysicalHeapBase && address < Guest::PhysicalHeapLimit) ||
                        (address >= Guest::VirtualHeapBase && address < Guest::VirtualHeapLimit);
    if (inHeap)
    {
        std::lock_guard<std::mutex> lock(g_allocatorMutex);
        if (!InLiveRegion(address))
        {
            static const bool logged = getenv("COD3_PROTECTLOG") != nullptr;
            static std::atomic<int> announced{ 0 };
            if (logged && announced.fetch_add(1) < 400)
            {
                printf("protect: 0x%08X -> 0 (freed), from 0x%08X\n", address, uint32_t(ctx.lr));
                fflush(stdout);
            }
            ctx.r3.u32 = 0;
            return;
        }
    }

    ctx.r3.u32 = 0x04;    // PAGE_READWRITE
    // COD3_PROTECTLOG=1: every ask, for seeing what the title walks.
    static const bool logged = getenv("COD3_PROTECTLOG") != nullptr;
    static std::atomic<int> announced{ 0 };
    if (logged && announced.fetch_add(1) < 400)
    {
        printf("protect: 0x%08X -> %u, from 0x%08X\n", address, ctx.r3.u32, uint32_t(ctx.lr));
        fflush(stdout);
    }
}
