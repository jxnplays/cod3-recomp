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
#include <cstdlib>
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
// Memory: loader allocator
// -----------------------------------------------------------------------------
//
// Xbox 360 pool tags are packed as four bytes. The tag is still passed through
// for the shutdown report, but it is no longer stored in a header.
//
// That header used to be a PoolBlock in front of every payload, host byte
// order, which only worked while allocations came from VirtualAlloc. They now
// come from the guest heap, so there is nothing in front of the payload: the
// guest dereferences these addresses with its own load/store macros and any
// header would be part of the guest's view of its own heap.
//
// The allocation itself lives in ppc_runtime.cpp, next to g_heap_ptr, because it
// is a bump pointer over the mapped heap region and needs those definitions.
// This is the thin wrapper the rest of the runtime calls.

static size_t g_pool_live = 0;

size_t cod3_pool_live_allocations() { return g_pool_live; }

void* cod3_pool_alloc(uint32_t bytes, uint32_t tag, uint32_t /*flags*/)
{
    if (bytes == 0)
        bytes = 4;

    // NOTE: this deliberately allocates from the guest heap region, NOT from
    // VirtualAlloc. The result is a guest virtual address, because the callers
    // hand it to the guest: sub_82352998 stores the pointer into a global, and
    // every later access is a guest load or store at that address. A host
    // pointer would work until the first such access and then fault on an
    // address that is not a guest VA.
    //
    // VirtualAlloc was used here previously. It happened to work for the
    // allocations made during NtAllocateVirtualMemory setup because those
    // results were stored and compared but rarely dereferenced through the
    // guest's own load/store macros. Publishing the CRT thread block through it
    // made that assumption fail immediately: the guest faulted at 0x80180110,
    // which is a host address with the guest's high bits OR'd on.
    if (bytes == 0)
        bytes = 4;

    void* result = cod3_heap_alloc(bytes, tag);
    if (result != nullptr)
        g_pool_live++;
    return result;
}

