// Xbox 360 kernel imports: threads.
//
// A guest thread is a real host thread running recompiled code. Each one needs
// its own processor context and its own guest stack, because the translated
// code keeps every PowerPC register in the context it is handed and addresses
// its stack through r1.
//
// This is what turns the rest of the runtime concurrent, so the tables in the
// other kernel files are locked from here on.

#include "kernel.h"
#include <string>
#include "scheduler.h"

#include <atomic>
#include <cstdio>
#include <map>
#include <mutex>
#include <thread>
#include <chrono>

#include <Windows.h>

namespace
{
    constexpr uint32_t X_STATUS_SUCCESS           = 0x00000000;
    constexpr uint32_t X_STATUS_INVALID_HANDLE    = 0xC0000008;
    constexpr uint32_t X_STATUS_NO_MEMORY         = 0xC0000017;
    constexpr uint32_t X_STATUS_INVALID_PARAMETER = 0xC000000D;

    constexpr uint32_t DefaultStackSize = 512u << 10;
    constexpr uint32_t CreateSuspended  = 0x00000001;

    struct GuestThread
    {
        HANDLE host = nullptr;
        uint32_t id = 0;
        uint32_t osId = 0;
        uint32_t stackTop = 0;
        uint32_t startAddress = 0;
        uint32_t startContext = 0;
        uint32_t xapiStartup = 0;

        // Where its thread pointer block is, so an affinity change can be
        // written into it.
        uint32_t threadPointer = 0;

        // Parking. The film decoder's workers idle by suspending themselves
        // and are resumed by whoever has work for them, with a flag of the
        // title's own saying which state they are in. The flag is set
        // before the suspend takes effect, so a resume can arrive in
        // between, do nothing, and leave the worker asleep for good. The
        // state here closes that: a resume that finds the thread not yet
        // suspended is kept as a credit, and a self suspend that finds a
        // credit spends it instead of sleeping.
        bool parking = false;       // between deciding to suspend and suspending
        int resumeCredits = 0;      // resumes that arrived early

        // Where this thread lives in guest memory.
        //
        // The Ke family of thread calls take a pointer to the thread object,
        // not a handle, and the title gets that pointer from
        // ObReferenceObjectByHandle. With no object to hand back, that call
        // failed, the title passed on whatever was in the uninitialised
        // variable, and KeResumeThread was asked to resume a stack address. The
        // thread it was starting never ran, and the thread waiting for it to
        // finish waited forever.
        uint32_t guestObject = 0;
    };

    // Type six is a thread in the console's dispatcher, and a wait on one
    // completes when the thread ends.
    constexpr uint32_t ThreadObjectSize = 0x100;
    constexpr uint32_t ThreadObjectType = 6;
    constexpr uint32_t DispatchSignalState = 0x04;
    constexpr uint32_t ThreadObjectId = 0x14C;

    std::mutex g_threadsMutex;
    std::map<uint32_t, GuestThread> g_threads;
    uint32_t g_nextThreadHandle = 0x00020000;
    std::atomic<uint32_t> g_nextThreadId{ 1 };

    struct StartArguments
    {
        uint32_t stackTop;
        uint32_t startAddress;
        uint32_t startContext;
        uint32_t xapiStartup;
    
        // Which hardware thread the title asked for, or minus one for any.
        int processor = -1;
    };

