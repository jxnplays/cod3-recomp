// Xbox 360 kernel imports: process and system queries, time, interrupt level,
// critical sections, thread local storage, and the debug entry points.
//
// Guest threads are real host threads, so the primitives here are real too.
// Thread local storage is per thread with a shared slot allocator, and a
// critical section blocks a thread that does not own it. Interrupt level is
// the exception: there are no interrupts to mask, so raising and lowering it
// is bookkeeping the guest never observes.

#include "kernel.h"
#include "sampler.h"
#include "scheduler.h"
#include <chrono>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <Windows.h>

namespace
{
    constexpr uint32_t X_STATUS_SUCCESS           = 0x00000000;
    constexpr uint32_t X_STATUS_INVALID_PARAMETER = 0xC000000D;
    constexpr uint32_t X_STATUS_NOT_FOUND         = 0xC0000225;

    // The console's timebase, used for KeQueryPerformanceFrequency and by any
    // guest code that turns tick deltas into seconds.
    constexpr uint64_t TimebaseFrequency = 49875000;

    // Critical section layout, as the title sees it.
    constexpr uint32_t CS_LOCK_COUNT      = 0x10;
    constexpr uint32_t CS_RECURSION_COUNT = 0x14;
    constexpr uint32_t CS_OWNING_THREAD   = 0x18;

    // Slot indices are shared across threads; the values in them are not.
    constexpr uint32_t TlsSlotCount = 64;
    std::mutex g_tlsMutex;
    bool g_tlsTaken[TlsSlotCount] = {};
    thread_local uint32_t t_tlsSlots[TlsSlotCount] = {};

    std::string GuestString(const uint8_t* base, uint32_t address, size_t limit = 1024)
    {
        if (address == 0) return {};
        std::string out;
        for (size_t i = 0; i < limit; i++)
        {
            const char c = static_cast<char>(*(base + address + i));
            if (c == '\0') break;
            out += c;
        }
        return out;
    }

    [[noreturn]] void Stop(const char* what, uint32_t code)
    {
        printf("\nThe guest stopped the console: %s (0x%08X)\n", what, code);
        fflush(stdout);
            Kernel::Exit(1);
    }
}

// --- Process and system queries ------------------------------------------

// ULONG KeGetCurrentProcessType(void)
PPC_FUNC(__imp__KeGetCurrentProcessType)
{
    Kernel::CountImport("KeGetCurrentProcessType");
    ctx.r3.u32 = 1;   // a title, as opposed to the dashboard or a system process
}

// NTSTATUS ExGetXConfigSetting(WORD category, WORD setting, void* buffer,
//                              WORD length, WORD* outLength)
PPC_FUNC(__imp__ExGetXConfigSetting)
{
    Kernel::CountImport("ExGetXConfigSetting");
    const uint32_t category = ctx.r3.u32 & 0xFFFF;
    const uint32_t setting = ctx.r4.u32 & 0xFFFF;
    const uint32_t buffer = ctx.r5.u32;
    const uint32_t length = ctx.r6.u32 & 0xFFFF;

    uint32_t value = 0;
    bool known = true;

    if (category == 0x0002)         // secured settings
    {
        switch (setting)
        {
        case 0x0002: value = 0x00001000; break;   // AV region: NTSC
        default: known = false; break;
        }
    }
    else if (category == 0x0003)    // user settings
    {
        switch (setting)
        {
        case 0x0001: value = 0; break;            // time zone bias
        case 0x0009: value = 1; break;            // language: English
        case 0x000A: value = 0x00040000; break;   // video flags: widescreen
        case 0x000C: value = 0; break;            // retail flags
        case 0x000E: value = 103; break;          // country: United States
        default: known = false; break;
        }
    }
    else
    {
        known = false;
    }

    if (!known)
    {
        // Report it rather than inventing a value. A setting the title
        // actually depends on will show up here first.
        printf("ExGetXConfigSetting: category 0x%04X setting 0x%04X is not known, "
               "reporting not found\n", category, setting);
        ctx.r3.u32 = X_STATUS_NOT_FOUND;
        return;
    }

    if (buffer != 0 && length >= 4)
        Guest::Write32(base, buffer, value);
    if (ctx.r7.u32 != 0)
        Guest::Write32(base, ctx.r7.u32, 4);

    ctx.r3.u32 = X_STATUS_SUCCESS;
}

