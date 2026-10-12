// The thread pointer.
//
// On this console r13 points at a per thread block, and guest code reads its
// own thread state straight out of it rather than asking the kernel. Two
// idioms in the title's own code name the layout beyond any doubt: reading
// r13+0x100 then +0x14C is how it gets its thread id, and writing r13+0x100
// then +0x160 is how it records its last error.
//
// This runtime left r13 as zero, so every one of those reads went to guest
// address zero and came back as zero. Every thread believed it was thread
// zero, and every timestamp read this way was the same instant forever. That
// last part is what mattered: the title's loading watchdog waits until either
// the thing it is watching changes or five seconds pass, and with a clock that
// never advances, five seconds never pass.

#include "kernel.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include <Windows.h>

namespace
{
    // One page a thread, carved into the three things that live in it.
    constexpr uint32_t BlockSize = 0x1000;
    constexpr uint32_t ThreadOffset = 0x400;   // the thread object
    constexpr uint32_t TlsOffset = 0x600;      // thread local storage slots

    // Fields inside those, at the offsets the title's own code uses.
    constexpr uint32_t PcrTlsPointer = 0x000;
    constexpr uint32_t PcrCurrentThread = 0x100;

    // Which hardware thread this one runs on.
    //
    // The console has six, and guest code reads the number straight out of the
    // block rather than calling the kernel. This title uses it as an index:
    // each worker registers itself in a table at that slot, and a barrier waits
    // until every participating slot has set its own byte. With the number left
    // at zero every thread wrote the same byte, the barrier could never fill,
    // and the thread that had to acknowledge the loading request span there
    // forever.
    constexpr uint32_t PcrProcessorNumber = 0x10C;
    constexpr uint32_t HardwareThreads = 6;
    constexpr uint32_t ThreadTimestamp = 0x058;
    constexpr uint32_t ThreadId = 0x14C;

    std::mutex g_mutex;
    std::vector<uint32_t> g_threadObjects;   // guest addresses, one a thread
    uint32_t g_nextProcessor = 0;
    std::atomic<bool> g_clockRunning{ false };
    std::thread g_clockThread;
    std::chrono::steady_clock::time_point g_start;

    // A region of its own, below everything else the runtime hands out.
    //
    // The first attempt took these blocks from the stack allocator, which put
    // the very first one at the top of the main thread's own stack. The stack
    // grows down over it, so the thread pointer was overwritten within
    // microseconds and read back as whatever the title had last pushed.
    // Eight megabytes each, in the gap below the guest heap. A megabyte was
    // enough while the title was stuck on its loading screen and ran nine
    // threads; once it started loading properly it ran out, handed back a null
    // thread pointer, and the guest read through it until an address landed
    // outside the address space altogether.
    constexpr uint32_t RegionBase = 0x3F000000;
    constexpr uint32_t RegionSize = 0x00800000;

    // A second region, for the dispatcher headers of objects the title holds
    // handles to.
    constexpr uint32_t ObjectRegionBase = 0x3F800000;
    constexpr uint32_t ObjectRegionSize = 0x00800000;
    uint32_t g_nextObject = ObjectRegionBase;

    uint32_t g_next = RegionBase;

    std::vector<uint32_t> g_freeBlocks;

    uint32_t Allocate()
    {
        static std::mutex mutex;
        std::lock_guard<std::mutex> lock(mutex);

        // A thread that has ended gives its block back, so a title that starts
        // and stops many threads does not run the region dry.
        if (!g_freeBlocks.empty())
        {
            const uint32_t reused = g_freeBlocks.back();
            g_freeBlocks.pop_back();
            memset(Guest::Ptr(reused), 0, BlockSize);
            return reused;
        }

        if (g_next + BlockSize > RegionBase + RegionSize)
        {
            static bool reported = false;
            if (!reported)
            {
                reported = true;
                fprintf(stderr, "thread: the thread pointer region is full after "
                                "%u blocks\n", (g_next - RegionBase) / BlockSize);
            }
            return 0;
        }

        const uint32_t block = g_next;
        g_next += BlockSize;

        // Sixty four kilobytes at a time, aligned down, which is what the
        // fault handler commits in. Committing a single page inside one of
        // those left the rest of it in a state the handler then failed to
        // commit, and every access to it was reported as a fault this runtime
        // could not satisfy.
        const uint32_t region = block & ~0xFFFFu;
        if (VirtualAlloc(Guest::Ptr(region), 0x10000, MEM_COMMIT,
                         PAGE_READWRITE) == nullptr)
            return 0;
        memset(Guest::Ptr(block), 0, BlockSize);
        return block;
    }