    DWORD WINAPI ThreadEntry(void* parameter)
    {
        // Owned by this thread from here, and freed before it returns.
        StartArguments* arguments = static_cast<StartArguments*>(parameter);
        const StartArguments start = *arguments;
        delete arguments;

        // A fresh processor context. The alignment matters: PPCContext is
        // declared alignas(0x40) and the vector registers are loaded with
        // aligned instructions.
        alignas(0x40) PPCContext ctx{};
        ctx.r1.u32 = start.stackTop;
        const uint32_t threadPointer =
            Guest::CreateThreadPointer(GetCurrentThreadId(), start.processor);
        ctx.r13.u32 = threadPointer;
        Kernel::SetCurrentContext(&ctx);
        Kernel::RecordThreadPointer(GetCurrentThreadId(), threadPointer);
        Kernel::ArmWatchpoints();
        // COD3_HOSTPROFILE naming a start address (82123DE0, say) profiles
        // the threads that start there.
        if (getenv("COD3_HOSTPROFILE") != nullptr)
        {
            char tag[16];
            snprintf(tag, sizeof(tag), "%08X", start.xapiStartup != 0 ? start.startAddress : start.startAddress);
            const std::string* name = new std::string("guest thread at " + std::string(tag) + " (os " + std::to_string(GetCurrentThreadId()) + ")");
            Kernel::ProfileHostThread(GetCurrentThreadId(), name->c_str(), (new std::string(tag))->c_str());
        }
        // The thread that feeds the GPU, the replay of the recorded command
        // lists at sub_82302C80, gets a hardware thread of its own rather
        // than the one the title puts it on, which it shares with a render
        // worker: the console preempts the worker the moment the GPU asks,
        // this runtime only at the worker's next kernel call, and the GPU
        // waited milliseconds for every list. It reads the lists and writes
        // the ring, nothing the workers share a cursor over. COD3_HELPERSLOT
        // moves it back (a number) or elsewhere.
        int processor = Guest::CurrentProcessor(ctx);
        if (start.startAddress == 0x82302C80u)
        {
            static const char* const helperSlot = getenv("COD3_HELPERSLOT");
            processor = helperSlot != nullptr ? int(strtol(helperSlot, nullptr, 10)) : 6;
            printf("thread: the GPU feeder runs on hardware thread %d\n", processor);
        }
        Scheduler::Attach(processor);
        struct Leave { ~Leave() { Scheduler::Detach(); } } leave;
        ctx.fpscr.loadFromHost();

        PPCFunc* entry = nullptr;
        if (start.xapiStartup != 0)
        {
            // The console hands the title's own startup shim the routine and
            // its argument, and lets the shim call it.
            entry = Guest::Lookup(start.xapiStartup);
            ctx.r3.u32 = start.startAddress;
            ctx.r4.u32 = start.startContext;
        }
        else
        {
            entry = Guest::Lookup(start.startAddress);
            ctx.r3.u32 = start.startContext;
        }

        if (entry == nullptr)
        {
            fprintf(stderr, "thread: nothing recompiled at 0x%08X\n",
                start.xapiStartup != 0 ? start.xapiStartup : start.startAddress);
            return 1;
        }

        entry(ctx, Guest::Base);

        printf("thread: os %u finished, returning %u\n",
            GetCurrentThreadId(), ctx.r3.u32);
        fflush(stdout);

        // A wait on a thread object completes when the thread ends.
        Kernel::SignalThreadExit(GetCurrentThreadId());
        Guest::ReleaseThreadPointer(threadPointer);
        // The context lives on this stack, which is about to go: the
        // sampler must not find it afterwards.
        Kernel::SetCurrentContext(nullptr);
        return ctx.r3.u32;
    }
}

uint32_t Guest::AllocateStack(uint32_t size)
{
    // Stacks come out of the top of the virtual region and grow down, away
    // from the heap that grows up from the bottom.
    //
    // The main thread's stack is the megabyte ending at StackBase, and it is
    // not handed out here: the first stack this gives out has to begin
    // below it. It used to begin at StackBase itself, which gave the first
    // thread the title created the main thread's own stack, byte for byte,
    // and every thread after it a slice of the same megabyte. Two threads
    // pushing frames over each other's is what all the corruption looked
    // like: a list head of 1, an object of 0x20, a menu element of null,
    // each a value one thread had left where the other kept a pointer.
    //
    // A page is left uncommitted between stacks so that one running off its
    // end is reported as a commit outside every allocation rather than
    // read back by its neighbour.
    static std::mutex mutex;
    static uint32_t next = Guest::StackBase - Guest::StackSize;
    constexpr uint32_t Gap = 0x10000;

    std::lock_guard<std::mutex> lock(mutex);

    size = (size + 0xFFFF) & ~0xFFFFu;
    if (next < Guest::VirtualHeapBase + size + Gap)
        return 0;

    next -= size + Gap;
    if (VirtualAlloc(Guest::Ptr(next), size, MEM_COMMIT, PAGE_READWRITE) == nullptr)
        return 0;

    memset(Guest::Ptr(next), 0, size);
    return next + size;   // r1 starts at the top and grows down
}