// ULONG XGetLanguage(void)
PPC_FUNC(__imp__XGetLanguage) { ctx.r3.u32 = 1; }          // English

// ULONG XGetAVPack(void)
PPC_FUNC(__imp__XGetAVPack) { ctx.r3.u32 = 8; }            // HDMI

// ULONG XGetGameRegion(void)
PPC_FUNC(__imp__XGetGameRegion) { ctx.r3.u32 = 0x00FF; }   // region free

// VOID XGetVideoMode(X_VIDEO_MODE* mode)
PPC_FUNC(__imp__XGetVideoMode)
{
    Kernel::CountImport("XGetVideoMode");
    const uint32_t mode = ctx.r3.u32;
    if (mode == 0) return;

    memset(Guest::Ptr(mode), 0, 48);
    Guest::Write32(base, mode + 0x00, 1280);        // width
    Guest::Write32(base, mode + 0x04, 720);         // height
    Guest::Write32(base, mode + 0x08, 0);           // not interlaced
    Guest::Write32(base, mode + 0x0C, 1);           // widescreen
    Guest::Write32(base, mode + 0x10, 1);           // high definition
    Guest::Write32(base, mode + 0x14, 0x42700000);  // 60.0f refresh rate
    Guest::Write32(base, mode + 0x18, 1);           // NTSC
}

// ULONG XamGetSystemVersion(void)
PPC_FUNC(__imp__XamGetSystemVersion) { ctx.r3.u32 = 0; }

// NTSTATUS XexCheckExecutablePrivilege(ULONG privilege)
PPC_FUNC(__imp__XexCheckExecutablePrivilege) { ctx.r3.u32 = 0; }

// HANDLE XexGetModuleHandle(char* name, HANDLE* out)
PPC_FUNC(__imp__XexGetModuleHandle)
{
    Kernel::CountImport("XexGetModuleHandle");
    // Only the title itself is loaded, and it has no handle to hand out yet.
    if (ctx.r4.u32 != 0)
        Guest::Write32(base, ctx.r4.u32, 0);
    ctx.r3.u32 = X_STATUS_NOT_FOUND;
}

// --- Time ------------------------------------------------------------------

// VOID KeQueryPerformanceFrequency(void) -> frequency in r3:r4
PPC_FUNC(__imp__KeQueryPerformanceFrequency)
{
    Kernel::CountImport("KeQueryPerformanceFrequency");
    ctx.r3.u64 = TimebaseFrequency;
}

// VOID KeQuerySystemTime(LARGE_INTEGER* time)
PPC_FUNC(__imp__KeQuerySystemTime)
{
    Kernel::CountImport("KeQuerySystemTime");
    Scheduler::Checkpoint();
    if (ctx.r3.u32 == 0) return;

    // The coarse form of this only moves every 15.6 milliseconds, and the
    // title reads it in a tight loop to measure short intervals.
    FILETIME now;
    GetSystemTimePreciseAsFileTime(&now);
    const uint64_t value =
        (uint64_t(now.dwHighDateTime) << 32) | uint64_t(now.dwLowDateTime);
    Guest::Write64(base, ctx.r3.u32, value);
}

