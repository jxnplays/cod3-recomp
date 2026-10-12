// A trace of the title's indirect buffer pool, for the stall at the start
// of a level: COD3_TRACEPOOL=1.
//
// The title carves its command buffers out of one pool, in segments, and
// before it reuses a segment it waits for the GPU's last fence to have
// passed it. Every kick appends such a fence, but a recorded command buffer
// (begun by sub_82302DB0, ended by sub_82302E88) is kicked without one, and
// a lap of the pool spent on recordings leaves nothing for the wait to see.
// This prints each of those steps with the pool's state, so the sequence
// that ends in the wait can be read off.

#include "kernel.h"
#include "scheduler.h"
#include "pool_trace.h"
#include "timeline.h"
#include "native_state.h"

#include <chrono>

#include <atomic>
#include <cstdio>
#include <cstdlib>

#include <Windows.h>

namespace
{
    bool Enabled()
    {
        static const bool enabled = []() {
            const char* text = getenv("COD3_TRACEPOOL");
            return text != nullptr && text[0] != 0 && text[0] != '0';
        }();
        return enabled;
    }

    std::atomic<int> g_lines{ 0 };
    constexpr int LineLimit = 4000;

    void Line(const char* what, uint32_t device, uint8_t* base, uint32_t a, uint32_t b)
    {
        // From the level load on: the title screen alone would use up the
        // line budget in seconds. COD3_TRACEPOOL=N starts it at the Nth
        // file opened instead (a second level's load is past eighty).
        static const long from = []() { const char* t = getenv("COD3_TRACEPOOL"); const long v = t ? strtol(t, nullptr, 10) : 0; return v > 1 ? v : 40L; }();
        if (!Enabled() || Kernel::Stats().filesOpened.load(std::memory_order_relaxed) < uint64_t(from)) return;
        if (g_lines.fetch_add(1) >= LineLimit) return;
        const uint32_t block = Guest::Read32(base, device + 10768);
        printf("pool: %-9s os %-5lu at %08X lap %u segment %08X.. next %08X recording %08X/%u "
               "gpu word %08X counter %u owner %u pending %u/%u flags %02X%02X  [%08X %08X]\n",
            what, GetCurrentThreadId(),
            Guest::Read32(base, device + 40), Guest::Read32(base, device + 13500),
            Guest::Read32(base, device + 13508), Guest::Read32(base, device + 13496),
            Guest::Read32(base, device + 12944), Guest::Read32(base, device + 12948),
            block ? Guest::Read32(base, block + 4) : 0, Guest::Read32(base, device + 10780),
            Guest::Read32(base, device + 10760),
            Guest::Read32(base, device + 10864), Guest::Read32(base, device + 10868),
            Guest::Read8(base, device + 10808), Guest::Read8(base, device + 10809),
            a, b);
    }
}

// sub_822EC168(wait): one turn of the title's wait for the GPU, a spin of
// its own with no kernel call in it. The thread that would satisfy the
// wait shares this thread's hardware thread, so the wait must stand aside
// for it: the console's scheduler would preempt here, this runtime's only
// hands over at kernel calls, and a spinning thread that never made one
// held its hardware thread until the two second timeout let the other
// run.
// With the decompiled code (COD3_HAVE_DECOMP) the turn itself is its
// decompiled body, decomp_sub_822EC168.
#ifdef COD3_HAVE_DECOMP
PPC_FUNC(decomp_sub_822EC168);
#define COD3_WAIT_TURN decomp_sub_822EC168
#else
extern "C" PPC_FUNC(__imp__sub_822EC168);
#define COD3_WAIT_TURN __imp__sub_822EC168
#endif
PPC_FUNC(sub_822EC168)
{
    Scheduler::Checkpoint();
    COD3_WAIT_TURN(ctx, base);
}

// How the title's GPU pipeline spends its time, once a second: how long
// the workers wait for the GPU, how often the replaying thread runs and
// for how long, and how many buffers reach the ring. This is what says
// which of them is the frame's bottleneck.
namespace
{
    std::atomic<uint64_t> g_waitNanoseconds{ 0 }, g_waitCalls{ 0 };
    std::atomic<uint64_t> g_replayNanoseconds{ 0 }, g_replayRuns{ 0 };
    std::atomic<uint64_t> g_ringWrites{ 0 }, g_ringBuffers{ 0 };
    std::atomic<uint64_t> g_kicks{ 0 }, g_recordings{ 0 };
    int64_t Now()
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }
}