// NTSTATUS ExCreateThread(HANDLE* handle, ULONG stackSize, ULONG* threadId,
//                         void* xapiThreadStartup, void* startAddress,
//                         void* startContext, ULONG creationFlags)
PPC_FUNC(__imp__ExCreateThread)
{
    Kernel::CountImport("ExCreateThread");
    const uint32_t handleOut = ctx.r3.u32;
    uint32_t stackSize = ctx.r4.u32;
    const uint32_t threadIdOut = ctx.r5.u32;
    const uint32_t xapiStartup = ctx.r6.u32;
    const uint32_t startAddress = ctx.r7.u32;
    const uint32_t startContext = ctx.r8.u32;
    const uint32_t creationFlags = ctx.r9.u32;

    if (stackSize == 0) stackSize = DefaultStackSize;
    if (stackSize < (64u << 10)) stackSize = 64u << 10;

    const uint32_t stackTop = Guest::AllocateStack(stackSize);
    if (stackTop == 0)
    {
        fprintf(stderr, "ExCreateThread: no room for a %u byte stack\n", stackSize);
        ctx.r3.u32 = X_STATUS_NO_MEMORY;
        return;
    }

    // The top byte of the creation flags is a mask of the hardware threads the
    // title will accept. It is not decoration: this title starts one worker
    // with mask 0x10, and a barrier elsewhere waits for exactly the byte that
    // hardware thread four writes. Ignoring the mask left that worker with the
    // wrong number, writing the wrong byte, and spinning forever.
    int processor = -1;
    const uint32_t processorMask = creationFlags >> 24;
    if (processorMask != 0)
    {
        for (int bit = 0; bit < 6; bit++)
            if (processorMask & (1u << bit)) { processor = bit; break; }
    }
    else
    {
        // With no mask the console starts the thread on the hardware thread its
        // creator is running on. That is not a detail: it is what stops the two
        // of them from ever being inside the same unlocked structure at once.
        processor = Guest::CurrentProcessor(ctx);
    }

    auto* arguments = new StartArguments{
        // Leave a little headroom: the translated prologues write below r1
        // before adjusting it.
        stackTop - 0x100, startAddress, startContext, xapiStartup, processor };

    printf("thread: started at 0x%08X%s%s", startAddress,
        (creationFlags & CreateSuspended) ? ", suspended until resumed" : "", "");
    if (processor >= 0) printf(", on hardware thread %d", processor);
    printf("\n");
    fflush(stdout);

    DWORD hostId = 0;
    // Sixteen megabytes of host stack, reserved not committed: a guest call
    // is a host call, so a level script that recurses deeply on its own
    // megabyte of guest stack recurses just as deeply on the host's, and a
    // host stack that runs out past its guard page ends the process with
    // no exception anyone gets to see.
    HANDLE host = CreateThread(nullptr, 16u << 20, ThreadEntry, arguments,
        ((creationFlags & CreateSuspended) ? CREATE_SUSPENDED : 0) | STACK_SIZE_PARAM_IS_A_RESERVATION, &hostId);


    if (host == nullptr)
    {
        delete arguments;
        fprintf(stderr, "ExCreateThread: CreateThread failed with %lu\n", GetLastError());
        ctx.r3.u32 = X_STATUS_NO_MEMORY;
        return;
    }

    GuestThread thread;
    thread.host = host;
    thread.id = g_nextThreadId++;
    thread.osId = hostId;
    thread.stackTop = stackTop;
    thread.startAddress = startAddress;
    thread.startContext = startContext;
    thread.xapiStartup = xapiStartup;

    uint32_t handle;
    {
        std::lock_guard<std::mutex> lock(g_threadsMutex);
        handle = g_nextThreadHandle;
        g_nextThreadHandle += 4;

        thread.guestObject = Guest::AllocateKernelObject(ThreadObjectSize);
        if (thread.guestObject != 0)
        {
            Guest::Write32(Guest::Base, thread.guestObject,
                ThreadObjectType << 24);
            Guest::Write32(Guest::Base, thread.guestObject + DispatchSignalState, 0);
            Guest::Write32(Guest::Base, thread.guestObject + ThreadObjectId,
                thread.id);
        }
        g_threads[handle] = thread;
    }

    Kernel::Stats().threadsCreated.fetch_add(1, std::memory_order_relaxed);

    if (handleOut != 0) Guest::Write32(base, handleOut, handle);
    if (threadIdOut != 0) Guest::Write32(base, threadIdOut, thread.id);
    ctx.r3.u32 = X_STATUS_SUCCESS;
}

