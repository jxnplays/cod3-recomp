#include "kernel.h"
#include "timeline.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <Windows.h>

namespace
{
    struct Entry
    {
        int64_t nanoseconds;
        uint32_t thread;
        const char* what;
        uint32_t a, b;
    };
    constexpr uint32_t Capacity = 60000;   // the last few seconds: the kernel's waits and sets are many
    Entry g_entries[Capacity];
    std::atomic<uint32_t> g_count{ 0 };
    std::atomic<bool> g_printed{ false };
    int64_t g_start = 0;

    int64_t Now()
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }
}

bool Timeline::OnStallOnly()
{
    static const bool stall = []() { const char* text = getenv("COD3_TIMELINE"); return text != nullptr && strcmp(text, "stall") == 0; }();
    return stall;
}

bool Timeline::Enabled()
{
    static const bool enabled = []() {
        const char* text = getenv("COD3_TIMELINE");
        return text != nullptr && text[0] != 0 && text[0] != '0';
    }();
    return enabled;
}

void Timeline::Mark(const char* what, uint32_t a, uint32_t b)
{
    if (!Enabled()) return;
    // The level, once its loading is over: eight seconds after its files
    // started opening.
    static std::atomic<int64_t> levelSince{ 0 };
    if (Kernel::Stats().filesOpened.load(std::memory_order_relaxed) < 40) return;
    int64_t since = levelSince.load(std::memory_order_relaxed);
    if (since == 0)
    {
        int64_t expected = 0;
        levelSince.compare_exchange_strong(expected, Now());
        return;
    }
    const int64_t now = Now();
    if (now - since < 8000000000ll) return;
    if (g_printed.load(std::memory_order_relaxed)) return;
    // A ring: the last moments before whatever asks for the report, which
    // is the first wait the command processor abandons, or the end.
    const uint32_t index = g_count.fetch_add(1, std::memory_order_relaxed);
    if (index == 0) g_start = now;
    g_entries[index % Capacity] = { now, GetCurrentThreadId(), what, a, b };
}

void Timeline::Report()
{
    if (!Enabled() || g_printed.exchange(true)) return;
    const uint32_t count = g_count.load();
    const uint32_t held = count < Capacity ? count : Capacity;
    if (!OnStallOnly())
    {
        printf("timeline: the last %u moments, microseconds from the first recorded, os thread, what, values\n", held);
        for (uint32_t i = 0; i < held; i++)
        {
            const Entry& e = g_entries[(count - held + i) % Capacity];
            printf("t %10.1f  os %-6u %-14s %08X %08X\n", (e.nanoseconds - g_start) / 1000.0, e.thread, e.what, e.a, e.b);
        }
        fflush(stdout);
        return;
    }
    // At a stall, the GPU pipeline alone: the kernel's waits and sets are
    // most of what is recorded (the sound thread's alone fill thousands of
    // entries a second) and would push the handful that matter out of any
    // window. The last three thousand of the rest.
    auto kernelMoment = [](const char* what) {
        return strcmp(what, "ke wait") == 0 || strcmp(what, "ke waited") == 0 ||
               strcmp(what, "set event") == 0 || strcmp(what, "unblocked") == 0;
    };
    uint32_t wanted = 0, first = held;
    for (uint32_t i = held; i > 0; i--)
    {
        if (kernelMoment(g_entries[(count - held + i - 1) % Capacity].what)) continue;
        first = i - 1;
        if (++wanted == 3000) break;
    }
    printf("timeline: the last %u moments of the GPU pipeline (of %u kept), microseconds, os thread, what, values\n", wanted, held);
    for (uint32_t i = first; i < held; i++)
    {
        const Entry& e = g_entries[(count - held + i) % Capacity];
        if (kernelMoment(e.what)) continue;
        printf("t %10.1f  os %-6u %-14s %08X %08X\n", (e.nanoseconds - g_start) / 1000.0, e.thread, e.what, e.a, e.b);
    }
    fflush(stdout);
}