void cod3_pool_free(void* ptr)
{
    if (ptr == nullptr)
        return;

    // cod3_pool_alloc now carves from the guest heap rather than calling
    // VirtualAlloc, so there is no PoolBlock header in front of the payload and
    // the magic check would read guest memory that belongs to the title.
    //
    // The region is a bump allocator owned by the loader for the life of the
    // process, so there is nothing to release. Freeing is a no-op, which is also
    // what a real loader's boot structures do. Silently ignoring a free is safe
    // here precisely because nothing is reclaimed.
    (void)ptr;
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
        cod3_store_u32(outHandle, GUEST_HANDLE(h));
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

uint32_t* g_vm_brk_slot()
{
    return &g_vm_brk;
}

// The first page this allocator handed out. Recorded so a descriptor the guest
// actually created can be inspected, rather than one inferred from where the
// heap is expected to begin - the loader's own structures occupy the low part of
// that region, so a computed guess reads them instead of the heap.
static uint32_t g_vm_first_page = 0;

uint32_t cod3_first_vm_page()
{
    return g_vm_first_page;
}

// Live regions, so a new allocation never overlaps one already handed out.
// The title stores these addresses in long-lived descriptors, so handing back
// the same address twice corrupts them silently and much later.
struct VmRegion
{
    uint32_t base;
    uint32_t end;
};
static std::vector<VmRegion> g_vm_regions;

// The VM logs its activity, which is useful while bringing the boot path up and
// noisy afterwards. COD3_QUIET_VM=1 silences it.
// Whether kernel-memory chatter reaches stderr. Shared with mm_physical.cpp,
// which allocates through the same region and would otherwise be the only
// allocator that talks unconditionally.
bool cod3_vm_quiet()
{
    static const bool quiet = []() {
        const char* v = std::getenv("COD3_QUIET_VM");
        return !(v != nullptr && v[0] == '1');
    }();
    return quiet;
}

PPC_FUNC(__imp__NtAllocateVirtualMemory)
{
    // r3 points at a caller slot holding the requested BaseAddress, and we write
    // the allocated address back into that same slot. It is a pointer TO a
    // pointer; one dereference yields the slot, two would follow the caller's
    // (still unset) pointer into address 0.
    auto* pBaseSlot = reinterpret_cast<uint32_t*>(
        static_cast<uintptr_t>(ctx.r3.u32));
    auto* pRegion = reinterpret_cast<uint32_t*>(
        static_cast<uintptr_t>(ctx.r4.u32));

    const uint32_t flags = ctx.r5.u32;
    const uint32_t attr = ctx.r6.u32;
    (void)attr;

    if (g_vm_brk == 0)
    {
        // Start above the loader's private region at the base of the heap. The
        // loader keeps structures the CRT must never be told it owns - the CRT
        // thread block among them - and NtAllocateVirtualMemory zeroes what it
        // hands out, so overlapping the two destroys them silently. See
        // cod3_heap_alloc().
        g_vm_brk = g_guest_heap_base + kLoaderRegionBytes;
    }

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
        (pBaseSlot != nullptr && *pBaseSlot != 0) ? *pBaseSlot : 0u;
    const uint32_t heapEnd = g_guest_heap_base + g_guest_heap_size;

    // Honour a requested address only if it is inside the arena AND no live
    // region already covers it. Reusing a live address hands the title two
    // overlapping objects, which shows up much later as a descriptor whose
    // fields have been overwritten by a different allocation.
    uint32_t addr = 0;
    bool fromRequest = false;

    if (requestedBase >= g_guest_heap_base &&
        static_cast<uint32_t>(requestedBase + size) <= heapEnd)
    {
        // A COMMIT at an address already inside a live region is the normal
        // Windows pattern and must grow that region, not fail.
        //
        // The title alternates MEM_RESERVE and MEM_COMMIT:
        //
        //   NtAllocate(&base, &sz, MEM_RESERVE, ...)  -> reserves base
        //   NtAllocate(&base, &sz, MEM_COMMIT,  ...)  -> commits INSIDE base
        //
        // Treating the overlap as a conflict and bump-allocating a fresh page
        // sent the COMMIT somewhere the CRT did not expect, so the region the
        // CRT believes it owns and the region that actually holds memory
        // diverged. That divergence is what left the heap region header at
        // 0x846A8000 uninitialised while the CRT walked it as if it were set up.
        //
        // So: if the requested range lies within one live region, grow that
        // region to cover it and report success. Only a range that straddles two
        // regions, or overlaps a region it does not contain, is a real conflict.
        for (const auto& r : g_vm_regions)
        {
            if (requestedBase >= r.base && requestedBase < r.end)
            {
                if (static_cast<uint32_t>(requestedBase + size) <= r.end)
                {
                    addr = requestedBase;
                    fromRequest = true;
                }
                break;
            }
        }

        // Not inside a live region: honour the request if nothing else covers it.
        if (addr == 0)
        {
            bool overlaps = false;
            for (const auto& r : g_vm_regions)
            {
                if (requestedBase < r.end && r.base < requestedBase + size)
                {
                    overlaps = true;
                    break;
                }
            }

            if (!overlaps)
            {
                addr = requestedBase;
                fromRequest = true;
            }
        }
    }

    if (addr == 0)
    {
        // Bump-allocate. Advance past any gap left by a region that was
        // allocated at a lower address than the break.
        uint32_t candidate = (g_vm_brk + 0xFFF) & ~0xFFFu;

        // Skip forward past live regions so we never overlap one.
        bool moved = true;
        while (moved)
        {
            moved = false;
            for (const auto& r : g_vm_regions)
            {
                if (candidate < r.end && r.base < candidate + size)
                {
                    candidate = (r.end + 0xFFF) & ~0xFFFu;
                    moved = true;
                }
            }
        }

        addr = candidate;

        if (static_cast<uint32_t>(addr + size) > heapEnd)
        {
            std::fprintf(stderr,
                "[cod3] NtAllocateVirtualMemory: out of guest VM space "
                "(want 0x%X at 0x%08X, heap ends 0x%08X)\n",
                size, addr, heapEnd);
            SET_R3(ctx, static_cast<NTSTATUS>(0xC0000017ul)); // STATUS_NO_MEMORY
            return;
        }

        if (static_cast<uint32_t>(addr + size) > g_vm_brk)
            g_vm_brk = addr + size;
    }
    else if (static_cast<uint32_t>(addr + size) > g_vm_brk)
    {
        g_vm_brk = addr + size;
    }

    g_vm_regions.push_back({ addr, addr + size });
    (void)fromRequest;

    // Commit zeroes the region, matching Windows semantics that the title
    // relies on for its page heap and TLS.
    std::memset(reinterpret_cast<void*>(static_cast<uintptr_t>(addr)), 0, size);

    // Write back through the byte-swapping helpers: the guest is big-endian and
    // would otherwise read a byteswapped pointer.
    cod3_store_u32(ctx.r3.u32, addr);
    cod3_store_u32(ctx.r4.u32, size);

    // Verify the write-back is actually visible; this is the single most
    // important invariant of the whole runtime and worth asserting explicitly.
    const uint32_t readBack = cod3_load_u32(ctx.r3.u32);
    if (readBack != addr)
    {
        std::fprintf(stderr,
            "[cod3] NtAllocateVirtualMemory: WRITE-BACK FAILED at 0x%08X "
            "(read back 0x%08X)\n",
            ctx.r3.u32, readBack);
    }

    // The VM logs its activity, which is useful while bringing the boot path up
    // Record the first page handed out. Inspecting a descriptor the guest
    // actually created is more reliable than inferring one from where the heap is
    // expected to begin: the loader's own structures occupy the low part of that
    // region, so a computed guess reads them and reports our data as the guest's.
    if (g_vm_first_page == 0)
        g_vm_first_page = addr;

    // and noisy afterwards. COD3_QUIET_VM=1 silences it.
    if (cod3_vm_quiet())
    {
        std::fprintf(stderr,
            "[cod3] NtAllocateVirtualMemory: flags=0x%X size=0x%X -> 0x%08X\n",
            flags, size, addr);
    }

    SET_R3(ctx, STATUS_SUCCESS);
}

PPC_FUNC(__imp__NtQueryVirtualMemory)
{
    // (BaseAddress, MemoryInformationClass, MemoryInformation,
    //  MemoryInformationLength, ReturnLength)
    //
    // The CRT queries the region it was given to discover where the XEX
    // descriptor lives, and uses the answer to build its heap descriptors.
    // Returning success with an unfilled structure leaves those descriptors
    // holding whatever was on the stack, which is how the boot path used to die
    // writing through a small bogus pointer.
    auto* pBase = reinterpret_cast<uint32_t*>(
        static_cast<uintptr_t>(ctx.r3.u32));
    const uint32_t infoClass = ctx.r4.u32;
    auto* pInfo = reinterpret_cast<uint32_t*>(
        static_cast<uintptr_t>(ctx.r5.u32));

    if (pInfo == nullptr)
    {
        SET_R3(ctx, static_cast<NTSTATUS>(0xC000000Dul)); // STATUS_INVALID_PARAMETER
        return;
    }

    const uint32_t queryAddr = (pBase != nullptr) ? *pBase : 0;

    // Locate the containing live region, so the answer describes real memory.
    uint32_t regionBase = queryAddr & ~0xFFFu;
    uint32_t regionSize = 0x1000;
    for (const auto& r : g_vm_regions)
    {
        if (queryAddr >= r.base && queryAddr < r.end)
        {
            regionBase = r.base;
            regionSize = r.end - r.base;
            break;
        }
    }

    if (cod3_vm_quiet())
    {
        std::fprintf(stderr,
            "[cod3] NtQueryVirtualMemory: addr=0x%08X class=%u -> base=0x%08X "
            "size=0x%X (%zu live region(s))\n",
            queryAddr, infoClass, regionBase, regionSize, g_vm_regions.size());
    }

    // MemoryInformationClass values used by the Xbox 360 CRT:
    //   0 = MemoryBasicInformation
    //   2 = MemorySectionInformation
    // We answer every class with a basic descriptor, which is enough for the
    // CRT to continue; anything more specific would need the real region map.
    //
    // Written through the byte-swapping helpers: the guest reads these
    // big-endian, and a native store would leave every field byteswapped.
    std::memset(pInfo, 0, 0x20);
    cod3_store_u32(ctx.r5.u32 + 0x00, regionBase); // BaseAddress
    cod3_store_u32(ctx.r5.u32 + 0x04, regionBase); // AllocationBase
    cod3_store_u32(ctx.r5.u32 + 0x08, regionSize); // RegionSize
    cod3_store_u32(ctx.r5.u32 + 0x0C, 0x1000);     // State: MEM_COMMIT
    cod3_store_u32(ctx.r5.u32 + 0x10, 0x04);       // Protect: PAGE_READWRITE
    cod3_store_u32(ctx.r5.u32 + 0x14, 0x02);       // Type: MEM_PRIVATE

    // ReturnLength (r7) points at a ULONG to receive the size filled in.
    cod3_store_u32(ctx.r7.u32, 0x20);

    (void)infoClass;
    SET_R3(ctx, STATUS_SUCCESS);
}

// The title frees whole reserved regions. The region is dropped from the live
// set so the address can be reused; the underlying arena pages are retained
// because they are owned by the guest VM for the life of the process.
PPC_FUNC(__imp__NtFreeVirtualMemory)
{
    auto* pBaseSlot = reinterpret_cast<uint32_t*>(
        static_cast<uintptr_t>(ctx.r3.u32));

    if (pBaseSlot != nullptr)
    {
        const uint32_t base = *pBaseSlot;
        for (auto it = g_vm_regions.begin(); it != g_vm_regions.end(); ++it)
        {
            if (it->base == base)
            {
                g_vm_regions.erase(it);
                break;
            }
        }
    }

    SET_R3(ctx, STATUS_SUCCESS);
}

// -----------------------------------------------------------------------------
// Thread local storage
// -----------------------------------------------------------------------------
//
// The CRT allocates TLS slots during boot and stores per-thread values in them.
// Slots are process-wide indices; the values are per OS thread, because each
// guest thread runs on its own Win32 thread and TLS is what keeps their state
// separate.
//
// The slot table is stored in guest memory (big-endian) because the guest reads
// slot indices out of its own TLS directory.

namespace {

constexpr uint32_t kMaxTlsSlots = 64;

// Slot value tables, one per OS thread. Thread-local storage for the runtime
// itself, keyed by Win32 thread id.
struct TlsState
{
    DWORD threadId = 0;
    uint32_t values[kMaxTlsSlots] = {};
};

static TlsState& current_tls()
{
    // A small fixed pool avoids pulling in the C++ TLS runtime; the port only
    // ever has a handful of live guest threads.
    static TlsState pool[64];
    static DWORD owner[64] = {};

    const DWORD id = GetCurrentThreadId();
    int freeSlot = -1;

    for (int i = 0; i < 64; ++i)
    {
        if (owner[i] == id)
            return pool[i];
        if (owner[i] == 0 && freeSlot < 0)
            freeSlot = i;
    }

    // Reuse a slot from a thread that has exited, if we are out of room.
    if (freeSlot < 0)
        freeSlot = 0;

    owner[freeSlot] = id;
    pool[freeSlot] = TlsState{};
    pool[freeSlot].threadId = id;
    return pool[freeSlot];
}

} // namespace

// The CRT keeps a *function pointer* in each TLS slot and calls through it
// indirectly (verified in sub_82352750: KeTlsGetValue -> mtctr -> bctrl).
//
// On hardware the XEX loader installs a real dispatcher at that pointer. For
// this title the source slot is zero, so an unset slot would be called through
// address 0. When a slot has no value, hand back the runtime's TLS dispatcher
// service instead: it reports "not handled" by returning 0, which is what a real
// dispatcher returns for a slot it does not own, so the CRT takes its normal
// fallback path rather than jumping to zero.

// Reserve a thread-local slot without going through a guest call.
//
// KeTlsAlloc below uses this too, so both paths share one counter and cannot
// hand out the same index. The runtime calls it at load time to publish the
// CRT's own slot, which the title expects the kernel to have created.
uint32_t cod3_reserve_tls_slot()
{
    static uint32_t nextSlot = 0;
    if (nextSlot >= kMaxTlsSlots)
        return 0xFFFFFFFFu; // TLS_OUT_OF_INDEXES

    // Allocate the backing store in guest memory so the guest can read slot
    // contents directly if it does so.
    (void)cod3_pool_alloc(0x1000, 'TLS', 0);

    return nextSlot++;
}

PPC_FUNC(__imp__KeTlsAlloc)
{
    // Returns a slot index, or TLS_OUT_OF_INDEXES (0xFFFFFFFF) on failure.
    const uint32_t slot = cod3_reserve_tls_slot();
    if (slot == 0xFFFFFFFFu)
    {
        SET_R3(ctx, 0xFFFFFFFFu);
        return;
    }

    // Allocate the backing store in guest memory so the guest can read slot
    // contents directly if it does so.
    (void)cod3_pool_alloc(0x1000, 'TLS\u0000', 0);

    if (cod3_vm_quiet())
    {
        std::fprintf(stderr, "[cod3] KeTlsAlloc -> slot %u\n", slot);
    }

    SET_R3(ctx, slot);
}

PPC_FUNC(__imp__KeTlsFree)
{
    // Slot indices are never recycled; freeing a slot is a no-op here.
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__KeTlsGetValue)
{
    const uint32_t slot = ctx.r3.u32;
    TlsState& tls = current_tls();

    uint32_t value = 0;
    if (slot < kMaxTlsSlots)
        value = tls.values[slot];

    // An unset slot must still yield something the guest can call, because the
    // CRT invokes the result indirectly.
    //
    // Not every caller wants that, though. sub_82352690 checks the result for
    // zero and, seeing zero, calls KeTlsSetValue to install the default. If
    // this returns the dispatcher stub instead, the guest takes the "already
    // has a value" branch, never installs its own default, and every later
    // error path operates on a stub rather than a thread error record -
    // which is what produced the fault at address 0x48.
    //
    // So the substitution is opt-in: COD3_TLS_STUB=1 restores the old
    // behaviour for diagnosing the call site that needs it.
    if (value == 0 && std::getenv("COD3_TLS_STUB") != nullptr)
        value = cod3_runtime_service_address(Cod3RuntimeService_TlsDispatcher);

    if (cod3_vm_quiet())
    {
        std::fprintf(stderr, "[cod3] KeTlsGetValue(slot=%u) -> 0x%08X\n",
                     slot, value);
    }

    SET_R3(ctx, value);
}

PPC_FUNC(__imp__KeTlsSetValue)
{
    const uint32_t slot = ctx.r3.u32;
    const uint32_t value = ctx.r4.u32;
    TlsState& tls = current_tls();

    // Refuse to store the runtime service placeholder back over itself; the
    // value would otherwise be indistinguishable from a real one.
    if (slot < kMaxTlsSlots)
        tls.values[slot] = value;

    // Returns the value that was stored, not zero.
    //
    // This was returning 0 unconditionally, and that single byte is what stops
    // the title from ever initialising its heap. sub_82352998 gates CRT setup on
    // it:
    //
    //   0x823529F8  bl    sub_82352998 -> KeTlsSetValue
    //   0x823529FC  cmpwi r3,0
    //   0x82352A00  beq   0x82352abc      bail out when r3 == 0
    //   0x82352A04  bl    sub_82352628
    //   0x82352A08  bl    sub_82357BF0    <- seeds the 36 free-list slots
    //
    // With r3 always 0 the branch is always taken and sub_82357BF0 never runs,
    // so every size class in the CRT free list stays empty. sub_82357D08 then
    // tries to pop from class 10, sees a zero head, allocates another 28-byte
    // node and calls back into sub_82357E48 - which also sees a zero head and
    // calls back again. That pair runs until the host stack is exhausted,
    // which is how boot ends. Measured at ~6,420 levels.
    //
    // Verified against the guest's own use of the result: the value is compared
    // against 0 to decide whether to continue, never for a status code, so
    // echoing back what was stored is what it expects.
    ctx.r3.u64 = value;
}


// =============================================================================
//  Threading and synchronisation
// =============================================================================
//
//  Boot now reaches these: once the CRT heap fault is tolerated (see
//  heap_fault_tolerance.cpp), the title gets as far as creating events and
//  mutexes, waiting on them, resuming threads and setting affinity. They were
//  being called ten times each and returning nothing.
//
//  Unlike the heap, these have exact Windows semantics and can be implemented
//  faithfully rather than stubbed. Real Win32 objects sit underneath, so a guest
//  that blocks on a wait really blocks, and the ordering the title depends on is
//  the ordering it gets.
//
//  HANDLE representation follows the rest of the runtime: real Win32 HANDLE
//  values narrowed to 32 bits, which is how the guest stores them.
//
//  Signatures are taken from the recompiled call sites, not assumed. Where a
//  call site does not pin a parameter down, that is stated rather than guessed.

// -----------------------------------------------------------------------------
// NtCreateEvent
// -----------------------------------------------------------------------------
//
//   r3 = &EventHandle        (out)
//   r4 = EventAttributes
//   r5 = EventType           0 = notification, 1 = manual reset
//   r6 = InitialState
//
// On the 360 r3 is the STATUS, not the handle: the handle is written through
// the out pointer. That is the same convention NtAllocateVirtualMemory uses here,
// and getting it backwards would leave the guest holding a status code as a
// handle.
PPC_FUNC(__imp__NtCreateEvent)
{
    const HANDLE h = CreateEventW(
        nullptr,                                   // no security attributes
        (ctx.r5.u32 != 0),                         // manual reset
        (ctx.r6.u32 != 0),                         // initial state
        nullptr);

    if (h == nullptr)
    {
        SET_R3(ctx, static_cast<NTSTATUS>(0xC0000001ul)); // STATUS_UNSUCCESSFUL
        return;
    }

    cod3_store_u32(ctx.r3.u32, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(h)));
    SET_R3(ctx, STATUS_SUCCESS);
}

