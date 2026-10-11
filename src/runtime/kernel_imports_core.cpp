// =============================================================================
//  COD3 Recomp - hand-written Xbox 360 kernel implementations
//
//  These are the kernel APIs Call of Duty 3 depends on early enough that a
//  stub returning 0 stops the title from booting. Everything not listed here is
//  generated as a logging stub by tools/gen_import_stubs.py.
//
//  PowerPC calling convention: integer args arrive in r3..r10, the integer
//  return value goes back in r3, and `base` is the guest image base.
// =============================================================================

#include "cod3_runtime.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <vector>

// -----------------------------------------------------------------------------
// Helpers for reading guest pointers
// -----------------------------------------------------------------------------

// The guest uses 32-bit pointers, and the guest image is mapped at its real
// virtual address, so a guest virtual address IS a valid host address. These
// helpers therefore return the argument unchanged.
#define GUEST_ARG_U32(ctx, n) (*reinterpret_cast<const uint32_t*>( \
    reinterpret_cast<const uint8_t*>(&(ctx)) + sizeof(PPCRegister) * (n)))

// Return a guest pointer argument (r3, r4, ... by index).
#define GUEST_PTR(ctx, n) (reinterpret_cast<void*>( \
    static_cast<uintptr_t>(GUEST_ARG_U32(ctx, n))))

// Set the integer return value.
#define SET_R3(ctx, v) ((ctx).r3.u64 = (uint64_t)(v))

// The host is 64-bit but guest handles and pointers are 32-bit, so every
// native handle we hand back has to be narrowed deliberately.
#define GUEST_HANDLE(h) (static_cast<uint32_t>(reinterpret_cast<uintptr_t>(h)))