// NTSTATUS KeDelayExecutionThread(mode, alertable, LARGE_INTEGER* interval)
PPC_FUNC(__imp__KeDelayExecutionThread)
{
    Kernel::CountImport("KeDelayExecutionThread");
    Scheduler::Release();
    struct Resume { ~Resume() { Scheduler::Acquire(); } } resume;
    Kernel::DeliverApcs(ctx, ctx.r4.u32 != 0);
    uint32_t milliseconds = 0;
    if (ctx.r5.u32 != 0)
    {
        // Negative means relative, in 100 nanosecond units.
        const int64_t interval = static_cast<int64_t>(Guest::Read64(base, ctx.r5.u32));
        if (interval < 0)
            milliseconds = static_cast<uint32_t>((-interval) / 10000);
    }
    Sleep(milliseconds);
    ctx.r3.u32 = X_STATUS_SUCCESS;
}

// NTSTATUS NtYieldExecution(void)
PPC_FUNC(__imp__NtYieldExecution)
{
    Kernel::CountImport("NtYieldExecution");
    Scheduler::Release();
    struct Resume { ~Resume() { Scheduler::Acquire(); } } resume;
    Kernel::DeliverApcs(ctx, false);
    SwitchToThread();
    ctx.r3.u32 = X_STATUS_SUCCESS;
}