// -----------------------------------------------------------------------------
// NtCreateMutant
// -----------------------------------------------------------------------------
//
//   r3 = &MutantHandle       (out)
//   r4 = MutantAttributes
//   r5 = InitialCount
//
// A Win32 mutex is the right primitive: it is recursive, which is what a mutant
// with an initial count above one requires, and releasing one that is not held
// fails rather than succeeding, matching NT semantics.
PPC_FUNC(__imp__NtCreateMutant)
{
    const HANDLE h = CreateMutexW(
        nullptr,
        (ctx.r5.u32 != 0),
        nullptr);

    if (h == nullptr)
    {
        SET_R3(ctx, static_cast<NTSTATUS>(0xC0000001ul));
        return;
    }

    cod3_store_u32(ctx.r3.u32, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(h)));
    SET_R3(ctx, STATUS_SUCCESS);
}

// -----------------------------------------------------------------------------
// NtWaitForSingleObjectEx
// -----------------------------------------------------------------------------
//
//   r3 = Handle
//   r4 = Alertable
//   r5 = Timeout (LARGE_INTEGER*, negative = relative, null = wait forever)
//
// Real waiting, not a stub. A timeout of zero returns immediately, which is the
// case a game uses for polling, and it must not block - getting that wrong turns
// a poll loop into a hang.
PPC_FUNC(__imp__NtWaitForSingleObjectEx)
{
    const HANDLE h = reinterpret_cast<HANDLE>(
        static_cast<uintptr_t>(ctx.r3.u32));

    if (h == nullptr || h == INVALID_HANDLE_VALUE)
    {
        SET_R3(ctx, static_cast<NTSTATUS>(0xC0000008ul)); // STATUS_INVALID_HANDLE
        return;
    }

    DWORD ms = INFINITE;
    if (ctx.r5.u32 != 0)
    {
        // A negative LARGE_INTEGER on NT is a relative timeout in 100ns units.
        // No 64-bit load helper exists. The guest wrote a big-endian
        // LARGE_INTEGER, so read the halves: high at +0, low at +4.
        const uint32_t hi = cod3_load_u32(ctx.r5.u32 + 0);
        const uint32_t lo = cod3_load_u32(ctx.r5.u32 + 4);
        const int64_t raw = (static_cast<int64_t>(hi) << 32) |
                            static_cast<int64_t>(lo);
        if (raw < 0)
        {
            const int64_t hundredNanos = -raw;
            // Round up so a sub-millisecond timeout is not truncated to "no wait",
            // which would turn a real wait into a spin.
            const int64_t m = (hundredNanos + 9999) / 10000;
            ms = (m > 0xFFFFFFFFll) ? INFINITE : static_cast<DWORD>(m);
        }
        else
        {
            ms = INFINITE; // absolute deadline: not expressible as a Win32 wait
        }
    }

    const DWORD r = WaitForSingleObject(h, ms);

    switch (r)
    {
    case WAIT_OBJECT_0:    SET_R3(ctx, STATUS_SUCCESS); break;
    case WAIT_TIMEOUT:     SET_R3(ctx, static_cast<NTSTATUS>(0x00000102ul)); break;
    case WAIT_ABANDONED:   SET_R3(ctx, static_cast<NTSTATUS>(0x000000C0ul)); break;
    case WAIT_FAILED:
    default:               SET_R3(ctx, static_cast<NTSTATUS>(0xC0000008ul)); break;
    }
}