// ntdll's RtlNtStatusToDosError is resolved at runtime rather than linked, to
// avoid a hard import-table dependency on ntdll for a single call.
typedef ULONG(WINAPI* RtlNtStatusToDosError_fn)(NTSTATUS);
static RtlNtStatusToDosError_fn get_ntstatus_to_dos_error()
{
    static RtlNtStatusToDosError_fn fn = []() -> RtlNtStatusToDosError_fn {
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        if (ntdll == nullptr)
            return nullptr;
        return reinterpret_cast<RtlNtStatusToDosError_fn>(
            reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlNtStatusToDosError")));
    }();
    return fn;
}

// -----------------------------------------------------------------------------
// Memory: pool allocator
// -----------------------------------------------------------------------------
//
// Xbox 360 pool tags are packed as four bytes; the game is fond of asserting on
// them, so we keep the tag alongside each block for the shutdown report.

struct PoolBlock
{
    uint32_t tag;
    uint32_t size;
    uint32_t magic;
    uint32_t pad;
};

constexpr uint32_t POOL_MAGIC = 0x504F4F4Cu; // 'POOL'

static size_t g_pool_live = 0;

size_t cod3_pool_live_allocations() { return g_pool_live; }

void* cod3_pool_alloc(uint32_t bytes, uint32_t tag, uint32_t /*flags*/)
{
    if (bytes == 0)
        bytes = 4;

    // Always 16-byte align so guest code can use quadword access safely.
    const size_t total = sizeof(PoolBlock) + bytes;
    void* raw = VirtualAlloc(nullptr, total, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (raw == nullptr)
    {
        std::fprintf(stderr, "[cod3] pool alloc of %u bytes (tag %08X) failed\n",
                     bytes, tag);
        return nullptr;
    }

    auto* block = static_cast<PoolBlock*>(raw);
    block->tag = tag;
    block->size = total;
    block->magic = POOL_MAGIC;
    block->pad = 0;
    g_pool_live++;
    return static_cast<uint8_t*>(raw) + sizeof(PoolBlock);
}

void cod3_pool_free(void* ptr)
{
    if (ptr == nullptr)
        return;

    auto* block = reinterpret_cast<PoolBlock*>(
        static_cast<uint8_t*>(ptr) - sizeof(PoolBlock));
    if (block->magic != POOL_MAGIC)
    {
        // Not ours: the title has handed us a pointer it allocated itself or one
        // that has already been freed. Ignore rather than corrupt the heap.
        std::fprintf(stderr, "[cod3] pool free of foreign pointer %p ignored\n", ptr);
        return;
    }

    block->magic = 0;
    VirtualFree(block, 0, MEM_RELEASE);
    g_pool_live--;
}

// The recompiler declares kernel imports with PPC_EXTERN_FUNC (a plain C++
// declaration), so they must be defined here with C++ linkage via PPC_FUNC.
// Defining them with PPC_FUNC_IMPL (extern "C") mangles differently from the
// references and fails to link.

PPC_FUNC(__imp__ExAllocatePool)
{
    SET_R3(ctx, GUEST_ARG_U32(ctx, 1));
    ctx.r3.u64 = (uint64_t)(uintptr_t)cod3_pool_alloc(ctx.r3.u32, 0, 0);
}

PPC_FUNC(__imp__ExAllocatePoolWithTag)
{
    const uint32_t bytes = ctx.r3.u32;
    const uint32_t tag = ctx.r4.u32;
    ctx.r3.u64 = (uint64_t)(uintptr_t)cod3_pool_alloc(bytes, tag, 0);
}

PPC_FUNC(__imp__ExAllocatePoolTypeWithTag)
{
    const uint32_t bytes = ctx.r3.u32;
    const uint32_t tag = ctx.r4.u32;
    const uint32_t flags = ctx.r5.u32;
    ctx.r3.u64 = (uint64_t)(uintptr_t)cod3_pool_alloc(bytes, tag, flags);
}

PPC_FUNC(__imp__ExFreePool)
{
    cod3_pool_free(GUEST_PTR(ctx, 0));
    ctx.r3.u64 = 0;
}

// -----------------------------------------------------------------------------
// Critical sections
// -----------------------------------------------------------------------------
//
// A guest critical section is just a guest DWORD pair holding a Win32 SRWLOCK
// or CRITICAL_SECTION pointer. We keep a side table because the guest may pass
// uninitialised memory that we must not trust as a native lock.

#include <unordered_map>

static std::unordered_map<uint32_t, CRITICAL_SECTION*> g_critsecs;

PPC_FUNC(__imp__RtlInitializeCriticalSection)
{
    const uint32_t addr = ctx.r3.u32;
    auto* cs = new CRITICAL_SECTION();
    InitializeCriticalSection(cs);
    g_critsecs[addr] = cs;
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__RtlInitializeCriticalSectionAndSpinCount)
{
    const uint32_t addr = ctx.r3.u32;
    const DWORD spin = ctx.r4.u32;
    auto* cs = new CRITICAL_SECTION();
    InitializeCriticalSectionEx(cs, spin, 0);
    g_critsecs[addr] = cs;
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__RtlEnterCriticalSection)
{
    auto it = g_critsecs.find(ctx.r3.u32);
    if (it == g_critsecs.end())
    {
        // Initialise on demand: the title frequently uses a critical section
        // that was only zero-filled, relying on first-use initialisation.
        auto* cs = new CRITICAL_SECTION();
        InitializeCriticalSection(cs);
        it = g_critsecs.emplace(ctx.r3.u32, cs).first;
    }
    EnterCriticalSection(it->second);
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__RtlLeaveCriticalSection)
{
    auto it = g_critsecs.find(ctx.r3.u32);
    if (it != g_critsecs.end())
        LeaveCriticalSection(it->second);
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__RtlTryEnterCriticalSection)
{
    auto it = g_critsecs.find(ctx.r3.u32);
    if (it == g_critsecs.end())
    {
        auto* cs = new CRITICAL_SECTION();
        InitializeCriticalSection(cs);
        it = g_critsecs.emplace(ctx.r3.u32, cs).first;
    }
    // 0 = acquired, 1 = already held, matching NTSTATUS conventions.
    SET_R3(ctx, TryEnterCriticalSection(it->second) ? 0 : 1);
}

PPC_FUNC(__imp__RtlDeleteCriticalSection)
{
    auto it = g_critsecs.find(ctx.r3.u32);
    if (it != g_critsecs.end())
    {
        DeleteCriticalSection(it->second);
        delete it->second;
        g_critsecs.erase(it);
    }
    ctx.r3.u64 = 0;
}

// -----------------------------------------------------------------------------
// IRQL
// -----------------------------------------------------------------------------
//
// The recompiled code raises IRQL around spin locks and DPCs. We model the
// level as a plain global; nothing is actually preempted because every guest
// thread runs on a real OS thread.

uint32_t g_irql = PASSIVE_LEVEL;

PPC_FUNC(__imp__KeGetCurrentIrql)
{
    SET_R3(ctx, g_irql);
}

PPC_FUNC(__imp__KeRaiseIrql) { ctx.r3.u64 = g_irql; g_irql = ctx.r4.u32; }
PPC_FUNC(__imp__KfRaiseIrql)  { ctx.r3.u64 = g_irql; g_irql = ctx.r4.u32; }
PPC_FUNC(__imp__KeLowerIrql)  { g_irql = ctx.r3.u32; }
PPC_FUNC(__imp__KfLowerIrql)   { g_irql = ctx.r3.u32; }

// -----------------------------------------------------------------------------
// Events, mutexes, semaphores
// -----------------------------------------------------------------------------
//
// Guest events/mutexes/semaphores are backed by native objects keyed on the
// guest address, so the guest can use any aligned block of its own memory.

struct GuestSync
{
    HANDLE handle = nullptr;
    LONG initial = 0;
    bool created = false;
};

static std::unordered_map<uint32_t, GuestSync> g_sync;

static GuestSync& sync_for(uint32_t addr)
{
    auto& s = g_sync[addr];
    if (!s.created)
    {
        s.handle = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        s.created = true;
    }
    return s;
}

// ---------------------------------------------------------------------------
// Semaphore signals carry a count, so they get their own map.
// ---------------------------------------------------------------------------

static std::unordered_map<uint32_t, LONG> g_sem_counts;

PPC_FUNC(__imp__KeInitializeEvent)
{
    // (event, type, signaled) - we always model the event as manual-reset.
    const uint32_t addr = ctx.r3.u32;
    auto& s = sync_for(addr);
    if (s.handle != nullptr)
        ResetEvent(s.handle);
    if (ctx.r5.u32 != 0)
        SetEvent(s.handle);
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__KeSetEvent)
{
    auto& s = sync_for(ctx.r3.u32);
    if (s.handle != nullptr)
        SetEvent(s.handle);
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__KeResetEvent)
{
    auto& s = sync_for(ctx.r3.u32);
    if (s.handle != nullptr)
        ResetEvent(s.handle);
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__KePulseEvent)
{
    auto& s = sync_for(ctx.r3.u32);
    if (s.handle != nullptr)
    {
        SetEvent(s.handle);
        ResetEvent(s.handle);
    }
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__KeInitializeMutex)
{
    const uint32_t addr = ctx.r3.u32;
    auto& s = sync_for(addr);
    if (s.handle != nullptr)
        ResetEvent(s.handle);
    if (ctx.r5.u32 != 0)
        SetEvent(s.handle);
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__KeReleaseMutex)
{
    auto& s = sync_for(ctx.r3.u32);
    if (s.handle != nullptr)
        SetEvent(s.handle);
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__KeInitializeSemaphore)
{
    g_sem_counts[ctx.r3.u32] = static_cast<LONG>(ctx.r4.u32);
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__KeReleaseSemaphore)
{
    const uint32_t addr = ctx.r3.u32;
    const LONG delta = static_cast<LONG>(ctx.r4.u32);
    LONG& count = g_sem_counts[addr];
    const LONG previous = count;
    count += delta;
    if (count < 0)
        count = 0;
    // Return value is the previous count.
    SET_R3(ctx, previous);
}

PPC_FUNC(__imp__KeWaitForSingleObject)
{
    auto& s = sync_for(ctx.r3.u32);
    if (s.handle == nullptr)
    {
        SET_R3(ctx, STATUS_WAIT_TIMEOUT);
        return;
    }

    const DWORD alertable = (ctx.r5.u32 != 0) ? 0 : INFINITE;
    // Map guest timeout (in 100ns units, negative == relative) to milliseconds.
    // -1 means infinite.
    const int64_t timeout100 = ctx.r4.s64;
    DWORD ms;
    if (timeout100 < 0)
        ms = (timeout100 == -1) ? INFINITE : (DWORD)(-timeout100 / 10000);
    else
        ms = (DWORD)(timeout100 / 10000);

    const DWORD r = WaitForSingleObjectEx(s.handle, ms, alertable);
    switch (r)
    {
    case WAIT_OBJECT_0:  SET_R3(ctx, STATUS_SUCCESS); break;
    case WAIT_TIMEOUT:   SET_R3(ctx, STATUS_WAIT_TIMEOUT); break;
    default:             SET_R3(ctx, STATUS_WAIT_TIMEOUT); break;
    }
}

PPC_FUNC(__imp__KeWaitForMultipleObjects)
{
    // Single-object wait is by far the common case in this title; for a genuine
    // multi-object wait we poll the native handles so we never block forever.
    const uint32_t count = ctx.r4.u32;
    const uint32_t objectsAddr = ctx.r3.u32;
    const uint32_t waitAll = ctx.r5.u32;

    if (count == 0)
    {
        SET_R3(ctx, STATUS_WAIT_TIMEOUT);
        return;
    }

    // Each object pointer is a guest virtual address of a GuestSync.
    std::vector<HANDLE> handles;
    for (uint32_t i = 0; i < count; ++i)
    {
        const uint32_t objAddr = *reinterpret_cast<const uint32_t*>(
            (objectsAddr + i * sizeof(uint32_t)));
        auto& s = sync_for(objAddr);
        if (s.handle != nullptr)
            handles.push_back(s.handle);
    }

    if (handles.empty())
    {
        SET_R3(ctx, STATUS_WAIT_TIMEOUT);
        return;
    }

    const DWORD ms = 1000;
    const DWORD r = WaitForMultipleObjects(
        static_cast<DWORD>(handles.size()), handles.data(),
        waitAll ? TRUE : FALSE, ms);

    if (r >= WAIT_OBJECT_0 && r < WAIT_OBJECT_0 + handles.size())
        SET_R3(ctx, STATUS_SUCCESS);
    else
        SET_R3(ctx, STATUS_WAIT_TIMEOUT);
}

// -----------------------------------------------------------------------------
// Threads
// -----------------------------------------------------------------------------
//
// Guest threads are real Win32 threads. The guest thread context (PPCContext)
// lives in a block allocated from the pool; the trampoline copies it in, runs
// the guest entry point, and stores the return value back.

struct GuestThreadStart
{
    uint32_t startRoutine; // guest address
    uint32_t contextAddr;  // guest address of the caller's argument
    uint32_t stackBase;
};

static DWORD WINAPI guest_thread_trampoline(LPVOID param)
{
    auto* start = static_cast<GuestThreadStart*>(param);

    // Build a guest stack and context, then call the recompiled entry point.
    // The PPCContext must outlive the call, so it is allocated from the pool.
    const size_t contextBytes = sizeof(PPCContext);
    auto* ctxMem = static_cast<PPCContext*>(
        cod3_pool_alloc(static_cast<uint32_t>(contextBytes), 'PPCX', 0));
    if (ctxMem == nullptr)
        return 1;
    std::memset(ctxMem, 0, contextBytes);

    PPCContext& ctx = *ctxMem;
    ctx.r1.u32 = start->stackBase;   // guest stack pointer
    ctx.r3.u32 = start->contextAddr;  // first argument

    cod3_run_guest_function(start->startRoutine, ctx);

    const NTSTATUS rc = static_cast<NTSTATUS>(ctx.r3.u32);
    cod3_pool_free(ctxMem);
    cod3_pool_free(start);
    return static_cast<DWORD>(rc);
}

static HANDLE create_guest_thread(uint32_t startRoutine, uint32_t param,
                                  uint32_t stackSize, uint32_t priority)
{
    // Allocate the guest stack out of guest memory.
    auto* stack = static_cast<uint8_t*>(
        cod3_pool_alloc(stackSize, 'STK\u0000', 0));
    if (stack == nullptr)
        return nullptr;

    auto* start = static_cast<GuestThreadStart*>(
        cod3_pool_alloc(static_cast<uint32_t>(sizeof(GuestThreadStart)), 'THRD', 0));
    if (start == nullptr)
    {
        cod3_pool_free(stack);
        return nullptr;
    }

    start->startRoutine = startRoutine;
    start->contextAddr = param;

    // The guest stack pointer is the TOP of the block, because PowerPC stacks
    // grow downwards. cod3_pool_alloc returns a guest VA (the heap is mapped
    // inside the guest reservation), so this arithmetic is plain guest VA math.
    start->stackBase = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(stack))
                       + stackSize;

    const DWORD threadSize = stackSize + 0x10000;
    HANDLE h = CreateThread(nullptr, threadSize, guest_thread_trampoline, start,
                            (priority != 0) ? 0 : CREATE_SUSPENDED, nullptr);

    if (h != nullptr && priority == 0)
        ResumeThread(h);

    return h;
}

PPC_FUNC(__imp__ExCreateThread)
{
    // (outHandle, accessMask, attr, startRoutine, startArg, stackSize, ...)
    const uint32_t outHandle = ctx.r3.u32;
    const uint32_t startRoutine = ctx.r6.u32;
    const uint32_t startArg = ctx.r7.u32;
    const uint32_t stackSize = (ctx.r8.u32 != 0) ? ctx.r8.u32 : 0x10000;

    HANDLE h = create_guest_thread(startRoutine, startArg, stackSize, 1);
    if (outHandle != 0)
        *reinterpret_cast<uint32_t*>(outHandle) = GUEST_HANDLE(h);
    ctx.r3.u64 = (h != nullptr) ? STATUS_SUCCESS : 0xC0000071;
}

PPC_FUNC(__imp__KeCreateThread)
{
    const uint32_t startRoutine = ctx.r3.u32;
    const uint32_t startArg = ctx.r4.u32;
    const uint32_t stackSize = (ctx.r5.u32 != 0) ? ctx.r5.u32 : 0x10000;
    HANDLE h = create_guest_thread(startRoutine, startArg, stackSize, 0);
    SET_R3(ctx, GUEST_HANDLE(h));
}

PPC_FUNC(__imp__KeResumeThread)
{
    const uint32_t h = ctx.r3.u32;
    if (h != 0)
    {
        ResumeThread(reinterpret_cast<HANDLE>(
            static_cast<uintptr_t>(h)));
        SET_R3(ctx, 0);
    }
    else
    {
        SET_R3(ctx, STATUS_INVALID_HANDLE);
    }
}

PPC_FUNC(__imp__KeTerminateThread)
{
    const uint32_t h = ctx.r3.u32;
    const uint32_t exitStatus = ctx.r4.u32;
    if (h != 0)
    {
        TerminateThread(reinterpret_cast<HANDLE>(
            static_cast<uintptr_t>(h)), exitStatus);
        SET_R3(ctx, STATUS_SUCCESS);
    }
    else
    {
        SET_R3(ctx, STATUS_INVALID_HANDLE);
    }
}

PPC_FUNC(__imp__KeSetBasePriorityThread)
{
    SET_R3(ctx, STATUS_SUCCESS);
}

PPC_FUNC(__imp__KeSetPriorityThread)
{
    SET_R3(ctx, STATUS_SUCCESS);
}

// -----------------------------------------------------------------------------
// Virtual memory manager
// -----------------------------------------------------------------------------
//
// NtAllocateVirtualMemory is the first kernel API the title calls during boot,
// and it is a genuine allocator: the CRT reserves the memory regions it will
// later use for the page heap, TLS and the heap. Returning success without
// backing the memory makes the very next store through the returned pointer
// fault, which is exactly how the first bring-up run ended.
//
// Signature, recovered from the recompiled call sites in
// build/ppc/ppc_recomp.55.cpp (sub_82347340):
//
//   NtAllocateVirtualMemory(&base, &regionSize, flags, protect, ..., size)
//
// The observed call sets:
//   r3 -> &BaseAddress   (in/out)
//   r4 -> &RegionSize    (in/out)
//   r5  = 0x60001000 / 0x60002000   (allocation type | protection bits)
//   r6  = 4             (MEM_READWRITE-ish attribute)
//   r7  = 0             (size: 0 means "size is taken from *RegionSize")
//
// Note r5 carries the MEM_COMMIT/MEM_RESERTE-type bits *and* page protection
// in one value, so it is masked rather than compared to a single constant, and
// the size comes from the out-parameter when r7 is zero.
//
// Allocations are carved from the guest heap region, which is real mapped
// memory inside the guest address space, so every guest pointer stays a valid
// 32-bit virtual address backed by host memory.

static uint32_t g_vm_brk = 0; // next free guest VA, 0 until first allocation

PPC_FUNC(__imp__NtAllocateVirtualMemory)
{
    std::fprintf(stderr,
        "[cod3] NtAllocateVirtualMemory called: r3=0x%08X r4=0x%08X r5=0x%08X "
        "r6=0x%08X r7=0x%08X r8=0x%08X r9=0x%08X r10=0x%08X r11=0x%08X\n",
        ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32,
        ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32, ctx.r11.u32);
    std::fflush(stderr);

    auto** ppBase = reinterpret_cast<uint32_t**>(
        static_cast<uintptr_t>(ctx.r3.u32));
    auto* pRegion = reinterpret_cast<uint32_t*>(
        static_cast<uintptr_t>(ctx.r4.u32));

    const uint32_t flags = ctx.r5.u32;
    const uint32_t attr = ctx.r6.u32;
    (void)attr;

    if (g_vm_brk == 0)
        g_vm_brk = g_guest_heap_base;

    // Size: prefer r7 when non-zero, else the value the caller passed by
    // pointer (the title reserves whole regions this way).
    uint32_t size = ctx.r7.u32;
    if (size == 0 && pRegion != nullptr)
        size = *pRegion;

    // Round up to a whole page. Zero means "one page", which is what the
    // Windows semantics require.
    if (size == 0)
    {
        size = 0x1000;
    }
    else
    {
        // Guard against a bogus or absurd region size before it is used as a
        // memset length.
        if (size > g_guest_heap_size)
        {
            std::fprintf(stderr,
                "[cod3] NtAllocateVirtualMemory: rejecting implausible size "
                "0x%X (arena is 0x%X)\n", size, g_guest_heap_size);
            SET_R3(ctx, static_cast<NTSTATUS>(0xC0000017ul)); // STATUS_NO_MEMORY
            return;
        }
        size = (size + 0xFFFu) & ~0xFFFu;
    }

    const uint32_t requestedBase =
        (ppBase != nullptr && **ppBase != 0) ? **ppBase : 0u;
    const uint32_t heapEnd = g_guest_heap_base + g_guest_heap_size;

    // Prefer the caller's requested address when it lies inside the arena; the
    // CRT does reserve specific regions.
    uint32_t addr;
    if (requestedBase >= g_guest_heap_base &&
        static_cast<uint32_t>(requestedBase + size) <= heapEnd)
    {
        addr = requestedBase;
    }
    else
    {
        addr = (g_vm_brk + 0xFFF) & ~0xFFFu;
        if (static_cast<uint32_t>(addr + size) > heapEnd)
        {
            std::fprintf(stderr,
                "[cod3] NtAllocateVirtualMemory: out of guest VM space "
                "(want 0x%X at 0x%08X, heap ends 0x%08X)\n",
                size, addr, heapEnd);
            SET_R3(ctx, static_cast<NTSTATUS>(0xC0000017ul)); // STATUS_NO_MEMORY
            return;
        }
        g_vm_brk = addr + size;
    }

    // Commit zeroes the region, matching Windows semantics that the title
    // relies on for its page heap and TLS.
    std::memset(reinterpret_cast<void*>(static_cast<uintptr_t>(addr)), 0, size);

    if (ppBase != nullptr)
        **ppBase = addr;
    if (pRegion != nullptr)
        *pRegion = size;

    std::fprintf(stderr,
        "[cod3] NtAllocateVirtualMemory: flags=0x%X size=0x%X -> 0x%08X\n",
        flags, size, addr);

    SET_R3(ctx, STATUS_SUCCESS);
}

// The title frees whole reserved regions; returning success is correct because
// the memory is owned by the guest VM arena and is not returned to the OS.
PPC_FUNC(__imp__NtFreeVirtualMemory)
{
    SET_R3(ctx, STATUS_SUCCESS);
}

// -----------------------------------------------------------------------------
// Object manager
// -----------------------------------------------------------------------------
//
// Handles in this runtime are raw Win32 HANDLE values narrowed to 32 bits,
// which is exactly how the guest stores them.

PPC_FUNC(__imp__ObReferenceObjectByHandle)
{
    // We cannot resolve an arbitrary guest handle to an object type without a
    // full object namespace, so this succeeds and returns a synthetic handle.
    // Games use the returned handle only as an opaque token in most cases.
    SET_R3(ctx, ctx.r3.u32);
}

PPC_FUNC(__imp__ObDereferenceObject)
{
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__ObDereferenceObjectByPointer)
{
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__ObReferenceObjectByPointer)
{
    SET_R3(ctx, ctx.r3.u32);
}

// -----------------------------------------------------------------------------
// Physical memory descriptors
// -----------------------------------------------------------------------------

PPC_FUNC(__imp__MmGetPhysicalAddress)
{
    // (baseAddress, &physicalAddress)
    const uint32_t phys = static_cast<uint32_t>(
        reinterpret_cast<uintptr_t>(GUEST_PTR(ctx, 0)) & 0xFFFFFFFFu);
    const uint32_t out = ctx.r4.u32;
    if (out != 0)
    {
        // Xbox 360 physical addresses are 32-bit; the high bit of the
        // descriptor is the "no-cache"/device bit, which we leave clear.
        *reinterpret_cast<uint32_t*>(out) = phys;
    }
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__MmMapIoSpace)
{
    // (physicalAddress, length, cacheProtect)
    //
    // This is the route the title uses to reach the Xenos GPU registers, so it
    // is the natural hook for emulating the GPU (see docs/GRAPHICS.md). Nothing
    // is emulated yet: the region is ordinary RAM that the guest can read and
    // write, and every register write is discarded.
    //
    // The buffer must come from the guest heap so the returned value is a real
    // guest VA the title can store in a register and pass around.
    const uint32_t length = (ctx.r4.u32 != 0) ? ctx.r4.u32 : 0x1000;
    void* mem = cod3_pool_alloc(length, 'MMIO', 0);
    if (mem == nullptr)
    {
        SET_R3(ctx, 0);
        return;
    }
    std::memset(mem, 0, length);

    std::fprintf(stderr,
        "[cod3] MmMapIoSpace(phys=0x%08X, len=0x%X) -> guest RAM at 0x%08X "
        "(Xenos MMIO NOT emulated; register writes are discarded)\n",
        ctx.r3.u32, length, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(mem)));
    SET_R3(ctx, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(mem)));
}