// VOID ExTerminateThread(ULONG exitCode)
PPC_FUNC(__imp__ExTerminateThread)
{
    Kernel::CountImport("ExTerminateThread");

    // Ends the calling thread. There is no way to unwind the recompiled stack,
    // so the host thread is ended where it stands, but everything the rest of
    // the runtime hangs off this thread has to be let go first.
    //
    // Leaving it out cost twice over: a thread waiting on this one's handle
    // waited for a signal that never came, and the hardware thread this one was
    // holding was never given back, so everything else assigned to it fell back
    // to running unserialised after a timeout.
    uint32_t block = 0;
    {
        std::lock_guard<std::mutex> lock(g_threadsMutex);
        for (auto& entry : g_threads)
            if (entry.second.osId == GetCurrentThreadId())
            {
                block = entry.second.threadPointer;
                break;
            }
    }

    Kernel::SignalThreadExit(GetCurrentThreadId());
    Scheduler::Detach();
    if (block != 0) Guest::ReleaseThreadPointer(block);
    Kernel::SetCurrentContext(nullptr);

    ExitThread(ctx.r3.u32);
}

namespace
{
    // A handle or the object pointer: the Nt family passes one, the Ke family
    // the other, and both have to arrive at the same thread.
    GuestThread* FindThread(uint32_t handleOrObject)
    {
        auto found = g_threads.find(handleOrObject);
        if (found != g_threads.end()) return &found->second;

        for (auto& entry : g_threads)
            if (entry.second.guestObject != 0 &&
                entry.second.guestObject == handleOrObject)
                return &entry.second;
        return nullptr;
    }
}

// NTSTATUS NtResumeThread(HANDLE handle, ULONG* previousCount)
PPC_FUNC(__imp__NtResumeThread)
{
    Kernel::CountImport("NtResumeThread");
    HANDLE host = nullptr;
    bool parking = false;
    bool credited = false;
    {
        std::lock_guard<std::mutex> lock(g_threadsMutex);
        GuestThread* thread = FindThread(ctx.r3.u32);
        if (thread != nullptr)
        {
            host = thread->host;
            parking = thread->parking;
            if (parking)
            {
                thread->parking = false;
            }
            else if (thread->osId != 0 && thread->osId != GetCurrentThreadId())
            {
                // Not parking and, if it is not suspended either, the resume
                // would be lost; the credit is spent by its next self suspend.
                // A thread still suspended from its creation is resumed for
                // real below and the credit taken back.
                thread->resumeCredits++;
                credited = true;
            }
        }
    }
    if (host == nullptr)
    {
        printf("thread: asked to resume 0x%08X, which is not a thread\n", ctx.r3.u32);
        fflush(stdout);
        ctx.r3.u32 = X_STATUS_INVALID_HANDLE;
        return;
    }

    DWORD previous = ResumeThread(host);
    if (parking)
    {
        // The thread said it was about to suspend; if it has not yet, the
        // resume did nothing and is tried again until it has.
        while (previous == 0)
        {
            std::this_thread::yield();
            previous = ResumeThread(host);
        }
    }
    else if (credited && previous != DWORD(-1) && previous != 0)
    {
        // It was suspended after all, so the resume counted; no credit.
        std::lock_guard<std::mutex> lock(g_threadsMutex);
        GuestThread* thread = FindThread(ctx.r3.u32);
        if (thread != nullptr && thread->resumeCredits > 0) thread->resumeCredits--;
    }
    if (ctx.r4.u32 != 0 && previous != DWORD(-1))
        Guest::Write32(base, ctx.r4.u32, previous);
    ctx.r3.u32 = X_STATUS_SUCCESS;
}

