#include "write_watch.h"
#include "kernel.h"
#include "settings.h"

#include <atomic>
#include <cstdlib>
#include <mutex>

#include <Windows.h>

namespace
{
    constexpr uint32_t PageShift = 12;
    constexpr uint32_t WindowSize = 0x20000000u;           // 512 MB of physical memory
    constexpr uint32_t Pages = WindowSize >> PageShift;
    constexpr uint32_t Windows[] = { 0xA0000000u, 0xC0000000u, 0xE0000000u };

    std::atomic<uint8_t> g_armed[Pages];
    std::atomic<uint64_t> g_written[Pages];   // the sequence the page was last noted written at
    std::atomic<uint64_t> g_sequence{ 1 };
    std::mutex g_lock;                        // arming and disarming

    bool Protect(uint32_t firstPage, uint32_t count, DWORD protection)
    {
        bool all = true;
        for (const uint32_t window : Windows)
        {
            DWORD was = 0;
            if (!VirtualProtect(Guest::Base + window + (size_t(firstPage) << PageShift),
                                size_t(count) << PageShift, protection, &was))
                all = false;
        }
        return all;
    }

    // The armed pages of [first, end) disarmed and noted written now
    // (g_lock held), a run of them at a time.
    void Disarm(uint32_t first, uint32_t end)
    {
        uint32_t page = first;
        while (page < end)
        {
            if (g_armed[page].load(std::memory_order_relaxed) == 0) { page++; continue; }
            uint32_t last = page;
            while (last < end && g_armed[last].load(std::memory_order_relaxed) != 0) last++;
            const uint64_t now = g_sequence.fetch_add(1) + 1;
            for (uint32_t p = page; p < last; p++)
            {
                g_written[p].store(now, std::memory_order_relaxed);
                g_armed[p].store(0, std::memory_order_release);
            }
            Protect(page, last - page, PAGE_READWRITE);
            page = last;
        }
    }

    // The section's pages a guest address range covers, through any window
    // (or a physical address below them). False when it lies outside.
    bool PagesOf(uint32_t address, uint32_t bytes, uint32_t& first, uint32_t& end)
    {
        if (bytes == 0) return false;
        uint32_t offset = address;
        if (address >= Windows[0]) offset = address & (WindowSize - 1);
        else if (address >= WindowSize) return false;   // the virtual heap and below: not physical memory
        const uint64_t last = uint64_t(offset) + bytes - 1;
        if (last >= WindowSize) return false;
        first = offset >> PageShift;
        end = uint32_t(last >> PageShift) + 1;
        return true;
    }
}

// COD3_WRITEWATCH=0|1, else the settings' "Watch memory writes". Read once,
// after the settings are loaded (the first caller is the guest's own start).
bool WriteWatch::Enabled()
{
    static const bool enabled = []() {
        const char* t = getenv("COD3_WRITEWATCH");
        if (t != nullptr && t[0] != 0) return t[0] != '0';
        return Settings::Get().watchWrites;
    }();
    return enabled;
}

uint64_t WriteWatch::Sequence() { return g_sequence.load(std::memory_order_acquire); }

bool WriteWatch::Arm(uint32_t physical, uint32_t bytes)
{
    if (!Enabled()) return false;
    uint32_t first, end;
    if (!PagesOf(physical & (WindowSize - 1), bytes, first, end)) return false;
    std::lock_guard<std::mutex> lock(g_lock);
    uint32_t page = first;
    while (page < end)
    {
        if (g_armed[page].load(std::memory_order_relaxed) != 0) { page++; continue; }
        uint32_t last = page;
        while (last < end && g_armed[last].load(std::memory_order_relaxed) == 0) last++;
        // Armed before they are protected: a write that faults in between
        // finds them armed and waits for the lock.
        for (uint32_t p = page; p < last; p++) g_armed[p].store(1, std::memory_order_release);
        if (!Protect(page, last - page, PAGE_READONLY))
        {
            for (uint32_t p = page; p < last; p++)
            {
                g_written[p].store(g_sequence.fetch_add(1) + 1, std::memory_order_relaxed);
                g_armed[p].store(0, std::memory_order_release);
            }
            Protect(page, last - page, PAGE_READWRITE);
            return false;
        }
        page = last;
    }
    return true;
}

bool WriteWatch::Clean(uint32_t physical, uint32_t bytes, uint64_t since)
{
    uint32_t first, end;
    if (!PagesOf(physical & (WindowSize - 1), bytes, first, end)) return false;
    for (uint32_t page = first; page < end; page++)
        if (g_armed[page].load(std::memory_order_acquire) == 0 || g_written[page].load(std::memory_order_relaxed) > since)
            return false;
    return true;
}

void WriteWatch::Forget(uint32_t guestAddress, uint32_t bytes)
{
    if (!Enabled()) return;
    uint32_t first, end;
    if (!PagesOf(guestAddress, bytes, first, end)) return;
    std::lock_guard<std::mutex> lock(g_lock);
    Disarm(first, end);
}

bool WriteWatch::HandleFault(const void* hostAddress)
{
    if (!Enabled()) return false;
    const uintptr_t at = uintptr_t(hostAddress) - uintptr_t(Guest::Base);
    uint32_t page = Pages;
    for (const uint32_t window : Windows)
        if (at >= window && at < uint64_t(window) + WindowSize) page = uint32_t((at - window) >> PageShift);
    if (page >= Pages) return false;
    {
        std::lock_guard<std::mutex> lock(g_lock);
        if (g_armed[page].load(std::memory_order_acquire) != 0)
        {
            Disarm(page, page + 1);
            return true;
        }
    }
    // Not armed: another thread disarmed it a moment ago, and the write goes
    // through when it is made again; or the page is protected for another
    // reason (freed memory), which is the other handlers' business.
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(hostAddress, &info, sizeof(info)) == 0) return false;
    return info.State == MEM_COMMIT && info.Protect == PAGE_READWRITE;
}