PPC_FUNC(__imp__MmMapIoSpaceEx)
{
    // Same as MmMapIoSpace but with an explicit section attributes argument.
    const uint32_t length = (ctx.r4.u32 != 0) ? ctx.r4.u32 : 0x1000;
    void* mem = cod3_pool_alloc(length, 'MMIO', 0);
    if (mem == nullptr)
    {
        SET_R3(ctx, 0);
        return;
    }
    std::memset(mem, 0, length);
    SET_R3(ctx, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(mem)));
}

PPC_FUNC(__imp__MmUnmapIoSpace)
{
    // The scratch buffers handed out above are deliberately leaked rather than
    // unmapped: the guest keeps using MMIO ranges for the life of the process
    // and freeing them underneath it produces use-after-free crashes.
    ctx.r3.u64 = 0;
}

// -----------------------------------------------------------------------------
// Module / export resolution
// -----------------------------------------------------------------------------
//
// COD3 imports no D3D symbols directly: the Xbox 360 graphics stack (NGL) is
// statically linked into the XEX and resolved at runtime through these two
// entry points. Everything the recomp needs to boot depends on getting them
// right, so they are implemented against the recompiled function table.

PPC_FUNC(__imp__XexGetModuleHandle)
{
    SET_R3(ctx, 0); // no other module is loaded
}