// NTSTATUS NtSuspendThread(HANDLE handle, ULONG* previousCount)
PPC_FUNC(__imp__NtSuspendThread)
{
    Kernel::CountImport("NtSuspendThread");

    // The handle is looked up under the lock and the suspension done without
    // it. The film decoder's workers park themselves with this call and are
    // resumed by the thread that has work for them; suspending with the
    // lock held meant the parked thread took the thread table down with it,
    // the resume that would have woken it blocked on that same lock, and
    // every film stopped on its first frame with the runtime deadlocked.
    HANDLE host = nullptr;
    bool self = false;
    bool spentCredit = false;
    {
        std::lock_guard<std::mutex> lock(g_threadsMutex);
        GuestThread* thread = FindThread(ctx.r3.u32);
        if (thread == nullptr) { ctx.r3.u32 = X_STATUS_INVALID_HANDLE; return; }
        host = thread->host;
        self = thread->osId == GetCurrentThreadId();
        if (self)
        {
            if (thread->resumeCredits > 0)
            {
                thread->resumeCredits--;
                spentCredit = true;
            }
            else
            {
                thread->parking = true;
            }
        }
    }

    DWORD previous;
    static const bool noPark = getenv("COD3_NOPARK") != nullptr;
    if (self && (spentCredit || noPark))
    {
        // A resume already came for this suspend: it is over before it began.
        // COD3_NOPARK turns every self suspend into this, a yield instead of
        // a sleep, to tell whether a stall is the parking at all.
        if (noPark && !spentCredit)
        {
            std::lock_guard<std::mutex> lock(g_threadsMutex);
            GuestThread* thread = FindThread(ctx.r3.u32);
            if (thread != nullptr) thread->parking = false;
            Scheduler::Release();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            Scheduler::Acquire();
        }
        previous = 0;
    }
    else if (self)
    {
        // Parking: the hardware thread is handed back for the duration,
        // exactly as it is for a wait, and taken again on the way out.
        Scheduler::Release();
        previous = SuspendThread(GetCurrentThread());
        Scheduler::Acquire();
        std::lock_guard<std::mutex> lock(g_threadsMutex);
        GuestThread* thread = FindThread(ctx.r3.u32);
        if (thread != nullptr) thread->parking = false;
    }
    else
    {
        previous = SuspendThread(host);
    }
    if (ctx.r4.u32 != 0 && previous != DWORD(-1))
        Guest::Write32(base, ctx.r4.u32, previous);
    ctx.r3.u32 = X_STATUS_SUCCESS;
}

// ULONG KeResumeThread(void* thread)
PPC_FUNC(__imp__KeResumeThread)
{
    Kernel::CountImport("KeResumeThread");

    // The same as NtResumeThread, by object rather than handle, and with the
    // same care over a thread that is about to park: FindThread takes both.
    ctx.r4.u32 = 0;
    __imp__NtResumeThread(ctx, base);
    ctx.r3.u32 = 0;
}

