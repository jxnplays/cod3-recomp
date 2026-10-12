// Six hardware threads, the way the console has them.
//
// The console runs six hardware threads and every guest thread sits on one of
// them. Two threads on the same one cannot run at the same instant: the
// hardware alternates between them. A title can rely on that, and this one
// looks as though it does. It hands memory out of a frame arena with a single
// unlocked cursor from more than one thread, which is safe if those threads
// never overlap and corrupts itself if they do.
//
// This runtime ran every guest thread on its own host core, so they did
// overlap. Here each hardware thread is a lock, and a guest thread holds its
// own while it runs guest code.
//
// Two things keep that from turning into a hang. Every kernel call that can
// block releases the lock first and takes it again afterwards, so a thread
// waiting for another never holds the slot they share. And the lock is only
// ever waited for with a timeout: if something is held longer than it should
// be, the thread runs anyway and the serialisation is merely imperfect, rather
// than the title stopping dead.

#include "kernel.h"
#include "scheduler.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <Windows.h>
#include <map>

namespace
{
    // Six, as the console has, and two more that nothing on the console
    // has: a thread put on one of those runs alongside everything.
    constexpr int HardwareThreads = 8;
    // Waiting for a slot.
    //
    // Fifty milliseconds was chosen so nothing could wait forever, and it did
    // that, but a thread that gives up runs anyway: two guest threads then run
    // at once on a hardware thread the console never overlaps, which is the one
    // thing this file exists to prevent. Fifty of those happened in every run,
    // and the title died in a different place each time.
    //
    // Two seconds is long enough that no honest hand over reaches it, so the
    // serialisation now actually holds; short enough that a genuine deadlock
    // still reports itself and the run goes on rather than hanging silently.
    constexpr auto AcquireTimeout = std::chrono::seconds(2);

    std::timed_mutex g_slots[HardwareThreads];

    // How many threads are waiting for each slot. A hand over is only a
    // hand over if the waiter gets the slot: a thread that drops it and
    // takes it straight back wins the race nearly every time, and the
    // title's replaying thread, woken to feed the GPU, waited tens of
    // milliseconds behind a worker spinning on the same hardware thread.
    std::atomic<int> g_waiters[HardwareThreads];
    std::atomic<uint64_t> g_handOvers{ 0 };

    thread_local int t_slot = -1;
    thread_local bool t_held = false;
    thread_local std::chrono::steady_clock::time_point t_since{};

    std::mutex g_pendingMutex;
    std::map<unsigned long, int> g_pending;   // os thread id -> wanted slot

    std::atomic<uint64_t> g_timeouts{ 0 };
    std::atomic<uint64_t> g_moves{ 0 };
    std::atomic<uint64_t> g_checkpoints{ 0 };
    std::atomic<bool> g_enabled{ true };

    // A pending move is applied while the slot is not held, which is the only
    // moment it is safe to change which one this thread belongs to.
    void ApplyPending()
    {
        if (t_held) return;

        std::lock_guard<std::mutex> lock(g_pendingMutex);
        auto found = g_pending.find(GetCurrentThreadId());
        if (found == g_pending.end()) return;

        t_slot = found->second;
        g_pending.erase(found);
        g_moves.fetch_add(1, std::memory_order_relaxed);
    }

    void Take()
    {
        ApplyPending();
        if (t_slot < 0 || t_held || !g_enabled.load(std::memory_order_relaxed)) return;

        g_waiters[t_slot].fetch_add(1, std::memory_order_acq_rel);
        const bool taken = g_slots[t_slot].try_lock() || g_slots[t_slot].try_lock_for(AcquireTimeout);
        g_waiters[t_slot].fetch_sub(1, std::memory_order_acq_rel);
        if (taken)
        {
            t_held = true;
        }
        else
        {
            // Running without the slot is worse than running with it and far
            // better than not running at all. It is also a correctness hole,
            // so it says so: at two seconds this should never happen, and if
            // it does the reason matters more than the run continuing.
            g_timeouts.fetch_add(1, std::memory_order_relaxed);
            static std::atomic<int> announced{ 0 };
            if (announced.fetch_add(1) < 4)
            {
                printf("scheduling: hardware thread %d could not be taken in "
                       "two seconds, so two guest threads now share it\n",
                    t_slot);
                fflush(stdout);
            }
        }
        t_since = std::chrono::steady_clock::now();
    }

    void Drop()
    {
        if (!t_held) return;
        t_held = false;
        g_slots[t_slot].unlock();
    }
}

void Scheduler::Attach(int hardwareThread)
{
    if (hardwareThread < 0 || hardwareThread >= HardwareThreads) return;
    // COD3_ONESLOT=1: every guest thread on the first hardware thread, so
    // no two ever run guest code at once. Slow, and only for telling a race
    // between threads from a fault of one thread alone.
    static const bool oneSlot = getenv("COD3_ONESLOT") != nullptr;
    if (oneSlot) hardwareThread = 0;
    t_slot = hardwareThread;
    Take();
}

void Scheduler::Detach()
{
    Drop();
    t_slot = -1;
}

void Scheduler::Release() { Drop(); }
void Scheduler::Acquire() { Take(); }

void Scheduler::Checkpoint()
{
    if (!t_held) return;

    // A thread that never calls anything blocking would otherwise keep its
    // hardware thread to itself for as long as it spins. With nobody waiting
    // there is nothing to do; with someone waiting the slot goes to them:
    // dropped, and not taken back until they have it or a while has passed,
    // since the one who just dropped it would otherwise take it straight
    // back. Twenty microseconds between hand overs keeps this from being
    // all a spinning thread does.
    if (g_waiters[t_slot].load(std::memory_order_acquire) == 0) return;
    const auto now = std::chrono::steady_clock::now();
    if (now - t_since < std::chrono::microseconds(20)) return;

    g_checkpoints.fetch_add(1, std::memory_order_relaxed);
    const int slot = t_slot;
    Drop();
    for (int spins = 0; spins < 200 && g_waiters[slot].load(std::memory_order_acquire) > 0; spins++)
        std::this_thread::yield();
    g_handOvers.fetch_add(1, std::memory_order_relaxed);
    Take();
}

void Scheduler::SetEnabled(bool enabled) { g_enabled.store(enabled); }

void Scheduler::Report()
{
    const uint64_t timeouts = g_timeouts.load();
    const uint64_t checkpoints = g_checkpoints.load();
    if (timeouts == 0 && checkpoints == 0) return;

    printf("scheduling: %llu hardware thread hand overs, %llu moves between "
           "hardware threads, %llu times a slot could not be taken in time\n",
        (unsigned long long)g_handOvers.load(),
        (unsigned long long)g_moves.load(),
        (unsigned long long)timeouts);
    fflush(stdout);
}

void Scheduler::Reassign(unsigned long osThreadId, int hardwareThread)
{
    if (hardwareThread < 0 || hardwareThread >= HardwareThreads) return;
    static const bool oneSlot = getenv("COD3_ONESLOT") != nullptr;
    if (oneSlot) hardwareThread = 0;

    std::lock_guard<std::mutex> lock(g_pendingMutex);
    g_pending[osThreadId] = hardwareThread;
}