void PoolTrace::Report()
{
    static uint64_t lastWait = 0, lastReplay = 0, lastRuns = 0, lastRing = 0, lastBuffers = 0, lastKicks = 0, lastRecordings = 0;
    const uint64_t wait = g_waitNanoseconds.load(), replay = g_replayNanoseconds.load(), runs = g_replayRuns.load();
    const uint64_t ring = g_ringWrites.load(), buffers = g_ringBuffers.load(), kicks = g_kicks.load(), recordings = g_recordings.load();
    printf("pipeline: workers waited %.2f s for the GPU, the replay ran %llu times for %.2f s, "
           "%llu ring writes of %llu buffers, %llu kicks, %llu recordings\n",
        (wait - lastWait) / 1e9, (unsigned long long)(runs - lastRuns), (replay - lastReplay) / 1e9,
        (unsigned long long)(ring - lastRing), (unsigned long long)(buffers - lastBuffers),
        (unsigned long long)(kicks - lastKicks), (unsigned long long)(recordings - lastRecordings));
    lastWait = wait; lastReplay = replay; lastRuns = runs; lastRing = ring; lastBuffers = buffers;
    lastKicks = kicks; lastRecordings = recordings;
}

// The wait for the GPU (sub_822F16A0, wrapped in kernel_video.cpp) reports
// how long it took here.
void PoolTrace::Waited(uint64_t nanoseconds)
{
    g_waitNanoseconds.fetch_add(nanoseconds, std::memory_order_relaxed);
    g_waitCalls.fetch_add(1, std::memory_order_relaxed);
}

// sub_82302A90(queue): the replaying thread's run over its queue, timed.
// With the decompiled code (COD3_HAVE_DECOMP) the run is its decompiled
// body, decomp_sub_82302A90.
extern "C" PPC_FUNC(__imp__sub_82302A90);
#ifdef COD3_HAVE_DECOMP
PPC_FUNC(decomp_sub_82302A90);
#define COD3_REPLAY_RUN decomp_sub_82302A90
#else
#define COD3_REPLAY_RUN __imp__sub_82302A90
#endif
PPC_FUNC(sub_82302A90)
{
    const int64_t started = Now();
    Timeline::Mark("replay", ctx.r3.u32, ctx.r4.u32);
    COD3_REPLAY_RUN(ctx, base);
    Timeline::Mark("replayed", ctx.r3.u32);
    g_replayNanoseconds.fetch_add(uint64_t(Now() - started), std::memory_order_relaxed);
    g_replayRuns.fetch_add(1, std::memory_order_relaxed);
}

// sub_822F1E68(device, buffers, count): buffers written to the ring. With
// the decompiled code (COD3_HAVE_DECOMP) the write is its decompiled body.
#ifdef COD3_HAVE_DECOMP
PPC_FUNC(decomp_sub_822F1E68);
#define COD3_RING_WRITE decomp_sub_822F1E68
#else
extern "C" PPC_FUNC(__imp__sub_822F1E68);
#define COD3_RING_WRITE __imp__sub_822F1E68
#endif
PPC_FUNC(sub_822F1E68)
{
    g_ringWrites.fetch_add(1, std::memory_order_relaxed);
    g_ringBuffers.fetch_add(ctx.r5.u32, std::memory_order_relaxed);
    Timeline::Mark("ring write", ctx.r4.u32, ctx.r5.u32);
    COD3_RING_WRITE(ctx, base);
    Timeline::Mark("ring written", ctx.r4.u32, ctx.r5.u32);
}

// sub_822F2818(device): the current segment is kicked to the GPU.
extern "C" PPC_FUNC(__imp__sub_822F2818);
// With the decompiled code (COD3_HAVE_DECOMP) the kick is its decompiled
// body, decomp_sub_822F2818.
#ifdef COD3_HAVE_DECOMP
PPC_FUNC(decomp_sub_822F2818);
#define COD3_KICK decomp_sub_822F2818
#else
#define COD3_KICK __imp__sub_822F2818
#endif
PPC_FUNC(sub_822F2818)
{
    const uint32_t device = ctx.r3.u32;
    g_kicks.fetch_add(1, std::memory_order_relaxed);
    Timeline::Mark("kick", Guest::Read32(base, device + 40), uint32_t(ctx.lr));
    Line("kick", device, base, uint32_t(ctx.lr), 0);
    NativeState::Kicking(base, device);
    COD3_KICK(ctx, base);
    NativeState::Kicked(base, device);
}

// sub_822F1B78(device, flags, &size): a segment is taken from the pool.
extern "C" PPC_FUNC(__imp__sub_822F1B78);
// With the decompiled code (COD3_HAVE_DECOMP) the pool allocation is its decompiled
// body, decomp_sub_822F1B78.
#ifdef COD3_HAVE_DECOMP
PPC_FUNC(decomp_sub_822F1B78);
#define COD3_POOL_TAKE decomp_sub_822F1B78
#else
#define COD3_POOL_TAKE __imp__sub_822F1B78
#endif
PPC_FUNC(sub_822F1B78)
{
    const uint32_t device = ctx.r3.u32;
    const uint32_t flags = ctx.r4.u32;
    const uint32_t sizeAt = ctx.r5.u32;
    const uint32_t wanted = Guest::Read32(base, sizeAt);
    Line("alloc", device, base, flags, wanted);
    Timeline::Mark("pool alloc", flags, wanted);
    COD3_POOL_TAKE(ctx, base);
    Line("allocated", device, base, ctx.r3.u32, Guest::Read32(base, sizeAt));
    Timeline::Mark("pool got", ctx.r3.u32, Guest::Read32(base, sizeAt));
}