// -----------------------------------------------------------------------------
// NtReleaseMutant
// -----------------------------------------------------------------------------
//
//   r3 = Handle
//
// Releasing a mutex this thread does not own must fail. Win32 enforces that
// itself, so the error is real rather than simulated.
PPC_FUNC(__imp__NtReleaseMutant)
{
    const HANDLE h = reinterpret_cast<HANDLE>(
        static_cast<uintptr_t>(ctx.r3.u32));

    if (h == nullptr || h == INVALID_HANDLE_VALUE)
    {
        SET_R3(ctx, static_cast<NTSTATUS>(0xC0000008ul));
        return;
    }

    SET_R3(ctx, ReleaseMutex(h) ? STATUS_SUCCESS
                                : static_cast<NTSTATUS>(0xC0000045ul));
}

// -----------------------------------------------------------------------------
// NtResumeThread
// -----------------------------------------------------------------------------
//
//   r3 = ThreadHandle
//
// Returns the previous suspend count, as NT does. Resuming a thread that is not
// suspended leaves it running and returns zero, which is correct and is what a
// caller that over-resumes expects to see.
PPC_FUNC(__imp__NtResumeThread)
{
    const HANDLE h = reinterpret_cast<HANDLE>(
        static_cast<uintptr_t>(ctx.r3.u32));

    if (h == nullptr || h == INVALID_HANDLE_VALUE)
    {
        SET_R3(ctx, static_cast<NTSTATUS>(0xC0000008ul));
        return;
    }

    const DWORD previous = ResumeThread(h);
    SET_R3(ctx, (previous == static_cast<DWORD>(-1))
                     ? static_cast<NTSTATUS>(0xC0000001ul)
                     : static_cast<NTSTATUS>(previous));
}

