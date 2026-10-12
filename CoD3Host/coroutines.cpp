#include "coroutines.h"
#include "kernel.h"

#include <cstdio>
#include <cstdlib>
#include <map>

#include <Windows.h>

namespace
{
    struct Coroutine
    {
        uint32_t key = 0;
        void* fiber = nullptr;
        void* resumer = nullptr;         // the engine's fiber, while running
        PPCContext* ctx = nullptr;
        uint8_t* base = nullptr;
        PPCFunc* entry = nullptr;
        bool running = false;            // inside its entry function
        bool finished = false;
        uint32_t generation = 0;         // the game module's lifetime it began in

        // The longjmp the thread asked for when it yielded.
        bool jumpPending = false;
        jmp_buf* jumpBuffer = nullptr;
        int jumpValue = 0;
    };

    // Script threads run on the title's main thread only; the table and the
    // fiber state are that thread's.
    thread_local std::map<uint32_t, Coroutine*>* t_table = nullptr;
    thread_local Coroutine* t_current = nullptr;
    thread_local void* t_engineFiber = nullptr;

    constexpr size_t FiberReserve = 4u << 20;   // host stack, reserved
    constexpr size_t FiberCommit = 64u << 10;

    int g_announced = 0;

    // The game module's lifetimes, and what the threads did in the current one.
    uint32_t g_generation = 0;
    uint32_t g_begun = 0, g_resumed = 0, g_destroyed = 0, g_staleResumes = 0;

    // COD3_TRACECOROUTINES=1: every ten seconds, how many threads began,
    // resumed and were destroyed in them, and how many are suspended.
    uint32_t g_windowBegun = 0, g_windowResumed = 0, g_windowDestroyed = 0;
    void Window()
    {
        static const bool wanted = getenv("COD3_TRACECOROUTINES") != nullptr;
        if (!wanted) return;
        static uint64_t since = GetTickCount64();
        const uint64_t now = GetTickCount64();
        if (now - since < 10000) return;
        since = now;
        uint32_t suspended = 0;
        if (t_table != nullptr) for (const auto& entry : *t_table) if (entry.second->running) suspended++;
        printf("coroutines: in ten seconds %u begun, %u resumed, %u destroyed; %u suspended now, generation %u\n",
            g_windowBegun, g_windowResumed, g_windowDestroyed, suspended, g_generation);
        fflush(stdout);
        g_windowBegun = g_windowResumed = g_windowDestroyed = 0;
    }

    void CALLBACK Start(void* argument)
    {
        Coroutine* coroutine = static_cast<Coroutine*>(argument);
        coroutine->running = true;
        coroutine->entry(*coroutine->ctx, coroutine->base);
        // The start function returned: the thread ran to its end without
        // yielding, or came back from its last yield and finished. Either
        // way the engine continues after the call that started it.
        coroutine->running = false;
        coroutine->finished = true;
        SwitchToFiber(coroutine->resumer);
    }

    void EnsureFiberThread()
    {
        if (t_engineFiber != nullptr) return;
        t_engineFiber = ConvertThreadToFiberEx(nullptr, FIBER_FLAG_FLOAT_SWITCH);
        if (t_engineFiber == nullptr)
        {
            // Already a fiber: the current one is the engine's.
            t_engineFiber = GetCurrentFiber();
        }
        t_table = new std::map<uint32_t, Coroutine*>();
    }

    // Switches to the thread's fiber and, once it yields or finishes, does
    // the longjmp it asked for, if any.
    void Switch(Coroutine& coroutine)
    {
        coroutine.resumer = GetCurrentFiber();
        Coroutine* outer = t_current;
        t_current = &coroutine;
        SwitchToFiber(coroutine.fiber);
        t_current = outer;

        if (g_announced < 60)
        {
            g_announced++;
            printf("coroutines: thread 0x%08X %s%s r3 0x%08X r1 0x%08X\n", coroutine.key,
                coroutine.finished ? "finished" : "yielded",
                coroutine.jumpPending ? " with a longjmp" : "",
                coroutine.ctx->r3.u32, coroutine.ctx->r1.u32);
            fflush(stdout);
        }
        if (coroutine.jumpPending)
        {
            coroutine.jumpPending = false;
            jmp_buf* buffer = coroutine.jumpBuffer;
            const int value = coroutine.jumpValue;
            longjmp(*buffer, value);
        }
    }
}