// KAFFINITY KeSetAffinityThread(void* thread, KAFFINITY affinity)
PPC_FUNC(__imp__KeSetAffinityThread)
{
    Kernel::CountImport("KeSetAffinityThread");

    // The console pins threads to its six hardware threads, and a title that
    // spreads its work across them is relying on which ones. Ignoring this left
    // every thread the title created sharing one hardware thread with its
    // creator, which is correct but needlessly slow: they take it in turns when
    // they were meant to run side by side.
    const uint32_t object = ctx.r3.u32;
    const uint32_t affinity = ctx.r4.u32;

    int processor = -1;
    for (int bit = 0; bit < 6; bit++)
        if (affinity & (1u << bit)) { processor = bit; break; }

    ctx.r3.u32 = affinity;
    if (processor < 0) return;

    std::lock_guard<std::mutex> lock(g_threadsMutex);
    GuestThread* thread = FindThread(object);
    if (thread == nullptr) return;

    if (thread->threadPointer != 0)
        *(Guest::Base + thread->threadPointer + 0x10C) = uint8_t(processor);
    Scheduler::Reassign(thread->osId, processor);
}

// LONG KeSetBasePriorityThread(void* thread, LONG priority)
PPC_FUNC(__imp__KeSetBasePriorityThread)
{
    Kernel::CountImport("KeSetBasePriorityThread");
    std::lock_guard<std::mutex> lock(g_threadsMutex);
    GuestThread* thread = FindThread(ctx.r3.u32);
    if (thread == nullptr) { ctx.r3.u32 = 0; return; }

    const int32_t requested = int32_t(ctx.r4.u32);
    int priority = THREAD_PRIORITY_NORMAL;
    if (requested > 0) priority = THREAD_PRIORITY_ABOVE_NORMAL;
    else if (requested < 0) priority = THREAD_PRIORITY_BELOW_NORMAL;

    SetThreadPriority(thread->host, priority);
    ctx.r3.u32 = 0;
}

void Kernel::RegisterEntryThread()
{
    HANDLE duplicate = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
                         GetCurrentProcess(), &duplicate,
                         THREAD_ALL_ACCESS, FALSE, 0))
        return;

    GuestThread thread;
    thread.host = duplicate;
    thread.id = 0;   // zero marks the thread the title started on
    Kernel::ProfileHostThread(GetCurrentThreadId(), "title's first thread", "main");
    thread.osId = GetCurrentThreadId();

    std::lock_guard<std::mutex> lock(g_threadsMutex);
    g_threads[0] = thread;
}

std::vector<Kernel::ThreadSample> Kernel::SampleGuestThreads()
{
    std::lock_guard<std::mutex> lock(g_threadsMutex);
    std::vector<ThreadSample> out;
    out.reserve(g_threads.size());
    for (const auto& entry : g_threads)
        out.push_back({ entry.second.host, entry.second.id, entry.second.osId, entry.second.startAddress });
    return out;
}

void Kernel::SignalThreadExit(uint32_t osId)
{
    // Anything this thread still holds goes back before anyone is told it has
    // gone, so a waiter woken by the news finds the locks free.
    Kernel::AbandonMutants(osId);

    uint32_t object = 0;
    {
        std::lock_guard<std::mutex> lock(g_threadsMutex);
        for (auto& entry : g_threads)
            if (entry.second.osId == osId) { object = entry.second.guestObject; break; }
    }
    if (object == 0) return;

    std::lock_guard<std::mutex> lock(Kernel::DispatcherLock());
    Guest::Write32(Guest::Base, object + DispatchSignalState, 1);
    Kernel::WakeDispatcher();
}

uint32_t Kernel::ThreadObjectFor(uint32_t handle)
{
    std::lock_guard<std::mutex> lock(g_threadsMutex);
    GuestThread* thread = FindThread(handle);
    return thread != nullptr ? thread->guestObject : 0;
}

void Kernel::RecordThreadPointer(uint32_t osId, uint32_t block)
{
    std::lock_guard<std::mutex> lock(g_threadsMutex);
    for (auto& entry : g_threads)
        if (entry.second.osId == osId) { entry.second.threadPointer = block; return; }
}

std::string Kernel::DescribeThreadHandle(uint32_t handle)
{
    std::lock_guard<std::mutex> lock(g_threadsMutex);
    GuestThread* thread = FindThread(handle);
    if (thread == nullptr) return {};

    char text[96];
    snprintf(text, sizeof(text), "os %u, started at 0x%08X",
        thread->osId, thread->startAddress);
    return text;
}