// -----------------------------------------------------------------------------
// KeSetAffinityThread
// -----------------------------------------------------------------------------
//
//   r3 = AffinityMask
//
// The 360 takes an affinity mask; Windows wants a processor number. The mask is
// converted by taking the lowest set bit's index, which is the single processor
// the guest asked to be pinned to in every call the title makes.
//
// Returning success without calling anything would be wrong in a way that
// matters: the guest uses this to pin threads, and silently not pinning can
// produce timing behaviour the game was not written to tolerate.
PPC_FUNC(__imp__KeSetAffinityThread)
{
    const DWORD mask = ctx.r3.u32;

    if (mask == 0)
    {
        SET_R3(ctx, STATUS_SUCCESS);
        return;
    }

    DWORD remaining = mask;
    DWORD proc = 0;
    while ((remaining & 1u) == 0)
    {
        remaining >>= 1;
        ++proc;
    }

    // Taken as a function pointer. The runtime is full of guest imports whose
    // names shadow Win32 ones, so an unqualified call here is a coin flip - and
    // KeSetAffinityThread itself is one of them.
    using SetAffinityFn = DWORD_PTR(WINAPI*)(DWORD_PTR);
    // GetProcAddress returns an untyped FARPROC; the reinterpret_cast is the
    // standard way to give it the right signature and is what the SDK expects.
    static const SetAffinityFn setAffinity =
        reinterpret_cast<SetAffinityFn>(
            ::GetProcAddress(::GetModuleHandleW(L"kernel32.dll"),
                             "SetThreadAffinityMask"));
    if (setAffinity == nullptr)
    {
        SET_R3(ctx, static_cast<NTSTATUS>(0xC0000001ul));
        return;
    }

    const DWORD_PTR previous = setAffinity(static_cast<DWORD_PTR>(1ull << proc));

    // A zero previous mask means the call failed; the guest cannot tell the
    // difference between "pinned" and "not pinned" unless it is told, so report
    // the failure rather than pretending.
    SET_R3(ctx, (previous != 0) ? STATUS_SUCCESS
                                : static_cast<NTSTATUS>(0xC0000001ul));
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
        cod3_store_u32(out, phys);
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

// -----------------------------------------------------------------------------
// XEX header fields
// -----------------------------------------------------------------------------
//
// RtlImageXexHeaderField(handle, fieldId, &value)
//
// This is how the CRT discovers the memory layout it is entitled to: stack
// size, heap size, page heap flags, TLS directory. It is the single most
// important import during boot.
//
// Verified call site (build/ppc/ppc_recomp.55.cpp, sub_82349BF0):
//
//   lwz  r3,88(r11)              ; image handle
//   lis  r4,2  ; ori r4,0x401    ; fieldId = 0x00020401
//                                  XEX_HEADER_DEFAULT_HEAP_SIZE
//   bl   RtlImageXexHeaderField
//   cmplwi r3,0 ; beq <bail>     ; failure -> heap never set up
//
// Returning 0 here made the CRT skip heap initialisation entirely, which is what
// left the heap descriptor holding a small value instead of a pointer.
//
// Several of these fields are absent from retail titles; the Xbox 360 kernel
// substitutes defaults. Those defaults are reproduced below, because failing the
// call is strictly worse than returning a plausible value.

constexpr uint32_t XEX_HEADER_IMAGE_BASE_ADDRESS = 0x00010201;
constexpr uint32_t XEX_HEADER_ENTRY_POINT         = 0x00010100;
constexpr uint32_t XEX_HEADER_TLS_INFO            = 0x00020104;
constexpr uint32_t XEX_HEADER_DEFAULT_STACK_SIZE  = 0x00020200;
constexpr uint32_t XEX_HEADER_DEFAULT_HEAP_SIZE   = 0x00020401;
constexpr uint32_t XEX_HEADER_PAGE_HEAP_SIZE_AND_FLAGS = 0x00028002;

// Verified call sites (build/ppc/ppc_recomp.55.cpp, sub_82349CC0):
//
//   lis  r4,2 ; ori r4,0x401    ; fieldId = 0x00020401 (DEFAULT_HEAP_SIZE)
//   lwz  r3,88(r11)              ; image handle
//   bl   RtlImageXexHeaderField
//   cmplwi r3,0 ; beq <bail>     ; NULL return -> give up
//   lwz  r30,0(r3)               ; read the FIRST DWORD of the returned data
//   cmplwi r30,0 ; bne <bail>    ; nonzero -> give up
//
// So the return value in r3 is a POINTER to the header data, not a status, and
// there is no out-parameter. Returning a status here made the caller bail out
// of heap setup entirely, which is what produced the bad descriptor pointer.
PPC_FUNC(__imp__RtlImageXexHeaderField)
{
    const uint32_t field = ctx.r4.u32;

    uint32_t dataPtr = 0;
    if (!cod3_xex_header_field(field, &dataPtr))
    {
        // Absent from this title. The kernel still answers with a pointer to a
        // zeroed default, and the caller requires the first DWORD to be zero.
        // Hand back a dedicated zeroed page so the caller can read it safely.
        dataPtr = cod3_xex_default_field_slot(field);
    }

    if (cod3_vm_quiet())
    {
        std::fprintf(stderr,
            "[cod3] RtlImageXexHeaderField(0x%08X) -> 0x%08X\n",
            field, dataPtr);
    }

    SET_R3(ctx, dataPtr);
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
        cod3_store_u32(outPtr, resolved);

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
    // The signature is XVideoModeInfo* - ONE out-pointer - not four separate
    // out-parameters.
    //
    // The call site is unambiguous. sub_82400098 does:
    //
    //     addi r3,r1,144
    //     bl   XGetVideoMode
    //     lwz  r11,156(r1)      ; reads +0x18 relative to the struct base
    //     subfic r11,r11,0      ; negates it and tests the carry
    //
    // so r3 points at a caller stack structure, and the caller reads a field
    // 0x18 past that base afterwards. The previous four-out-parameter version
    // wrote through r4, r5 and r6 as though they were pointers; at this call
    // site they are ordinary register values, and storing through them faulted.
    // That is where the boot-ending access violation at address 0x61 came from.
    uint32_t dw = 0, dh = 0, dr = 0, df = 0;
    cod3_host_display_mode(&dw, &dh, &dr, &df);

    const uint32_t out = ctx.r3.u32;
    if (out == 0)
    {
        // No structure supplied. There is nowhere to report a mode and no status
        // to return, so leave r3 alone rather than inventing one.
        return;
    }

    // XVideoModeInfo. Field +0x18 is the one the caller tests, and it is written
    // as "1 = a valid mode was written" so that negating it clears the carry -
    // which is what the caller's `subfic` is checking for.
    cod3_store_u32(out + 0x00, dw);          // width
    cod3_store_u32(out + 0x04, dh);          // height
    cod3_store_u32(out + 0x08, dr);          // refresh rate
    cod3_store_u32(out + 0x0C, 1);           // refresh divide
    cod3_store_u32(out + 0x10, df);          // pixel format
    cod3_store_u32(out + 0x14, 0);           // flags
    cod3_store_u32(out + 0x18, 1);           // "mode is valid"
    cod3_store_u32(out + 0x1C, 0);           // interlaced
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
        cod3_store_u32(out + 0x00, dw);
        cod3_store_u32(out + 0x04, dh);
        cod3_store_u32(out + 0x08, 0);   // non-interlaced
        cod3_store_u32(out + 0x0C, 0);
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
        cod3_store_u32(out, 0x000A0000u); // gamma 1.0
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__VdSetDisplayGamma)
{
    ctx.r3.u64 = 0;
}