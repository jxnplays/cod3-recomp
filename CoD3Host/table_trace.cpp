// A trace of the title's table of 2372-byte entries at *(0x82A2A2E0),
// counted at 0x82A2A2D8: what fills it at a level's load and what it holds
// when the level's first reader walks it. COD3_TRACETABLE=1.

#include "kernel.h"
#include "anim_heap_trace.h"
#include "probe.h"
#include "coroutines.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <Windows.h>

namespace
{
    constexpr uint32_t CountAddress = 0x82A2A2D8;
    constexpr uint32_t TableAddress = 0x82A2A2E0;

    bool Wanted()
    {
        static const bool wanted = getenv("COD3_TRACETABLE") != nullptr;
        return wanted;
    }

    bool Plausible(uint32_t address) { return address >= 0x10000 && address < 0xC0000000u; }

    std::string Printable(uint8_t* base, uint32_t address, int length)
    {
        if (!Plausible(address)) return "<bad pointer>";
        std::string text;
        for (int i = 0; i < length; i++)
        {
            const uint8_t c = Guest::Read8(base, address + i);
            if (c == 0) break;
            if (c >= 32 && c < 127) text += char(c);
            else { char hex[8]; snprintf(hex, sizeof hex, "\\x%02X", c); text += hex; }
        }
        return text;
    }

    void Entry(uint8_t* base, int index, const char* why)
    {
        const uint32_t table = Guest::Read32(base, TableAddress);
        if (!Plausible(table)) { printf("table: %s [%d] table %08X\n", why, index, table); return; }
        const uint32_t entry = Guest::Read32(base, table + index * 4);
        if (!Plausible(entry)) { printf("table: %s [%d] entry %08X\n", why, index, entry); return; }
        const uint32_t name = Guest::Read32(base, entry + 8);
        const uint32_t label = Guest::Read32(base, entry + 1440);
        printf("table: %s [%d] entry %08X index %u hash %08X name %08X \"%s\" +1440 %08X \"%s\" +180 %u\n",
            why, index, entry, Guest::Read32(base, entry), Guest::Read32(base, entry + 4),
            name, Printable(base, name, 40).c_str(), label, Printable(base, label, 40).c_str(),
            Guest::Read32(base, entry + 180));
    }
}

// The reset at a level's load: entry 0 made anew and the count set to 0.
extern "C" PPC_FUNC(__imp__sub_82483510);
PPC_FUNC(sub_82483510)
{
    if (Wanted())
    {
        printf("table: reset, os %lu, count %d table %08X\n", GetCurrentThreadId(),
            int32_t(Guest::Read32(base, CountAddress)), Guest::Read32(base, TableAddress));
        fflush(stdout);
    }
    __imp__sub_82483510(ctx, base);
    if (Wanted()) { Entry(base, 0, "after reset"); fflush(stdout); }
}

// One registration, sub_8247CBF8(name, ...): the count goes up and the new
// entry is made and filled.
extern "C" PPC_FUNC(__imp__sub_8247CBF8);
PPC_FUNC(sub_8247CBF8)
{
    const uint32_t name = ctx.r3.u32;
    const uint32_t lr = uint32_t(ctx.lr);
    if (Wanted())
    {
        printf("table: register \"%s\" from %08X, count %d\n", Printable(base, name, 60).c_str(), lr,
            int32_t(Guest::Read32(base, CountAddress)));
        fflush(stdout);
    }
    __imp__sub_8247CBF8(ctx, base);
    if (Wanted())
    {
        const int count = int32_t(Guest::Read32(base, CountAddress));
        printf("table: registered, returned %08X, count %d\n", ctx.r3.u32, count);
        if (count > 0) Entry(base, count, "new");
        fflush(stdout);
    }
}

// The level's first walk of the table, sub_824E03F0(cache, ...): it looks up
// each entry's name at +1440 once, when the cache's word 7 is still 0.
extern "C" PPC_FUNC(__imp__sub_824E03F0);
PPC_FUNC(sub_824E03F0)
{
    static std::atomic<int> s_walks{ 0 };
    if (Wanted() && Plausible(ctx.r3.u32) && Guest::Read32(base, ctx.r3.u32 + 28) == 0 && s_walks.fetch_add(1) < 40)
    {
        const int count = int32_t(Guest::Read32(base, CountAddress));
        printf("table: walk by %08X for %08X, os %lu, count %d\n", ctx.r3.u32, uint32_t(ctx.lr), GetCurrentThreadId(), count);
        for (int i = 1; i <= count && i < 64; i++) Entry(base, i, "walk");
        fflush(stdout);
    }
    __imp__sub_824E03F0(ctx, base);
}

// The game module's init, sub_8256D978(level time, seed, restart, ...): which
// way a level's start and its restart after a death go through it.
extern "C" PPC_FUNC(__imp__sub_8256D978);
PPC_FUNC(sub_8256D978)
{
    if (Wanted())
    {
        printf("table: game init r3 %08X r4 %08X r5 %08X r6 %08X from %08X, count %d\n", ctx.r3.u32, ctx.r4.u32, ctx.r5.u32,
            ctx.r6.u32, uint32_t(ctx.lr), int32_t(Guest::Read32(base, CountAddress)));
        fflush(stdout);
    }
    AnimHeapTrace::LevelStart(base);
    ProbeRun(ctx, base);
    __imp__sub_8256D978(ctx, base);
}

// The dispatcher in front of it, sub_8256E920(message, ...): every message
// the engine sends the game module around a level's end and start.
extern "C" PPC_FUNC(__imp__sub_8256E920);
PPC_FUNC(sub_8256E920)
{
    static uint32_t counts[32] = {};
    const uint32_t message = ctx.r3.u32;
    // The game module's shutdown ends a generation of script threads.
    if (message == 1) Coroutines::NewGeneration();
    if (Wanted() && message < 32 && message != 2 && counts[message]++ < 6)
    {
        printf("table: game message %u (%08X %08X %08X) from %08X\n", message, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, uint32_t(ctx.lr));
        fflush(stdout);
    }
    __imp__sub_8256E920(ctx, base);
}