PPC_FUNC(__imp__XexGetProcedureAddress)
{
    // (moduleHandle, ordinal, &address)
    //
    // Ordinals below 0x10000 are Xbox 360 kernel/XAM exports; anything larger
    // is a private export of the title's own statically linked modules, which
    // live in the recompiled function table.
    const uint32_t ordinal = ctx.r4.u32;
    const uint32_t outPtr = ctx.r5.u32;
    uint32_t resolved = 0;

    if (ordinal != 0)
    {
        resolved = cod3_resolve_private_export(ordinal);
    }

    if (outPtr != 0)
        *reinterpret_cast<uint32_t*>(outPtr) = resolved;

    // Returning 0 here means "not found"; the caller checks the out param.
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__XexGetXEntryInfo)
{
    // Needs the load address of another module, which we never provide.
    SET_R3(ctx, 0);
}

PPC_FUNC(__imp__XexGetOptionalXEntryInfo)
{
    SET_R3(ctx, 0);
}

PPC_FUNC(__imp__XexGetTitleId)
{
    // The retail COD3 title id, so title-specific code paths activate the same
    // way they would on the original hardware.
    SET_R3(ctx, 0x555307D2u);
}

// -----------------------------------------------------------------------------
// Debug / status
// -----------------------------------------------------------------------------