    uint32_t Milliseconds()
    {
        const auto now = std::chrono::steady_clock::now();
        return uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(
            now - g_start).count());
    }

    // The clock every thread reads. Writing it once a millisecond costs
    // nothing and is finer than anything the title measures.
    std::atomic<uint32_t> g_timeStampBundle{ 0 };

    // The time since 1601 in hundreds of nanoseconds, which is how the
    // console's system time is expressed.
    uint64_t SystemTimeNow()
    {
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        return (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    }

    void ClockThread()
    {
        while (g_clockRunning.load(std::memory_order_acquire))
        {
            const uint32_t now = Milliseconds();

            // KeTimeStampBundle: interrupt time and system time in hundreds
            // of nanoseconds, then the millisecond tick count. Titles read
            // the tick count directly rather than asking the kernel.
            if (const uint32_t bundle = g_timeStampBundle.load())
            {
                const uint64_t system = SystemTimeNow();
                Guest::Write64(Guest::Base, bundle + 0, uint64_t(now) * 10000ull);
                Guest::Write64(Guest::Base, bundle + 8, system);
                Guest::Write32(Guest::Base, bundle + 16, now);
            }
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                for (uint32_t object : g_threadObjects)
                    Guest::Write32(Guest::Base, object + ThreadTimestamp, now);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
}

uint32_t Guest::CreateThreadPointer(uint32_t threadId, int processor)
{
    const uint32_t block = Allocate();
    if (block == 0)
    {
        fprintf(stderr, "thread: no room for a thread pointer block\n");
        return 0;
    }

    const uint32_t object = block + ThreadOffset;

    // A thread the title pinned keeps the number it asked for. One that did
    // not is handed the next free number, which only has to be distinct.
    uint32_t number = 0;
    if (processor >= 0 && processor < int(HardwareThreads))
    {
        number = uint32_t(processor);
    }
    else
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        number = g_nextProcessor;
        g_nextProcessor = (g_nextProcessor + 1) % HardwareThreads;
    }

    Write32(Base, block + PcrTlsPointer, block + TlsOffset);
    Write32(Base, block + PcrCurrentThread, object);
    *(Base + block + PcrProcessorNumber) = uint8_t(number);
    Write32(Base, object + ThreadId, threadId);

    // The timestamp starts at the current time, not at zero. The clock
    // thread brings it up within a millisecond, but a thread that reads it
    // before then sees zero, and the graphics driver's hang check on a
    // thread that has just started subtracts its last progress time from
    // it, gets a huge number, and declares the GPU hung the moment the
    // level loader starts.
    Write32(Base, object + ThreadTimestamp,
        g_threadObjects.empty() ? 0 : Milliseconds());

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_threadObjects.empty()) g_start = std::chrono::steady_clock::now();
        g_threadObjects.push_back(object);
    }

    bool expected = false;
    if (g_clockRunning.compare_exchange_strong(expected, true))
        g_clockThread = std::thread(ClockThread);

    return block;
}

void Guest::StopThreadClock()
{
    if (g_clockRunning.exchange(false) && g_clockThread.joinable())
        g_clockThread.join();
}

uint32_t Guest::AllocateKernelObject(uint32_t size)
{
    static std::mutex mutex;
    std::lock_guard<std::mutex> lock(mutex);

    size = (size + 0xF) & ~0xFu;
    if (g_nextObject + size > ObjectRegionBase + ObjectRegionSize)
    {
        static bool reported = false;
        if (!reported)
        {
            reported = true;
            fprintf(stderr, "object: the kernel object region is full\n");
        }
        return 0;
    }

    const uint32_t address = g_nextObject;
    g_nextObject += size;

    // Committed a page at a time; the region is reserved with everything else.
    const uint32_t region = address & ~0xFFFFu;
    VirtualAlloc(Guest::Ptr(region), 0x10000, MEM_COMMIT, PAGE_READWRITE);
    memset(Guest::Ptr(address), 0, size);
    return address;
}

void Guest::ReleaseThreadPointer(uint32_t block)
{
    if (block < RegionBase || block >= RegionBase + RegionSize) return;

    const uint32_t object = block + ThreadOffset;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (size_t i = 0; i < g_threadObjects.size(); i++)
        {
            if (g_threadObjects[i] == object)
            {
                g_threadObjects.erase(g_threadObjects.begin() + i);
                break;
            }
        }
    }

    static std::mutex mutex;
    std::lock_guard<std::mutex> lock(mutex);
    g_freeBlocks.push_back(block);
}

void Guest::SetProcessor(const PPCContext& ctx, int processor)
{
    if (ctx.r13.u32 == 0 || processor < 0 || processor >= int(HardwareThreads)) return;
    *(Base + ctx.r13.u32 + PcrProcessorNumber) = uint8_t(processor);
}

int Guest::CurrentProcessor(const PPCContext& ctx)
{
    if (ctx.r13.u32 == 0) return 0;
    return int(*(Base + ctx.r13.u32 + PcrProcessorNumber));
}

void Guest::SetTimeStampBundle(uint32_t address)
{
    g_timeStampBundle.store(address);
}