// --- Interrupt level and spin locks ---------------------------------------
//
// The spin locks did nothing, from the days when nothing ran concurrently,
// and the title's threads run concurrently now. What the title guards with
// them was not guarded at all: the graphics driver's count of pending
// command buffer submissions (device+0x2A74, under the lock at +0x2A78) was
// updated from two threads at once, and one update in some thousands was
// lost. Left at one or at minus one instead of zero, it sent every later
// submission to a list that nothing would ever flush, and the next wait for
// the GPU never ended ("The GPU is hung and can't be recovered").
//
// The console's spin lock is a word in the title's memory: zero when free,
// the owner's thread pointer while held, taken with a reservation and a
// conditional store. The same here, with a compare and swap on the guest's
// word, so code of the title's that looks at the word sees what it would on
// the console. A thread that has to wait stands aside at its hardware thread
// now and then: the holder may share it, and cannot run while the waiter
// holds it.
namespace
{
    void AcquireGuestSpinLock(const PPCContext& ctx, uint8_t* base, uint32_t lock)
    {
        if (lock == 0) return;
        uint32_t* word = reinterpret_cast<uint32_t*>(base + lock);
        const uint32_t owner = __builtin_bswap32(ctx.r13.u32 != 0 ? ctx.r13.u32 : 1u);
        for (uint32_t spins = 0;; spins++)
        {
            uint32_t expected = 0;
            if (__atomic_load_n(word, __ATOMIC_RELAXED) == 0 &&
                __atomic_compare_exchange_n(word, &expected, owner, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
                return;
            if ((spins & 63) == 63)
            {
                Scheduler::Checkpoint();
                std::this_thread::yield();
            }
            else YieldProcessor();
        }
    }

    void ReleaseGuestSpinLock(uint8_t* base, uint32_t lock)
    {
        if (lock == 0) return;
        __atomic_store_n(reinterpret_cast<uint32_t*>(base + lock), 0u, __ATOMIC_RELEASE);
    }
}

PPC_FUNC(__imp__KeEnterCriticalRegion) {}
PPC_FUNC(__imp__KeLeaveCriticalRegion) {}
// VOID KeAcquireSpinLockAtRaisedIrql(spinlock*)
PPC_FUNC(__imp__KeAcquireSpinLockAtRaisedIrql) { AcquireGuestSpinLock(ctx, base, ctx.r3.u32); }
// VOID KeReleaseSpinLockFromRaisedIrql(spinlock*)
PPC_FUNC(__imp__KeReleaseSpinLockFromRaisedIrql) { ReleaseGuestSpinLock(base, ctx.r3.u32); }
PPC_FUNC(__imp__KeLockL2) { ctx.r3.u32 = 0; }
PPC_FUNC(__imp__KeUnlockL2) {}
PPC_FUNC(__imp__KiApcNormalRoutineNop) { ctx.r3.u32 = 0; }

// KIRQL KeRaiseIrqlToDpcLevel(void)
PPC_FUNC(__imp__KeRaiseIrqlToDpcLevel) { ctx.r3.u32 = 0; }

// VOID KfLowerIrql(KIRQL irql)
PPC_FUNC(__imp__KfLowerIrql) {}

// KIRQL KfAcquireSpinLock(spinlock*): the lock, and the old interrupt level,
// which is always the lowest here.
PPC_FUNC(__imp__KfAcquireSpinLock) { AcquireGuestSpinLock(ctx, base, ctx.r3.u32); ctx.r3.u32 = 0; }

// VOID KfReleaseSpinLock(spinlock*, KIRQL)
PPC_FUNC(__imp__KfReleaseSpinLock) { ReleaseGuestSpinLock(base, ctx.r3.u32); }

// --- Critical sections -----------------------------------------------------

namespace
{
    // Who owns a critical section, in the form the guest writes there.
    //
    // The console stores a pointer to the thread object, not a number, and the
    // title's inline fast path stores exactly that. Storing a host thread id
    // instead means the two sides never recognise each other's ownership: the
    // recursive case is missed and the lock deadlocks on its own holder.
    uint32_t OwnerValue(PPCContext& ctx, uint8_t* base)
    {
        if (ctx.r13.u32 != 0)
        {
            const uint32_t object = Guest::Read32(base, ctx.r13.u32 + 0x100);
            if (object != 0) return object;
        }
        return GetCurrentThreadId();
    }
}

PPC_FUNC(__imp__RtlInitializeCriticalSection)
{
    Kernel::CountImport("RtlInitializeCriticalSection");
    const uint32_t cs = ctx.r3.u32;
    if (cs == 0) return;
    std::lock_guard<std::mutex> lock(Kernel::DispatcherLock());
    memset(Guest::Ptr(cs), 0, 0x1C);
    Guest::Write32(base, cs + CS_LOCK_COUNT, uint32_t(-1));
}

PPC_FUNC(__imp__RtlEnterCriticalSection)
{
    Kernel::CountImport("RtlEnterCriticalSection");
    Scheduler::Release();
    struct Resume { ~Resume() { Scheduler::Acquire(); } } resume;
    const uint32_t cs = ctx.r3.u32;
    if (cs == 0) return;

    // The lock is the lock count, not the owner field.
    //
    // A free critical section holds minus one here, and taking it is a compare
    // and exchange of minus one for zero. Titles inline that fast path and only
    // call the kernel when it fails, so a runtime that tracks ownership in a
    // different field is not sharing a lock with the guest at all: both sides
    // can hold it at once. That is what let one thread walk a list while
    // another was resetting it.
    // The protocol, as the console runs it. A free lock holds minus one. Taking
    // it is an atomic increment: the thread that sees zero has it. Anyone else
    // has already recorded that there is contention, which is how the thread
    // holding it knows to wake somebody on the way out.
    //
    // Doing this in a field of this runtime's own instead left the guest's
    // inline fast path and the kernel path holding different locks, and two
    // threads could be inside the same critical section at once.
    const uint32_t self = OwnerValue(ctx, base);

    if (Guest::AtomicAdd32(base, cs + CS_LOCK_COUNT, 1) == 0)
    {
        Guest::Write32(base, cs + CS_OWNING_THREAD, self);
        Guest::Write32(base, cs + CS_RECURSION_COUNT, 1);
        return;
    }

    if (Guest::Read32(base, cs + CS_OWNING_THREAD) == self)
    {
        Guest::Write32(base, cs + CS_RECURSION_COUNT,
            Guest::Read32(base, cs + CS_RECURSION_COUNT) + 1);
        return;
    }

    // Contended. The holder may release inline without telling this runtime,
    // so the wait re-checks on a timer rather than trusting a wake up.
    std::unique_lock<std::mutex> lock(Kernel::DispatcherLock());
    Kernel::EnterWait(cs, uint32_t(ctx.lr), true);
    int waited = 0;
    for (;;)
    {
        if (Guest::Read32(base, cs + CS_OWNING_THREAD) == 0)
        {
            Guest::Write32(base, cs + CS_OWNING_THREAD, self);
            Guest::Write32(base, cs + CS_RECURSION_COUNT, 1);
            Kernel::LeaveWait();
            return;
        }
        const uint64_t key = cs;
        Kernel::WaitDispatcherOn(lock, &key, 1, std::chrono::steady_clock::now() + std::chrono::milliseconds(1));

        // Two seconds on one critical section is a holder that is not
        // coming back: say which, held by whom, from where.
        if (++waited == 2000)
        {
            const uint32_t owner = Guest::Read32(base, cs + CS_OWNING_THREAD);
            printf("cs: 0x%08X has been held for two seconds by thread object 0x%08X "
                   "(os %u, recursion %u), wanted by 0x%08X (os %lu) from 0x%08X\n",
                cs, owner, owner ? Guest::Read32(base, owner + 0x14C) : 0,
                Guest::Read32(base, cs + CS_RECURSION_COUNT), self, GetCurrentThreadId(),
                uint32_t(ctx.lr));
            fflush(stdout);
            // Once: what every thread was last doing, which says what the
            // holder is waiting for while it holds this.
            static std::atomic<bool> reported{ false };
            if (!reported.exchange(true))
            {
                lock.unlock();
                Kernel::ReportLocks();
                Sampler::SampleNow();
                lock.lock();
            }
        }
    }
}

PPC_FUNC(__imp__RtlTryEnterCriticalSection)
{
    Kernel::CountImport("RtlTryEnterCriticalSection");
    const uint32_t cs = ctx.r3.u32;
    if (cs == 0) { ctx.r3.u32 = 0; return; }

    {
        std::lock_guard<std::mutex> lock(Kernel::DispatcherLock());
        const uint32_t me = OwnerValue(ctx, base);
        if (Guest::CompareExchange32(base, cs + CS_LOCK_COUNT, uint32_t(-1), 0))
        {
            Guest::Write32(base, cs + CS_OWNING_THREAD, me);
            Guest::Write32(base, cs + CS_RECURSION_COUNT, 1);
            ctx.r3.u32 = 1;
            return;
        }
        if (Guest::Read32(base, cs + CS_OWNING_THREAD) == me)
        {
            Guest::Write32(base, cs + CS_RECURSION_COUNT,
                Guest::Read32(base, cs + CS_RECURSION_COUNT) + 1);
            Guest::AtomicAdd32(base, cs + CS_LOCK_COUNT, 1);
            ctx.r3.u32 = 1;
            return;
        }
        ctx.r3.u32 = 0;
        return;
    }

    const uint32_t self = GetCurrentThreadId();
    std::lock_guard<std::mutex> lock(Kernel::DispatcherLock());

    const uint32_t owner = Guest::Read32(base, cs + CS_OWNING_THREAD);
    if (owner != 0 && owner != self) { ctx.r3.u32 = 0; return; }

    Guest::Write32(base, cs + CS_OWNING_THREAD, self);
    Guest::Write32(base, cs + CS_RECURSION_COUNT,
        Guest::Read32(base, cs + CS_RECURSION_COUNT) + 1);
    ctx.r3.u32 = 1;
}

PPC_FUNC(__imp__RtlLeaveCriticalSection)
{
    Kernel::CountImport("RtlLeaveCriticalSection");
    const uint32_t cs = ctx.r3.u32;
    if (cs == 0) return;

    const uint32_t recursion = Guest::Read32(base, cs + CS_RECURSION_COUNT);
    if (recursion > 1)
    {
        Guest::Write32(base, cs + CS_RECURSION_COUNT, recursion - 1);
        Guest::AtomicAdd32(base, cs + CS_LOCK_COUNT, -1);
        return;
    }

    // Ownership goes first, then the count, so a thread that sees the count
    // free never finds a stale owner behind it.
    Guest::Write32(base, cs + CS_RECURSION_COUNT, 0);
    Guest::Write32(base, cs + CS_OWNING_THREAD, 0);
    Guest::AtomicAdd32(base, cs + CS_LOCK_COUNT, -1);
    Kernel::WakeDispatcherForUnlocked(cs);
}

// --- Thread local storage --------------------------------------------------
// One set of slots, because there is one thread.

PPC_FUNC(__imp__KeTlsAlloc)
{
    Kernel::CountImport("KeTlsAlloc");
    std::lock_guard<std::mutex> lock(g_tlsMutex);
    for (uint32_t i = 0; i < TlsSlotCount; i++)
    {
        if (!g_tlsTaken[i])
        {
            g_tlsTaken[i] = true;
            ctx.r3.u32 = i;
            return;
        }
    }
    ctx.r3.u32 = uint32_t(-1);   // TLS_OUT_OF_INDEXES
}

PPC_FUNC(__imp__KeTlsFree)
{
    Kernel::CountImport("KeTlsFree");
    const uint32_t slot = ctx.r3.u32;
    if (slot < TlsSlotCount)
    {
        std::lock_guard<std::mutex> lock(g_tlsMutex);
        g_tlsTaken[slot] = false;
    }
    ctx.r3.u32 = 1;
}

PPC_FUNC(__imp__KeTlsGetValue)
{
    Kernel::CountImport("KeTlsGetValue");
    const uint32_t slot = ctx.r3.u32;
    ctx.r3.u32 = slot < TlsSlotCount ? t_tlsSlots[slot] : 0;
}

PPC_FUNC(__imp__KeTlsSetValue)
{
    Kernel::CountImport("KeTlsSetValue");
    const uint32_t slot = ctx.r3.u32;
    if (slot < TlsSlotCount) t_tlsSlots[slot] = ctx.r4.u32;
    ctx.r3.u32 = 1;
}

// --- Small Rtl helpers -----------------------------------------------------

// VOID RtlFillMemoryUlong(void* destination, SIZE_T length, ULONG pattern)
PPC_FUNC(__imp__RtlFillMemoryUlong)
{
    Kernel::CountImport("RtlFillMemoryUlong");
    const uint32_t destination = ctx.r3.u32;
    const uint32_t length = ctx.r4.u32 & ~3u;
    const uint32_t pattern = ctx.r5.u32;

    for (uint32_t offset = 0; offset < length; offset += 4)
        Guest::Write32(base, destination + offset, pattern);
}

// SIZE_T RtlCompareMemoryUlong(void* source, SIZE_T length, ULONG pattern)
PPC_FUNC(__imp__RtlCompareMemoryUlong)
{
    Kernel::CountImport("RtlCompareMemoryUlong");
    const uint32_t source = ctx.r3.u32;
    const uint32_t length = ctx.r4.u32 & ~3u;
    const uint32_t pattern = ctx.r5.u32;
    uint32_t matched = 0;
    for (uint32_t offset = 0; offset < length; offset += 4)
    {
        if (Guest::Read32(base, source + offset) != pattern) break;
        matched += 4;
    }
    ctx.r3.u32 = matched;
}

// VOID RtlInitAnsiString(ANSI_STRING* target, const char* source)
PPC_FUNC(__imp__RtlInitAnsiString)
{
    Kernel::CountImport("RtlInitAnsiString");
    const uint32_t target = ctx.r3.u32;
    const uint32_t source = ctx.r4.u32;
    if (target == 0) return;

    uint16_t length = 0;
    if (source != 0)
    {
        while (length < 0xFFFE && *(base + source + length) != 0) length++;
    }

    // ANSI_STRING is { WORD length; WORD maximumLength; char* buffer; }
    *reinterpret_cast<volatile uint16_t*>(base + target + 0) = __builtin_bswap16(length);
    *reinterpret_cast<volatile uint16_t*>(base + target + 2) =
        __builtin_bswap16(uint16_t(length == 0 ? 0 : length + 1));
    Guest::Write32(base, target + 4, source);
}

// ULONG RtlNtStatusToDosError(NTSTATUS status)
PPC_FUNC(__imp__RtlNtStatusToDosError)
{
    Kernel::CountImport("RtlNtStatusToDosError");
    const uint32_t status = ctx.r3.u32;

    // The ones that matter are the ones the title branches on: pending is
    // ERROR_IO_PENDING, which its overlapped file layer requires of every
    // read it issues, and end of file, timeout and not found each have a
    // meaning of their own. Anything unknown is what the console says for
    // a status it has no mapping for.
    switch (status)
    {
    case 0x00000000: ctx.r3.u32 = 0; break;       // success
    case 0x00000102: ctx.r3.u32 = 258; break;     // timeout -> WAIT_TIMEOUT
    case 0x00000103: ctx.r3.u32 = 997; break;     // pending -> ERROR_IO_PENDING
    case 0x80000005: ctx.r3.u32 = 234; break;     // buffer overflow -> ERROR_MORE_DATA
    case 0x80000006: ctx.r3.u32 = 18; break;      // no more files
    case 0xC0000002: ctx.r3.u32 = 1; break;       // not implemented
    case 0xC0000005: ctx.r3.u32 = 998; break;     // access violation -> NOACCESS
    case 0xC0000008: ctx.r3.u32 = 6; break;       // invalid handle
    case 0xC000000D: ctx.r3.u32 = 87; break;      // invalid parameter
    case 0xC000000E: ctx.r3.u32 = 2; break;       // no such device
    case 0xC000000F: ctx.r3.u32 = 2; break;       // no such file
    case 0xC0000010: ctx.r3.u32 = 1; break;       // invalid device request
    case 0xC0000011: ctx.r3.u32 = 38; break;      // end of file -> ERROR_HANDLE_EOF
    case 0xC0000017: ctx.r3.u32 = 8; break;       // no memory
    case 0xC0000022: ctx.r3.u32 = 5; break;       // access denied
    case 0xC0000023: ctx.r3.u32 = 122; break;     // buffer too small
    case 0xC0000033: ctx.r3.u32 = 123; break;     // object name invalid
    case 0xC0000034: ctx.r3.u32 = 2; break;       // object name not found
    case 0xC0000035: ctx.r3.u32 = 183; break;     // name collision
    case 0xC0000039: ctx.r3.u32 = 161; break;     // path invalid
    case 0xC000003A: ctx.r3.u32 = 3; break;       // path not found
    case 0xC0000043: ctx.r3.u32 = 32; break;      // sharing violation
    case 0xC000007F: ctx.r3.u32 = 112; break;     // disk full
    case 0xC000009A: ctx.r3.u32 = 1450; break;    // insufficient resources
    case 0xC00000BA: ctx.r3.u32 = 5; break;       // file is a directory
    case 0xC00000BB: ctx.r3.u32 = 50; break;      // not supported
    case 0xC0000101: ctx.r3.u32 = 145; break;     // directory not empty
    case 0xC0000120: ctx.r3.u32 = 995; break;     // cancelled
    default:         ctx.r3.u32 = 317; break;     // ERROR_MR_MID_NOT_FOUND
    }
}

// --- Debug and shutdown ----------------------------------------------------

// ULONG DbgPrint(const char* format, ...)
PPC_FUNC(__imp__DbgPrint)
{
    Kernel::CountImport("DbgPrint");
    // Formatted as the guest's printf would: the driver's report of a hung
    // GPU is lines of a register's name and its value, and with the format
    // alone it said nothing but the addresses of the names.
    const std::string format = Kernel::FormatGuestCall(ctx, base, ctx.r3.u32, 4);

    // At most a few hundred lines a second. The driver's report of a hung
    // GPU walks a list of indirect buffers and prints each one, and when the
    // list is corrupt that is three million lines of "0x0 0x0", an 800 MB
    // log, and a title that spends its time inside printf rather than
    // running. What it says is still kept; how often is not its call.
    static std::atomic<uint64_t> printed{ 0 };
    static std::atomic<uint64_t> dropped{ 0 };
    static auto second = std::chrono::steady_clock::now();
    static std::atomic<uint32_t> thisSecond{ 0 };
    {
        const auto now = std::chrono::steady_clock::now();
        if (now - second >= std::chrono::seconds(1))
        {
            const uint32_t lost = thisSecond.exchange(0);
            second = now;
            if (lost > 300)
                printf("guest: (%u more lines from the title this second were not printed)\n",
                    lost - 300);
        }
    }
    if (!format.empty())
    {
        if (thisSecond.fetch_add(1) < 300)
        {
            printf("guest: %s", format.c_str());
            printed.fetch_add(1, std::memory_order_relaxed);
        }
        else
            dropped.fetch_add(1, std::memory_order_relaxed);
    }
    ctx.r3.u32 = X_STATUS_SUCCESS;
}

PPC_FUNC(__imp__DbgBreakPoint)
{
    Kernel::CountImport("DbgBreakPoint");
    // The chain of callers too: the break is the title's own assertion,
    // reached through one wrapper from everywhere, and which one failed is
    // two or three frames up. The prologues save the return address eight
    // bytes below each back chain pointer.
    printf("\nguest: DbgBreakPoint at 0x%08X, from", uint32_t(ctx.lr));
    uint32_t frame = ctx.r1.u32;
    for (int depth = 0; depth < 12; depth++)
    {
        if (frame < 0x10000 || frame >= 0xC0000000u) break;
        const uint32_t caller = Guest::Read32(base, frame);
        if (caller <= frame || caller - frame > 0x100000 || (caller & 7) != 0 || caller >= 0xC0000000u) break;
        const uint32_t address = Guest::Read32(base, caller - 8);
        if (address < 0x82000000u || address >= 0x8A000000u) break;
        printf(" %08X", address);
        frame = caller;
    }
    printf("\n");
    fflush(stdout);
}

// VOID KeBugCheck(ULONG code)
PPC_FUNC(__imp__KeBugCheck)
{
    Kernel::CountImport("KeBugCheck");
    Stop("KeBugCheck", ctx.r3.u32);
}

// VOID KeBugCheckEx(ULONG code, ULONG p1, ULONG p2, ULONG p3, ULONG p4)
PPC_FUNC(__imp__KeBugCheckEx)
{
    Kernel::CountImport("KeBugCheckEx");
    printf("\n  parameters: 0x%08X 0x%08X 0x%08X 0x%08X\n",
        ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32);
    Stop("KeBugCheckEx", ctx.r3.u32);
}

// VOID HalReturnToFirmware(ULONG routine)
PPC_FUNC(__imp__HalReturnToFirmware)
{
    Kernel::CountImport("HalReturnToFirmware");
    printf("\nThe guest asked to return to the dashboard. Shutting down.\n");
    fflush(stdout);
    Kernel::Exit(0);
}

// NTSTATUS ExRegisterTitleTerminateNotification(void* routine, ULONG create)
PPC_FUNC(__imp__ExRegisterTitleTerminateNotification)
{
    Kernel::CountImport("ExRegisterTitleTerminateNotification");
    // Nothing calls the notification back, because nothing shuts the title
    // down cleanly yet.
    ctx.r3.u32 = X_STATUS_SUCCESS;
}