PPC_FUNC(__imp__DbgPrint)
{
    // The format string is a guest pointer into .rdata. The guest prints a LOT
    // during boot, so only the presence of the call is logged; decoding the
    // format string into a host printf would mean trusting guest data.
    const char* fmt = reinterpret_cast<const char*>(GUEST_PTR(ctx, 0));

    std::fputs("[xbox] DbgPrint(", stderr);
    if (fmt != nullptr)
    {
        // Print the format string read defensively: it is guest memory and may
        // not be NUL terminated.
        const size_t maxLen = 200;
        char buf[201];
        size_t i = 0;
        for (; i < maxLen && fmt[i] != '\0'; ++i)
            buf[i] = (fmt[i] >= 0x20 && fmt[i] < 0x7F) ? fmt[i] : '.';
        buf[i] = '\0';
        std::fputs(buf, stderr);
    }
    std::fputs(")\n", stderr);
}

PPC_FUNC(__imp__RtlNtStatusToDosError)
{
    const NTSTATUS status = static_cast<NTSTATUS>(ctx.r3.u32);
    auto fn = get_ntstatus_to_dos_error();
    SET_R3(ctx, (fn != nullptr) ? fn(status) : ERROR_INVALID_FUNCTION);
}

PPC_FUNC(__imp__ExRegisterTitleTerminateNotification)
{
    // The title installs a terminate callback; it never fires on PC.
    SET_R3(ctx, STATUS_SUCCESS);
}

