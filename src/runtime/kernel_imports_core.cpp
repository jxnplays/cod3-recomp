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

// The guest uses 32-bit pointers. Any pointer handed to us is a guest virtual
// address and must be rebased through `base` before it can be dereferenced.
#define GUEST_ARG_U32(ctx, n) (*reinterpret_cast<const uint32_t*>( \
    reinterpret_cast<const uint8_t*>(&(ctx)) + sizeof(PPCRegister) * (n)))

// Return a guest pointer argument (r3, r4, ... by index).
#define GUEST_PTR(ctx, n) (g_base + GUEST_ARG_U32(ctx, n))

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
            g_base + objectsAddr + i * sizeof(uint32_t));
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
    start->stackBase = static_cast<uint32_t>(
        reinterpret_cast<uintptr_t>(stack) - reinterpret_cast<uintptr_t>(g_base) + stackSize);

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
        *reinterpret_cast<uint32_t*>(g_base + outHandle) = GUEST_HANDLE(h);
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
        *reinterpret_cast<uint32_t*>(g_base + out) = phys;
    }
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__MmMapIoSpace)
{
    // (physicalAddress, length, cacheProtect)
    // The title maps the GPU/HDMI register ranges here. We cannot honour real
    // physical addresses, so hand back a zeroed scratch buffer of the requested
    // length and record that the mapping was faked.
    const uint32_t length = ctx.r4.u32;
    void* mem = VirtualAlloc(nullptr, length ? length : 0x1000,
                             MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (mem == nullptr)
    {
        SET_R3(ctx, 0);
        return;
    }
    std::memset(mem, 0, length ? length : 0x1000);
    std::fprintf(stderr,
        "[cod3] MmMapIoSpace(phys=0x%08X, len=0x%X) -> scratch buffer %p "
        "(hardware MMIO not mapped)\n",
        ctx.r3.u32, length, mem);
    SET_R3(ctx, static_cast<uint32_t>(
        reinterpret_cast<uintptr_t>(mem) - reinterpret_cast<uintptr_t>(g_base)));
}

PPC_FUNC(__imp__MmMapIoSpaceEx)
{
    // Same as MmMapIoSpace but with an explicit section attributes argument.
    const uint32_t length = ctx.r4.u32;
    void* mem = VirtualAlloc(nullptr, length ? length : 0x1000,
                             MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (mem == nullptr)
    {
        SET_R3(ctx, 0);
        return;
    }
    std::memset(mem, 0, length ? length : 0x1000);
    SET_R3(ctx, static_cast<uint32_t>(
        reinterpret_cast<uintptr_t>(mem) - reinterpret_cast<uintptr_t>(g_base)));
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
        *reinterpret_cast<uint32_t*>(g_base + outPtr) = resolved;

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
    // The format string is a guest pointer into .rdata; decode it without
    // handing untrusted guest data to printf.
    const char* fmt = reinterpret_cast<const char*>(GUEST_PTR(ctx, 0));
    if (fmt == nullptr || reinterpret_cast<uintptr_t>(fmt) <
        reinterpret_cast<uintptr_t>(g_base))
        return;

    std::fputs("[xbox] ", stderr);
    std::fputc('\n', stderr);
    (void)fmt;
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
    if (width)  *reinterpret_cast<uint32_t*>(g_base + ctx.r3.u32) = dw;
    if (ctx.r4.u32) *reinterpret_cast<uint32_t*>(g_base + ctx.r4.u32) = dh;
    if (ctx.r5.u32) *reinterpret_cast<uint32_t*>(g_base + ctx.r5.u32) = dr;
    if (ctx.r6.u32) *reinterpret_cast<uint32_t*>(g_base + ctx.r6.u32) = df;
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
        std::memset(g_base + out, 0, 0x28);
        *reinterpret_cast<uint32_t*>(g_base + out + 0x00) = dw;
        *reinterpret_cast<uint32_t*>(g_base + out + 0x04) = dh;
        *reinterpret_cast<uint32_t*>(g_base + out + 0x08) = 0;   // non-interlaced
        *reinterpret_cast<uint32_t*>(g_base + out + 0x0C) = 0;
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
        std::memset(g_base + out, 0, 0x20);
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
        *reinterpret_cast<uint32_t*>(g_base + out) = 0x000A0000u; // gamma 1.0
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__VdSetDisplayGamma)
{
    ctx.r3.u64 = 0;
}