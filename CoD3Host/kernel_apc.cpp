// Completion routines.
//
// An asynchronous read on this console does not call back from inside the read.
// The kernel queues the routine against the thread that asked, returns
// STATUS_PENDING, and runs it later, when that thread next waits. A title that
// streams a file relies on the whole shape of that: it issues a read, goes back
// to its loop, and only when the routine runs does it ask for the next block.
//
// This runtime used to ignore the routine entirely, and the title read the
// first four hundred kilobyte block of its eight megabyte archive and then
// waited for a completion that was never going to come.
//
// A routine runs when the thread waits and says it may be interrupted, which is
// what the console does. One that has waited longer than a frame runs anyway: a
// routine that is queued and never delivered is the failure this exists to
// prevent.

#include "kernel.h"

#include <cstdio>
#include <atomic>
#include <chrono>
#include <deque>
#include <map>
#include <mutex>

#include <Windows.h>

namespace
{
    struct Apc
    {
        uint32_t routine;
        uint32_t context;
        uint32_t statusBlock;
        uint32_t status;        // written into the block when this is delivered
        uint32_t information;
        std::chrono::steady_clock::time_point queued;
    };

    std::mutex g_mutex;
    std::map<uint32_t, std::deque<Apc>> g_queues;   // host thread id -> pending
    uint64_t g_queued = 0;
    uint64_t g_delivered = 0;

    // A thread queues its completion routines to itself: how many it has,
    // so the waits (two million a second, most of them the job lock) need
    // no lock to find there are none.
    thread_local uint32_t t_pending = 0;
}

void Kernel::QueueApc(uint32_t routine, uint32_t context, uint32_t statusBlock,
                      uint32_t status, uint32_t information)
{
    if (routine == 0) return;

    std::lock_guard<std::mutex> lock(g_mutex);
    g_queues[GetCurrentThreadId()].push_back(
        { routine, context, statusBlock, status, information,
          std::chrono::steady_clock::now() });
    g_queued++;
    t_pending++;
}

void Kernel::DeliverApcs(PPCContext& ctx, bool alertable)
{
    if (t_pending == 0) return;
    std::deque<Apc> pending;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto found = g_queues.find(GetCurrentThreadId());
        if (found == g_queues.end() || found->second.empty()) return;

        // Only at an alertable wait, which is the only time the console runs
        // a user mode routine. This used to hand them over anyway once they
        // were a frame old, on the theory that a title might never wait
        // alertably; what it did instead was run a completion routine in the
        // middle of whatever the thread was doing under a lock, and the
        // title's archive parser then found its own request list changed
        // under it. "malformed branch data []", one run in four to eight.
        // A routine that is genuinely never collected is reported below
        // rather than forced.
        if (!alertable)
        {
            const auto age = std::chrono::steady_clock::now()
                           - found->second.front().queued;
            if (age > std::chrono::seconds(2))
            {
                static std::atomic<int> announced{ 0 };
                if (announced.fetch_add(1) < 3)
                {
                    printf("apc: a completion routine has waited two seconds "
                           "for an alertable wait on thread %u\n",
                        GetCurrentThreadId());
                    fflush(stdout);
                }
            }
            return;
        }
        pending.swap(found->second);
        t_pending = 0;
    }

    // Everything the caller still needs. A completion routine is an ordinary
    // call and may use any register it is allowed to.
    const uint64_t savedLink = ctx.lr;
    const uint32_t savedR3 = ctx.r3.u32;
    const uint32_t savedR4 = ctx.r4.u32;
    const uint32_t savedR5 = ctx.r5.u32;
    const uint32_t savedR6 = ctx.r6.u32;
    const uint32_t savedR7 = ctx.r7.u32;

    for (const Apc& apc : pending)
    {
        // The low bits of the pointer are flags rather than address, which is
        // why the value the title passes is odd.
        PPCFunc* entry = Guest::Lookup(apc.routine & ~3u);
        if (entry == nullptr)
        {
            static bool reported = false;
            if (!reported)
            {
                reported = true;
                printf("apc: nothing recompiled at 0x%08X\n", apc.routine & ~3u);
                fflush(stdout);
            }
            continue;
        }

        // The status block is written at completion, which is now, the way
        // the console's I/O manager writes it: after the call returned pending
        // and before the routine runs. Writing it at the call instead left a
        // window in which a title that sets the block to pending itself after
        // the call would read that back as the result, and this one did: its
        // archive parser found an empty token where the data should have been
        // and threw "malformed general branch information []", one run in four.
        if (apc.statusBlock != 0)
        {
            Guest::Write32(Guest::Base, apc.statusBlock + 0, apc.status);
            Guest::Write32(Guest::Base, apc.statusBlock + 4, apc.information);
        }

        ctx.r3.u32 = apc.context;
        ctx.r4.u32 = apc.statusBlock;
        ctx.r5.u32 = 0;
        entry(ctx, Guest::Base);

        std::lock_guard<std::mutex> lock(g_mutex);
        g_delivered++;
    }

    ctx.lr = savedLink;
    ctx.r3.u32 = savedR3;
    ctx.r4.u32 = savedR4;
    ctx.r5.u32 = savedR5;
    ctx.r6.u32 = savedR6;
    ctx.r7.u32 = savedR7;
}

void Kernel::ReportApcs()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_queued == 0) return;

    size_t waiting = 0;
    for (const auto& entry : g_queues) waiting += entry.second.size();

    printf("completions: %llu queued, %llu delivered, %zu waiting\n",
        (unsigned long long)g_queued, (unsigned long long)g_delivered, waiting);
    fflush(stdout);
}