// sub_822F22C0(device, end, ib, dwords, count, list): an indirect buffer
// goes to the ring, or to the list when submissions are pending.
// A decompiled one (COD3_HAVE_DECOMP) takes its place: no "submit" lines then.
#ifndef COD3_HAVE_DECOMP
extern "C" PPC_FUNC(__imp__sub_822F22C0);
PPC_FUNC(sub_822F22C0)
{
    const uint32_t device = ctx.r3.u32;
    const uint32_t dwords = ctx.r6.u32;
    const uint32_t count = ctx.r7.u32;
    Line("submit", device, base, ctx.r5.u32, (count << 28) | dwords);
    Timeline::Mark("submit", ctx.r5.u32, (count << 28) | dwords);
    if (count != 0 && Enabled() && dwords <= 64 &&
        Kernel::Stats().filesOpened.load(std::memory_order_relaxed) >= 40)
    {
        // The buffer of a counted submission, in full: what the title
        // expects the GPU to do for it.
        printf("pool:   counted buffer at physical %08X:", ctx.r5.u32);
        for (uint32_t i = 0; i < dwords; i++)
            printf(" %08X", Guest::Read32(base, Guest::PhysicalAlias(ctx.r5.u32) + i * 4));
        printf("\n");
    }
    __imp__sub_822F22C0(ctx, base);
}
#endif

// sub_82302DB0(device) and sub_82302E88(device, ...): a recording begins and ends.
extern "C" PPC_FUNC(__imp__sub_82302DB0);
// With the decompiled code (COD3_HAVE_DECOMP) the beginning is its
// decompiled body, decomp_sub_82302DB0.
#ifdef COD3_HAVE_DECOMP
PPC_FUNC(decomp_sub_82302DB0);
#define COD3_RECORD_BEGIN decomp_sub_82302DB0
#else
#define COD3_RECORD_BEGIN __imp__sub_82302DB0
#endif
PPC_FUNC(sub_82302DB0)
{
    const uint32_t device = ctx.r3.u32;
    g_recordings.fetch_add(1, std::memory_order_relaxed);
    Timeline::Mark("record", uint32_t(ctx.lr));
    Line("record", device, base, uint32_t(ctx.lr), 0);
    COD3_RECORD_BEGIN(ctx, base);
    Line("recording", device, base, 0, 0);
}

extern "C" PPC_FUNC(__imp__sub_82302E88);
// With the decompiled code (COD3_HAVE_DECOMP) the end is its decompiled
// body, decomp_sub_82302E88.
#ifdef COD3_HAVE_DECOMP
PPC_FUNC(decomp_sub_82302E88);
#define COD3_RECORD_END decomp_sub_82302E88
#else
#define COD3_RECORD_END __imp__sub_82302E88
#endif
PPC_FUNC(sub_82302E88)
{
    const uint32_t device = ctx.r3.u32;
    Line("end rec", device, base, uint32_t(ctx.lr), 0);
    Timeline::Mark("end rec", uint32_t(ctx.lr));
    COD3_RECORD_END(ctx, base);
    Line("recorded", device, base, ctx.r3.u32, 0);
    Timeline::Mark("recorded", ctx.r3.u32);
}

// sub_822FDB70(device): what the title does when its wait for the GPU has
// seen no progress for five seconds (sub_822EC168 calls it and gives the
// wait up). Said every time: whether the title's own timeout comes at all
// is the question when a pool wait never ends.
extern "C" PPC_FUNC(__imp__sub_822FDB70);
// With the decompiled code (COD3_HAVE_DECOMP) the timeout is its decompiled
// body, decomp_sub_822FDB70.
#ifdef COD3_HAVE_DECOMP
PPC_FUNC(decomp_sub_822FDB70);
#define COD3_GPU_TIMEOUT decomp_sub_822FDB70
#else
#define COD3_GPU_TIMEOUT __imp__sub_822FDB70
#endif
PPC_FUNC(sub_822FDB70)
{
    static std::atomic<int> announced{ 0 };
    if (announced.fetch_add(1) < 8)
    {
        printf("pool: the title's own GPU timeout fired on os thread %lu, from %08X\n", GetCurrentThreadId(), uint32_t(ctx.lr));
        fflush(stdout);
    }
    Timeline::Mark("gpu timeout", uint32_t(ctx.lr));
    COD3_GPU_TIMEOUT(ctx, base);
    Timeline::Mark("gpu timeout done", ctx.r3.u32);
}