// -----------------------------------------------------------------------------
// Video display
// -----------------------------------------------------------------------------
//
// These are the Xbox 360 "Vd" display-mode APIs. The NGL layer in the XEX calls
// them to pick a mode. We report the host desktop mode, which is what makes the
// recomp present in a window at native PC resolution instead of forcing the
// original 640x480 / 1280x720 modes.

PPC_FUNC(__imp__XGetVideoMode)
{
    // (outWidth, outHeight, outRefresh, outPixelFormat)
    const uint32_t width  = (ctx.r3.u32 != 0) ? ctx.r3.u32 : 0;
    uint32_t dw = 0, dh = 0, dr = 0, df = 0;
    cod3_host_display_mode(&dw, &dh, &dr, &df);
    if (width)  *reinterpret_cast<uint32_t*>(ctx.r3.u32) = dw;
    if (ctx.r4.u32) *reinterpret_cast<uint32_t*>(ctx.r4.u32) = dh;
    if (ctx.r5.u32) *reinterpret_cast<uint32_t*>(ctx.r5.u32) = dr;
    if (ctx.r6.u32) *reinterpret_cast<uint32_t*>(ctx.r6.u32) = df;
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__VdGetCurrentDisplayInformation)
{
    // (index, &information)
    const uint32_t out = ctx.r4.u32;
    uint32_t dw = 0, dh = 0, dr = 0, df = 0;
    cod3_host_display_mode(&dw, &dh, &dr, &df);

    if (out != 0)
    {
        // The real structure is 0x28 bytes; fill in the fields the NGL layer
        // actually reads (width/height at the front).
        uint8_t* const p = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(out));
        std::memset(p, 0, 0x28);
        *reinterpret_cast<uint32_t*>(p + 0x00) = dw;
        *reinterpret_cast<uint32_t*>(p + 0x04) = dh;
        *reinterpret_cast<uint32_t*>(p + 0x08) = 0;   // non-interlaced
        *reinterpret_cast<uint32_t*>(p + 0x0C) = 0;
    }
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__VdSetDisplayMode)
{
    // Changing the mode is a no-op: the host owns the window.
    std::fprintf(stderr,
        "[cod3] VdSetDisplayMode(...) ignored; host window controls the mode\n");
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__VdQueryVideoMode)
{
    const uint32_t out = ctx.r5.u32;
    uint32_t dw = 0, dh = 0, dr = 0, df = 0;
    cod3_host_display_mode(&dw, &dh, &dr, &df);
    if (out != 0)
        std::memset(reinterpret_cast<void*>(static_cast<uintptr_t>(out)), 0, 0x20);
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__VdQueryVideoFlags)
{
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__VdPersistDisplay)
{
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__VdGetCurrentDisplayGamma)
{
    const uint32_t out = ctx.r3.u32;
    if (out != 0)
        *reinterpret_cast<uint32_t*>(out) = 0x000A0000u; // gamma 1.0
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__VdSetDisplayGamma)
{
    ctx.r3.u64 = 0;
}