void Coroutines::Begin(PPCContext& ctx, uint8_t* base, uint32_t key, PPCFunc* entry)
{
    EnsureFiberThread();
    Coroutine*& slot = (*t_table)[key];
    if (slot == nullptr) slot = new Coroutine();
    Coroutine& coroutine = *slot;
    if (coroutine.fiber != nullptr)
    {
        if (coroutine.running)
        {
            printf("coroutines: thread 0x%08X is started again while suspended; "
                   "its old fiber is abandoned\n", key);
            fflush(stdout);
        }
        DeleteFiber(coroutine.fiber);
        coroutine.fiber = nullptr;
    }
    coroutine.key = key;
    coroutine.ctx = &ctx;
    coroutine.base = base;
    coroutine.entry = entry;
    coroutine.running = false;
    coroutine.finished = false;
    coroutine.jumpPending = false;
    coroutine.generation = g_generation;
    g_begun++;
    g_windowBegun++;
    Window();
    coroutine.fiber = CreateFiberEx(FiberCommit, FiberReserve, FIBER_FLAG_FLOAT_SWITCH,
                                    Start, &coroutine);
    if (g_announced++ < 12)
    {
        printf("coroutines: thread 0x%08X begins\n", key);
        fflush(stdout);
    }
    if (coroutine.fiber == nullptr)
    {
        printf("coroutines: no fiber for thread 0x%08X (error %lu); running it inline\n",
            key, GetLastError());
        fflush(stdout);
        entry(ctx, base);
        return;
    }
    Switch(coroutine);
}

void Coroutines::Resume(PPCContext& ctx, uint8_t* base, uint32_t key)
{
    EnsureFiberThread();
    auto found = t_table->find(key);
    Coroutine* coroutine = found != t_table->end() ? found->second : nullptr;
    if (coroutine == nullptr || coroutine->fiber == nullptr || !coroutine->running)
    {
        // Nothing suspended under this key. A thread that was never begun
        // through Begin, or one that finished, has no fiber to continue;
        // the engine's continuation address says where it wanted to go.
        printf("coroutines: thread 0x%08X resumed at 0x%08X but nothing is suspended for it\n",
            key, uint32_t(ctx.lr));
        fflush(stdout);
        return;
    }
    coroutine->ctx = &ctx;
    coroutine->base = base;
    g_resumed++;
    g_windowResumed++;
    Window();
    if (coroutine->generation != g_generation && g_staleResumes++ < 20)
    {
        printf("coroutines: thread 0x%08X resumed on a fiber begun in generation %u, now %u: its host frames are from before the restart\n",
            key, coroutine->generation, g_generation);
        fflush(stdout);
    }
    if (g_announced < 60)
    {
        g_announced++;
        printf("coroutines: thread 0x%08X resumes at 0x%08X, r1 0x%08X\n", key, uint32_t(ctx.lr), ctx.r1.u32);
        fflush(stdout);
    }
    Switch(*coroutine);
}

bool Coroutines::YieldIfCoroutine(jmp_buf& buffer, int value)
{
    Coroutine* coroutine = t_current;
    if (coroutine == nullptr) return false;
    coroutine->jumpPending = true;
    coroutine->jumpBuffer = &buffer;
    coroutine->jumpValue = value;
    SwitchToFiber(coroutine->resumer);
    // Resumed: the engine has loaded this thread's registers and switched
    // back; t_current was set by Switch.
    return true;
}

bool Coroutines::Finished(uint32_t key)
{
    if (t_table == nullptr) return false;
    auto found = t_table->find(key);
    return found != t_table->end() && found->second->finished;
}

void Coroutines::Destroy(uint32_t key)
{
    if (t_table == nullptr) return;
    auto found = t_table->find(key);
    if (found == t_table->end()) return;
    Coroutine* coroutine = found->second;
    t_table->erase(found);
    g_destroyed++;
    g_windowDestroyed++;
    if (coroutine == t_current)
    {
        // Destroying oneself: the fiber cannot be deleted from inside.
        // It is left to leak, which a level does a handful of times.
        return;
    }
    if (coroutine->fiber != nullptr) DeleteFiber(coroutine->fiber);
    delete coroutine;
}

bool Coroutines::Inside()
{
    return t_current != nullptr;
}

void Coroutines::NewGeneration()
{
    uint32_t suspended = 0, finished = 0;
    if (t_table != nullptr)
        for (const auto& entry : *t_table)
        {
            if (entry.second->running) suspended++;
            else if (entry.second->finished) finished++;
        }
    printf("coroutines: generation %u ends: %u threads begun, %u resumes, %u destroyed, %u fibers still suspended, %u finished and kept, %u stale resumes so far\n",
        g_generation, g_begun, g_resumed, g_destroyed, suspended, finished, g_staleResumes);
    fflush(stdout);
    g_generation++;
    g_begun = g_resumed = g_destroyed = 0;
}
