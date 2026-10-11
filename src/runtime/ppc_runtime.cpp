// =============================================================================
//  COD3 Recomp - runtime core
//
//  Owns the guest image, dispatches recompiled guest functions, and keeps the
//  record of which Xbox 360 kernel APIs the title actually reaches.
// =============================================================================

#include "cod3_runtime.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include <image.h>
#include <xex.h>

// The recompiled function table produced by XenonRecomp.
#include "ppc_recomp_shared.h"

uint8_t* g_base = nullptr;
uint32_t g_guest_entry_point = 0;

// The guest image is mapped at its real virtual address (0x82000000), so g_base
// is null and guest addresses are host addresses. See cod3_load_image().
//
// Layout of the single reservation:
//
//   0x82000000  +---------------------------+
//               |  PE image (SizeOfImage)   |
//   stack_base  +---------------------------+
//               |  guest stack (64 MiB)     |
//   heap_base   +---------------------------+
//               |  guest heap (256 MiB)     |
//               +---------------------------+
uint32_t g_guest_stack_base = 0;
uint32_t g_guest_stack_top = 0;
uint32_t g_guest_heap_base = 0;
uint32_t g_guest_heap_size = 0;

// The address the title is linked to load, and therefore the address the image
// must occupy. This comes from the XEX IMAGE_BASE_ADDRESS optional header and
// is filled in during cod3_load_image().
uint32_t kGuestImageBase = 0x82000000;

// Backing store for the pool allocator: a pointer and length into the guest
// heap region, set by cod3_load_image(). See cod3_pool_alloc().
static uint8_t* g_heap_ptr = nullptr;
static size_t g_heap_size = 0;

// The low-memory scratch region (null page). See cod3_load_image().
void* g_low_scratch = nullptr;

// The indirect-call perfect-hash table, placed immediately after the image.
// See cod3_load_image() for the layout and why it is required.
static uint32_t g_indirect_table_base = 0;
static size_t g_indirect_table_size = 0;

// Start of the recompiled code range, matching PPC_CODE_BASE in ppc_config.h.
// Indirect call targets are keyed relative to this.
static constexpr uint32_t kCodeBase = 0x820A0000u;
// End of the recompiled code range, matching PPC_CODE_SIZE.
static constexpr uint32_t kCodeSize = 0x4FB1B8u;

// The heap method table slot the CRT dereferences during TLS bootstrap.
//
// Derived from the guest's own addressing, not guessed:
//   lis r11,-32110        -> (uint32)(-32110 & 0xFFFF) << 16 = 0x82920000
//   lwz r11,-3016(r11)    -> 0x82920000 - 3016 = 0x8291F438
static constexpr uint32_t kHeapMethodSlot = 0x8291F438;

// Target for indirect calls that resolve to no known function.
//
// Gaps in the table would otherwise be called through address 0. Pointing them
// here turns an inexplicable jump to zero into a report naming the guest
// address that had no translation.
static void cod3_unresolved_indirect_report(PPCContext& ctx, uint8_t* base)
{
    // Report the first few, then go quiet: a bad indirect call in a dispatch loop
// would otherwise flood the log and hide everything else.
    static int reported = 0;

    if (reported < 12)
    {
        std::fprintf(stderr,
            "[cod3] unresolved indirect call: target=0x%08X from guest 0x%08X "
            "(r3=0x%08X r4=0x%08X r8=0x%08X)\n",
            ctx.ctr.u32, static_cast<uint32_t>(ctx.lr),
            ctx.r3.u32, ctx.r4.u32, ctx.r8.u32);
        ++reported;
    }
    else if (reported == 12)
    {
        std::fprintf(stderr,
            "[cod3] (further unresolved indirect calls suppressed)\n");
        ++reported;
    }

    (void)base;
    ctx.r3.u64 = 0;
}

PPCFunc* cod3_unresolved_indirect_target = cod3_unresolved_indirect_report;

// Resolve a guest address through the indirect-call table.
//
// This replaces XenonRecomp's PPC_LOOKUP_FUNC, which hardcodes the table at
// PPC_IMAGE_BASE + PPC_IMAGE_SIZE (0x82C30000) - an address inside the PE image,
// where the runtime cannot place a table. The slot arithmetic is identical to
// what the macro performs; only the base address differs.
//
// The table is populated during cod3_load_image(). If it is not ready yet, fall
// back to the reporting trap rather than jumping through a null pointer.
PPCFunc* cod3_lookup_indirect(uint32_t guest)
{
    const uintptr_t tableHost =
        static_cast<uintptr_t>(g_indirect_table_base);
    const size_t tableSize = g_indirect_table_size;

    if (tableHost == 0 || tableSize < sizeof(void*))
        return cod3_unresolved_indirect_target;

    // A slot lives at byte offset (guest - CODE_BASE) * 2. Guest functions are
    // 4-byte aligned, so real slots are 8-byte aligned.
    if (guest < kCodeBase)
        return cod3_unresolved_indirect_target;

    const size_t offset =
        (static_cast<size_t>(guest - kCodeBase)) * 2;

    if (offset + sizeof(void*) > tableSize)
        return cod3_unresolved_indirect_target;

    PPCFunc* fn = *reinterpret_cast<PPCFunc**>(tableHost + offset);
    return fn != nullptr ? fn : cod3_unresolved_indirect_target;
}

// Resolve an indirect call and, when entry tracing is on, report it.
//
// This is what PPC_CALL_INDIRECT_FUNC expands to. Tracing here rather than in
// cod3_lookup_indirect() is deliberate: the lookup is a pure function of the
// target address, whereas the useful information - who called, with what
// arguments - is only available at the call site, where ctx is in scope.
PPCFunc* cod3_lookup_indirect_call(uint32_t guest, const PPCContext& ctx)
{
    static int printed = 0;
    if (printed < 600 && std::getenv("COD3_TRACE_ENTRIES") != nullptr)
    {
        ++printed;
        std::fprintf(stderr,
            "[call] -> 0x%08X   lr=0x%08X  r3=0x%08X r4=0x%08X r5=0x%08X\n",
            guest, static_cast<uint32_t>(ctx.lr), ctx.r3.u32, ctx.r4.u32,
            ctx.r5.u32);
    }
    return cod3_lookup_indirect(guest);
}

// -----------------------------------------------------------------------------
// Page-heap free-list walk
// -----------------------------------------------------------------------------
//
// sub_82357E48 walks a free list in a heap descriptor and recurses into
// sub_82357D08 for every entry; sub_82357D08 allocates and calls straight back
// into sub_82357E48. The pair alternates down the guest stack until it
// overflows, which is how boot currently ends.
//
// Nothing in the recompiled code writes the list head that sub_82357E48 reads,
// so the values it keeps seeing have to come from the descriptor the CRT
// built. Reporting them at each visit is what distinguishes "the walk never
// advances" from "the list is simply very long".
//
// Wired up by the HeapFreeListWalk entry in config/recomp/COD3.toml.
void HeapFreeListWalk(PPCRegister& r30, PPCRegister& r31, PPCRegister& r11)
{
    static int depth = 0;
    static int printed = 0;

    ++depth;
    if (std::getenv("COD3_TRACE_HEAP_WALK") == nullptr || printed >= 24)
        return;

    ++printed;

    // r30 indexes a heap-flags table (its values are small - 0x40, 0x50) and
    // r31 is a large constant, 0x825C7110. The instruction the hook sits on is
    // `lwzx r11,r30,r31`, which PowerPC computes as [r30 + r31], so the list
    // head lives at that sum - a static global in .data, not something derived
    // from r30 as a pointer.
    const uint32_t headAddr = r30.u32 + r31.u32;
    const uint32_t head = cod3_load_u32(headAddr);

    std::fprintf(stderr,
        "[heap-walk] depth=%-4d r30=0x%08X r31=0x%08X head=[0x%08X]=0x%08X\n",
        depth, r30.u32, r31.u32, headAddr, head);

    if (printed == 24)
    {
        std::fprintf(stderr,
            "[heap-walk] (suppressing further reports; the walk is not "
            "terminating)\n");
    }
}

// sub_8235B570 is the gate on writing the free-list head. sub_82357D08 only
// stores the block it allocated when this returns non-zero, so a zero here
// means the list is never seeded and the walk recurses forever.
//
// It lazily installs a method pointer at 0x8291F6D0 (defaulting to itself,
// 0x8235B540) and calls through it, then treats STATUS_NO_MEMORY as failure.
//
// Wired up by the HeapListSeedGate entry in config/recomp/COD3.toml.
void HeapListSeedGate(PPCRegister& r3, PPCRegister& r4)
{
    static int printed = 0;
    if (std::getenv("COD3_TRACE_HEAP_ALLOC") == nullptr || printed >= 8)
        return;
    ++printed;

    std::fprintf(stderr,
        "[heap-seed] gate entry r3=%08X r4=%08X  [0x8291F6D0]=0x%08X\n",
        r3.u32, r4.u32, cod3_load_u32(0x8291F6D0));
}

static void (*g_heap_slot_dumper)() = nullptr;

// The exit test for the sub_82357E48 / sub_82357D08 recursion:
//
//   0x82357D90  bl   sub_82357E48      ; recurse
//   0x82357D98  lwzx r11,r29,r30      ; has anything seeded the list yet?
//   0x82357DA4  bc   <give up>         ; non-zero -> stop
//
// r11 is the value just loaded. If it stays zero the pair recurses until the
// guest stack is exhausted, because the only writer of that slot is further
// down this same function - which is never reached.
//
// Wired up by the HeapSeedExitTest entry in config/recomp/COD3.toml.
void HeapSeedExitTest(PPCRegister& r11, PPCRegister& r29, PPCRegister& r30)
{
    static int calls = 0;
    if (std::getenv("COD3_TRACE_HEAP_ALLOC") == nullptr || calls >= 12)
        return;
    ++calls;

    std::fprintf(stderr,
        "[heap-exit] #%d slot=[0x%08X] r11=0x%08X r29=0x%08X r30=0x%08X\n",
        calls, r29.u32 + r30.u32, r11.u32, r29.u32, r30.u32);
}

// sub_82357D08 re-enters itself through sub_82357E48, so counting entries at
// loc_82357D44 - the point where it decides whether to seed the list - is the
// direct measurement of recursion depth. A growing count alongside a static
// head means the seeding never happens; a count that stays at 1 means the
// loop is somewhere else entirely.
//
// Wired up by the HeapSeedNodeEntry entry in config/recomp/COD3.toml.
void HeapSeedNodeEntry(PPCRegister& r29, PPCRegister& r30, PPCRegister& r28)
{
    static int calls = 0;
    if (std::getenv("COD3_TRACE_HEAP_ALLOC") == nullptr || calls >= 12)
        return;
    ++calls;

    const uint32_t headAddr = r29.u32 + r30.u32;
    std::fprintf(stderr,
        "[heap-node] entry #%d slot=[0x%08X] head=0x%08X r28=0x%08X\n",
        calls, headAddr, cod3_load_u32(headAddr), r28.u32);
}

// The store that seeds the page-heap free list:
//
//   loc_82357DD8:  stwx r28,r29,r30   ;  [r29 + r30] = r28
//
// r30 is the constant 0x825C7110 and r29 selects the slot, so this writes the
// newly allocated block to the head the walk keeps re-reading. If it is never
// reached the head stays zero, which is exactly the infinite recursion.
//
// Wired up by the HeapListSeedStore entry in config/recomp/COD3.toml.
void HeapListSeedStore(PPCRegister& r28, PPCRegister& r29, PPCRegister& r30)
{
    static int printed = 0;
    if (std::getenv("COD3_TRACE_HEAP_ALLOC") == nullptr || printed >= 8)
        return;
    ++printed;

    std::fprintf(stderr,
        "[heap-seed] storing block 0x%08X to [0x%08X]\n",
        r28.u32, r29.u32 + r30.u32);
}

// sub_823478F0 is the page-heap allocator proper:
//
//   mflr  r12
//   bl    0x8234F280          ; restfpr
//   addi  r31,r1,-320
//   stwu  r1,-320(r1)
//   ...
//   lwz   r11,20(r30)         ; r30 = r3 = descriptor; reads descriptor+0x14
//
// A null descriptor therefore faults at offset 0x14, which is what this port
// used to die on. It is also called on the path that recurses, so a null
// return here is the proximate cause of the stack overflow.
//
// Wired up by the HeapAllocEntry entry in config/recomp/COD3.toml.
void HeapAllocEntry(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5)
{
    static int printed = 0;
    static int depth = 0;

    ++depth;
    const bool want = std::getenv("COD3_TRACE_HEAP_ALLOC") != nullptr &&
                      printed < 16;
    if (want)
    {
        ++printed;
        const uint32_t desc = r3.u32;
        std::fprintf(stderr,
            "[heap-alloc] depth=%-4d desc=0x%08X flags=0x%08X size=0x%08X"
            "  [+0x14]=0x%08X [+0x18]=0x%08X [+0x1C]=0x%08X\n",
            depth, desc, r4.u32, r5.u32,
            desc ? cod3_load_u32(desc + 0x14) : 0,
            desc ? cod3_load_u32(desc + 0x18) : 0,
            desc ? cod3_load_u32(desc + 0x1C) : 0);
    }
    if (depth == 1)
        depth = 0;
}

void cod3_set_heap_slot_dumper(void (*fn)())
{
    g_heap_slot_dumper = fn;
}

void cod3_dump_heap_slots()
{
    if (g_heap_slot_dumper != nullptr)
        g_heap_slot_dumper();
}

// -----------------------------------------------------------------------------
// Runtime service addresses
// -----------------------------------------------------------------------------
//
// Some kernel APIs must hand back a *function pointer* that the guest later
// calls indirectly. Such a pointer has to be a guest address registered in the
// indirect-call table, not a host pointer.
//
// This allocates a small block of otherwise-unused guest addresses inside the
// code range and binds them to runtime-supplied implementations. The title
// occupies ~30k of ~1.27M table slots, so a handful more costs nothing.

enum RuntimeService
{
    RuntimeService_TlsDispatcher = 0,
    // The CRT calls this through [0x825F6E38] straight after the dispatcher and
    // requires a non-zero result, so it must report success.
    RuntimeService_HeapMethod = 1,
    RuntimeService_Count,
};

static uint32_t g_runtime_service_base = 0; // guest address of slot 0
static bool g_runtime_services_ready = false;

// The TLS dispatcher the CRT calls through when a slot holds no value.
//
// This is not a no-op. Verified in sub_82352750 (ppc_recomp.58.cpp):
//
//   r3  = TLS slot id
//   bctrl -> dispatcher
//   r31 = result                      ; a heap descriptor the CRT writes into
//   if (r31 != 0) {
//       r3 = [0x825F6E38]              ; a method table address
//       bctrl -> method               ; must return non-zero
//       stw 1,   20(r31)
//       stw r11, 92(r31)
//       ...
//   }
//
// So the dispatcher must return a live, zeroed descriptor block, and the method
// it calls through [0x825F6E38] must return success. Returning 0 leaves r31
// null and the CRT immediately faults storing to it.
//
// The descriptor is a zeroed page; the CRT initialises the rest of it itself
// (it writes +0, +4, +20, +92). The method table entry is one of the runtime
// services, which reports success.
static void cod3_tls_dispatcher_stub(PPCContext& ctx, uint8_t* base)
{
    (void)base;

    // One descriptor per thread, reused across calls.
    static std::unordered_map<DWORD, uint32_t> descriptors;

    const DWORD id = GetCurrentThreadId();
    auto it = descriptors.find(id);
    if (it == descriptors.end())
    {
        auto* mem = cod3_pool_alloc(0x1000, 'HEAP', 0);
        if (mem == nullptr)
        {
            ctx.r3.u64 = 0;
            return;
        }
        const uint32_t addr = static_cast<uint32_t>(
            reinterpret_cast<uintptr_t>(mem));
        // The descriptor is guest-visible, so its fields are big-endian. The
        // CRT zero-fills the ones it cares about before reading them, and the
        // page arrives zeroed, so no explicit store is needed here.
        it = descriptors.emplace(id, addr).first;
    }

    ctx.r3.u32 = it->second;
}

// The CRT calls this immediately after the TLS dispatcher and requires a
// non-zero result before it will initialise the heap descriptor. Reporting
// success is all it asks for; the CRT then fills the descriptor in itself.
static void cod3_heap_method_stub(PPCContext& ctx, uint8_t* base)
{
    (void)base;
    ctx.r3.u32 = 1; // success
}

// Reserve `count` consecutive free table slots directly above the highest guest
// function the title uses, and bind them to the runtime services.
void cod3_register_runtime_services()
{
    if (g_runtime_services_ready || g_indirect_table_size == 0)
        return;

    const uintptr_t tableHost =
        static_cast<uintptr_t>(g_indirect_table_base);

    // A slot's byte offset is (guest - CODE_BASE) * 2. Guest function
    // addresses are 4-byte aligned, so every REAL slot lands on an 8-byte
    // boundary. Scanning at 2-byte stride would read unaligned garbage as if
    // it were a pointer and conclude the table is full, so walk by
    // sizeof(void*) instead.
    constexpr size_t kSlotStride = sizeof(void*);

    // Find the highest occupied slot; place the services just above it.
    size_t highestUsedByte = 0;
    for (size_t byte = 0; byte + sizeof(void*) <= g_indirect_table_size;
         byte += kSlotStride)
    {
        auto** slot = reinterpret_cast<PPCFunc**>(tableHost + byte);
        if (*slot != nullptr && *slot != cod3_unresolved_indirect_target)
            highestUsedByte = byte;
    }

    const size_t firstByte = highestUsedByte + kSlotStride * 8;
    if (firstByte + RuntimeService_Count * kSlotStride >=
        g_indirect_table_size)
    {
        std::fprintf(stderr,
            "[cod3] no room in the indirect call table for runtime services "
            "(highestUsedByte=0x%zx need=0x%zx have=0x%zx)\n",
            highestUsedByte,
            firstByte + RuntimeService_Count * kSlotStride,
            g_indirect_table_size);
        return;
    }

    PPCFunc* services[RuntimeService_Count] = {};
    services[RuntimeService_TlsDispatcher] = cod3_tls_dispatcher_stub;
    services[RuntimeService_HeapMethod] = cod3_heap_method_stub;

    for (size_t i = 0; i < RuntimeService_Count; ++i)
    {
        *reinterpret_cast<PPCFunc**>(
            tableHost + firstByte + i * kSlotStride) =
            services[i] != nullptr ? services[i]
                                   : cod3_unresolved_indirect_target;
    }

    g_runtime_service_base =
        kCodeBase + static_cast<uint32_t>(firstByte);
    g_runtime_services_ready = true;

    std::fprintf(stderr,
        "[cod3] runtime services at 0x%08X (table slots %zu..%zu)\n",
        g_runtime_service_base,
        (g_runtime_service_base - kCodeBase) / kSlotStride,
        (g_runtime_service_base - kCodeBase) / kSlotStride +
            RuntimeService_Count - 1);

    // Publish the heap method table entry the CRT dereferences directly.
    //
    // Verified in sub_82352750:
    //   lis r11,-32110            ; 0x828C0000
    //   lwz r3,28212(r30)         ; descriptor argument
    //   lwz r11,-3016(r11)        ; [0x828BF438] -> method pointer
    //   mtctr r11 ; bctrl
    //
    // That slot lives in the title's .data and is zero in the raw XEX. On
    // hardware the loader installs the real method there.
    cod3_store_u32(kHeapMethodSlot,
                   cod3_runtime_service_address(RuntimeService_HeapMethod));

    const uint32_t verifyMethod = cod3_load_u32(kHeapMethodSlot);
    if (verifyMethod != cod3_runtime_service_address(RuntimeService_HeapMethod))
    {
        std::fprintf(stderr,
            "[cod3] heap method slot 0x%08X write-back failed (read 0x%08X)\n",
            kHeapMethodSlot, verifyMethod);
    }
}

// Return the guest address the guest should call for a given runtime service,
// or 0 if it is unavailable.
uint32_t cod3_runtime_service_address(int service)
{
    if (!g_runtime_services_ready ||
        service < 0 || service >= RuntimeService_Count)
    {
        return 0;
    }
    return g_runtime_service_base +
           static_cast<uint32_t>(service * sizeof(void*));
}

// -----------------------------------------------------------------------------
// NULL dispatch trap region
// -----------------------------------------------------------------------------
//
// Dispatching through a null or near-null function pointer does NOT land in the
// indirect call table. The lookup is:
//
//   table + (uint32_t(y) - PPC_CODE_BASE) * 2
//
// For y == 0 that is 0x82000000 + 0xC98000 + (0 - 0x820A0000) * 2, which with
// 32-bit wraparound resolves to 0x17EB58000 - about 380 MB past the table and
// outside every mapping. The result is an immediate access violation that says
// nothing about the cause.
//
// The CRT dispatches through vtables that the loader populates, and dispatches
// through null entries whenever one is unset. Mapping that far address and
// filling it with the same trap turns those into a reported "unresolved call"
// instead of a bare crash, which is what makes the boot path diagnosable.
//
// Only a small span is needed: small y values cluster at the base of this
// region, spaced 8 bytes apart.
void cod3_map_null_dispatch_trap()
{
    if (g_indirect_table_size == 0)
        return;

    // Where y == 0 resolves to.
    const uint64_t base =
        static_cast<uint64_t>(g_indirect_table_base) +
        ((0ull - 0x820A0000ull) & 0xFFFFFFFFull) * 2ull;

    // The computed address is far outside the 32-bit guest space, but on x64 it is
    // an ordinary legal host address. VirtualAlloc is the authority on whether
    // it can actually be reserved, so there is no separate range check to get
    // wrong.
    constexpr size_t kTrapSpan = 0x100000; // 1 MiB, plenty for small targets
    void* mem = VirtualAlloc(reinterpret_cast<void*>(static_cast<uintptr_t>(base)),
                             kTrapSpan, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (mem == nullptr)
    {
        std::fprintf(stderr,
            "[cod3] could not map the NULL dispatch trap at 0x%llX "
            "(Win32 error %lu); dispatching through null will fault\n",
            static_cast<unsigned long long>(base), GetLastError());
        return;
    }

    auto** slots = reinterpret_cast<PPCFunc**>(mem);
    const size_t count = kTrapSpan / sizeof(void*);
    for (size_t i = 0; i < count; ++i)
        slots[i] = cod3_unresolved_indirect_target;

    std::fprintf(stderr,
        "[cod3] NULL dispatch trap mapped at 0x%llX, 0x%zX bytes\n",
        static_cast<unsigned long long>(base), kTrapSpan);
}

// Guest addresses of every symbol the XEX exports. Populated by
// cod3_load_image(); cod3_resolve_private_export() indexes into it by ordinal.
static std::vector<uint32_t> g_exported_symbols;

// The XEX optional header table, keyed by header field id.
//
// The CRT reads these through RtlImageXexHeaderField() during boot. If that
// call fails, the title skips heap setup and later dereferences an unset
// descriptor, so the table is captured from the container at load time.
//
// Values are resolved the way XenonUtils' getOptHeaderPtr() does:
//   (field & 0xFF) == 0 -> the value is stored inline
//   (field & 0xFF) == 1 -> the value is a pointer to a u32 in the header block
//   otherwise           -> the value is an offset into the container
static std::unordered_map<uint32_t, uint32_t> g_xex_header_fields;

// Guest-relative copies of the header-block values, so a pointer field can be
// returned to the guest as a readable guest address rather than a raw file
// offset. The header block is copied into the scratch arena at load time.
static std::unordered_map<uint32_t, uint32_t> g_xex_header_pointers;

uint32_t be32_at(const uint8_t* data, size_t offset)
{
    return (static_cast<uint32_t>(data[offset]) << 24) |
           (static_cast<uint32_t>(data[offset + 1]) << 16) |
           (static_cast<uint32_t>(data[offset + 2]) << 8) |
           static_cast<uint32_t>(data[offset + 3]);
}

// -----------------------------------------------------------------------------
// Image loading
// -----------------------------------------------------------------------------
//
// XenonRecomp translated default.xex by running it through XenonUtils'
// Xex2LoadImage(), which decrypts, decompresses and patches the import thunks.
// We deliberately use the same code path so that the bytes the runtime executes
// are provably the bytes that were translated.

static std::vector<uint8_t> g_image_storage;

bool cod3_load_image(const char* xex_path)
{
    std::FILE* fp = std::fopen(xex_path, "rb");
    if (fp == nullptr)
    {
        std::fprintf(stderr, "[cod3] cannot open XEX: %s\n", xex_path);
        return false;
    }

    std::fseek(fp, 0, SEEK_END);
    const long size = std::ftell(fp);
    std::fseek(fp, 0, SEEK_SET);
    if (size <= 0)
    {
        std::fclose(fp);
        std::fprintf(stderr, "[cod3] XEX is empty: %s\n", xex_path);
        return false;
    }

    g_image_storage.resize(static_cast<size_t>(size));
    const size_t read = std::fread(g_image_storage.data(), 1,
                                   g_image_storage.size(), fp);
    std::fclose(fp);
    if (read != g_image_storage.size())
    {
        std::fprintf(stderr, "[cod3] short read on XEX\n");
        return false;
    }

    const Image image =
        Xex2LoadImage(g_image_storage.data(), g_image_storage.size());
    if (image.data == nullptr || image.size == 0)
    {
        std::fprintf(stderr, "[cod3] Xex2LoadImage() failed to build an image\n");
        return false;
    }

    // The guest image must be page aligned: the generated prologues assert
    // ((uintptr_t)base & 0x1F) == 0.
    //
    // Sizing matters here. image.size is the XEX security block's imageSize
    // (0xC30000), but that is NOT the full extent the guest can address: the
    // PE's SizeOfImage is 0xC98000 and the last section (.reloc) ends at
    // 0xC4D208. The difference is real addressable memory the kernel maps and
    // the title touches during boot, so the allocation has to cover the PE
    // SizeOfImage, not the XEX imageSize.
    //
    // SizeOfImage is read from the in-memory PE header (the same DOS/PE/NT
    // offsets the image was mapped from), rounded up to the section alignment.
    size_t image_extent = image.size;
    {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image.data.get());
        if (dos->e_magic == IMAGE_DOS_SIGNATURE)
        {
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(
                image.data.get() + dos->e_lfanew);
            if (nt->Signature == IMAGE_NT_SIGNATURE)
            {
                const size_t size_of_image = nt->OptionalHeader.SizeOfImage;
                const size_t section_align = nt->OptionalHeader.SectionAlignment;
                if (size_of_image > image_extent)
                {
                    std::fprintf(stderr,
                        "[cod3] PE SizeOfImage 0x%zX exceeds XEX imageSize 0x%zX; "
                        "extending the mapping\n",
                        size_of_image, image.size);
                    image_extent = size_of_image;
                }
                if (section_align != 0)
                {
                    image_extent = (image_extent + section_align - 1)
                                   & ~(static_cast<size_t>(section_align) - 1);
                }
            }
        }
    }

    // Reserve the guest address space at its REAL virtual address.
    //
    // The recompiled code forms every memory reference as `base + guest_va`:
    //
    //     #define PPC_STORE_U32(x, y) *(volatile uint32_t*)(base + (x)) = ...
    //
    // and `x` is a full 32-bit Xbox 360 virtual address (0x82BF0000, ...),
    // because PowerPC registers hold real VAs rather than offsets. The image is
    // therefore mapped exactly where the title is linked to load, and `base` is
    // the (non-null, 32-byte aligned) start of that mapping.
    //
    // `base` is deliberately NOT nullptr. The generated prologues assert
    // (base & 0x1F) == 0, and passing a null `base` lets the optimiser assume
    // `base + x` is non-null, which miscompiles the pointer arithmetic.
    // GUEST ADDRESS SPACE BUDGET
    //
    // The guest address space is 32-bit: registers hold 32-bit virtual
    // addresses, and the generated code computes base+guest_va in 32-bit
    // arithmetic. The whole reservation MUST therefore stay below 4 GiB.
    //
    // Layout, in order:
    //
    //   0x82000000  PE image                (SizeOfImage, 0xC98000)
    //   0x82C98000  indirect call table     (see below)
    //   .......     guest stack
    //   .......     guest heap
    //
    // INDIRECT CALL TABLE - this is not optional.
    //
    // PowerPC has no direct calls; every call goes through a register, so
    // XenonRecomp resolves indirect calls with a perfect-hash lookup:
    //
    //   PPC_LOOKUP_FUNC(x, y) =
    //     *(PPCFunc**)(x + PPC_IMAGE_BASE + PPC_IMAGE_SIZE
    //                   + (uint32_t(y) - PPC_CODE_BASE) * 2)
    //
    // With base == 0 that table starts at 0x82000000 + 0xC30000 = 0x82C30000
    // and spans (0x8259B1B8 - 0x820A0000) * 2 = 0x9F6370 bytes, ending at
    // 0x83626370. It must be present and populated, because the title makes
    // indirect calls constantly (vtables, jump tables, TLS dispatchers) and
    // every one of them faults without it.
    //
    // The table lives immediately after the image, so it has to be reserved
    // before the stack and heap are placed.
    constexpr size_t kGuestStackSize = 16 * 1024 * 1024;
    constexpr size_t kGuestHeapSize = 256 * 1024 * 1024;

    // Size of the indirect-call perfect-hash table.
    //
    // The lookup offsets by (guest - CODE_BASE) * 2 BYTES, and guest addresses
    // span PPC_CODE_SIZE, so the table is PPC_CODE_SIZE * 2 bytes plus one slot
    // for the boundary.
    // The recompiled code addresses the table through a macro that was expanded at
    // translation time and hardcodes its location as
    // PPC_IMAGE_BASE + PPC_IMAGE_SIZE, i.e. 0x82C30000. That address falls
    // inside the PE image (.reloc), so the table cannot simply be moved there.
    // Instead PPC_CALL_INDIRECT_FUNC is overridden at compile time - see
    // src/runtime/cod3_indirect_override.h - so the generated code asks the
    // runtime where the table actually is. This placement is the one the
    // override reports.
    const size_t indirectTableBase = kGuestImageBase + image_extent;
    const uint32_t codeBase = kCodeBase;
    const uint32_t codeSize = kCodeSize;
    // Headroom above the last code slot so the runtime services have somewhere
    // to live. See cod3_register_runtime_services().
    const size_t kServiceHeadroom = 0x1000;
    static_assert(kServiceHeadroom % sizeof(void*) == 0, "aligned slots");
    const size_t indirectTableSize =
        (static_cast<size_t>(codeSize) * 2) + kServiceHeadroom +
        sizeof(void*);

    const size_t imageBase = kGuestImageBase;
    const size_t stackBase = indirectTableBase + indirectTableSize;
    const size_t heapBase = stackBase + kGuestStackSize;
    const size_t reserve = (heapBase - imageBase) + kGuestHeapSize;

    // The whole guest reservation must stay inside the 32-bit address space,
    // because the title addresses it with 32-bit registers. Getting this wrong
    // silently truncates guest pointers.
    if (imageBase + reserve > 0xFFFFFFFFull)
    {
        std::fprintf(stderr,
            "[cod3] guest memory budget overflows 32-bit address space: "
            "0x%08X + 0x%zX > 4 GiB. Reduce the stack/heap sizes.\n",
            static_cast<uint32_t>(imageBase), reserve);
        return false;
    }

    void* mem = VirtualAlloc(reinterpret_cast<void*>(static_cast<uintptr_t>(imageBase)),
                             reserve, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (mem == nullptr)
    {
        std::fprintf(stderr,
            "[cod3] could not reserve 0x%llX bytes at the guest image base "
            "0x%08X (Win32 error %lu).\n"
            "[cod3] The image must be mapped at its real virtual address because "
            "the recompiled code forms references as base+guest_VA.\n",
            static_cast<unsigned long long>(reserve),
            static_cast<uint32_t>(imageBase), GetLastError());
        return false;
    }

    std::memcpy(mem, image.data.get(), image.size);

    // Populate the indirect-call perfect-hash table.
    //
    // Every PowerPC call is indirect, so this table is on the hottest path in
    // the entire port. Each entry is keyed by the guest address:
    //
    //   table[(guest - PPC_CODE_BASE) * 2] = host function pointer
    //
    // Entries not covered by a known function must point at a trap that reports
    // the unresolved address, so a bad indirect call is diagnosable instead of
    // jumping to garbage.
    {
        const uintptr_t tableHost =
            reinterpret_cast<uintptr_t>(indirectTableBase);
        std::memset(reinterpret_cast<void*>(tableHost), 0, indirectTableSize);

        // A slot lives at byte offset (guest - CODE_BASE) * 2, exactly as
        // PPC_LOOKUP_FUNC computes it. Guest functions are 4-byte aligned, so
        // every real slot is 8-byte aligned. Walking the table at 2-byte
        // stride would both miss that and let the gap fill below overlap and
        // corrupt adjacent real slots, so use sizeof(void*) throughout.
        constexpr size_t kSlotStride = sizeof(void*);

        size_t mapped = 0;
        for (size_t i = 0; PPCFuncMappings[i].host != nullptr; ++i)
        {
            const size_t guest = PPCFuncMappings[i].guest;
            if (guest < codeBase || guest >= codeBase + codeSize)
                continue;

            if ((guest - codeBase) % 4 != 0)
                continue; // not a real instruction address

            const size_t index = (guest - codeBase) * 2;
            auto** slot = reinterpret_cast<PPCFunc**>(tableHost + index);
            *slot = PPCFuncMappings[i].host;
            ++mapped;
        }

        // Record each function's stack frame size so the crash report can
        // recover its saved link register. Nearly every translated function
        // opens with `mflr r12 / stw r12,8(r1) / stwu r1,-N(r1)`, sometimes
        // after a `bl` to a register-save helper, so scan the first few
        // instructions rather than assuming a fixed shape.
        for (size_t i = 0; PPCFuncMappings[i].host != nullptr; ++i)
        {
            const uint32_t guest = PPCFuncMappings[i].guest;
            if (guest < codeBase || guest >= codeBase + codeSize)
                continue;

            for (uint32_t k = 0; k < 6; ++k)
            {
                const uint32_t insn = cod3_load_u32(guest + k * 4);
                // stwu r1,-N(r1)
                if ((insn & 0xFFFF0000u) == 0x94210000u)
                {
                    cod3_record_frame_size(guest,
                                           static_cast<int32_t>(insn & 0xFFFF));
                    break;
                }
            }
        }

        // Any gap left as null would be called through address 0, so point the
        // gaps at a trap that reports them.
        for (size_t offset = 0; offset + sizeof(void*) <= indirectTableSize;
             offset += kSlotStride)
        {
            auto** slot = reinterpret_cast<PPCFunc**>(tableHost + offset);
            if (*slot == nullptr)
                *slot = cod3_unresolved_indirect_target;
        }

        g_indirect_table_base = indirectTableBase;
        g_indirect_table_size = indirectTableSize;

        std::fprintf(stderr,
            "[cod3] indirect call table at 0x%08X, 0x%zX bytes, %zu functions "
            "mapped, %zu gaps trapped\n",
            static_cast<uint32_t>(indirectTableBase), indirectTableSize, mapped,
            (indirectTableSize / sizeof(void*)) - mapped);

        // Bind the runtime services the kernel APIs hand back to the guest.
        cod3_register_runtime_services();

        cod3_map_null_dispatch_trap();
    }

    // base == 0 is required: the generated code forms every reference as
    // `base + guest_va`, where guest_va is the full Xbox 360 virtual address
    // (e.g. 0x82BF0000). Only base == 0 makes that resolve to the byte the
    // guest intended. The image is mapped at PPC_IMAGE_BASE, so guest VAs and
    // host addresses coincide.
    //
    // This is safe with the generated prologues: PPC_FUNC_PROLOGUE asserts
    // (base & 0x1F) == 0, which a null base satisfies.
    g_base = nullptr;
    g_guest_entry_point = image.entry_point;

    // Low-memory scratch region.
    //
    // During early CRT initialisation the title dereferences descriptors whose
    // fields it expects the kernel to have filled in. Where a field is still
    // unset, the resulting pointer is a small value (observed: 0xCA4F) and the
    // store traps.
    //
    // Mapping the bottom of the address space turns those otherwise-fatal
    // accesses into harmless writes to a scratch page, which lets the boot path
    // continue instead of stopping.
    //
    // LIMITATION: 64-bit Windows permanently reserves the first 64 KB and
    // rejects mappings there with ERROR_INVALID_PARAMETER (87), so this can
    // only start at 0x10000. Guest addresses below that can never be made
    // valid and a fault there is always fatal. The address observed faulting
    // during boot, 0xCACF, is below the limit, so this region does not cover
    // it; it exists to absorb other small bogus pointers.
    constexpr size_t kScratchSize = 64 * 1024 * 1024;

    // The reservation must land low in the address space, so the address is
    // requested explicitly; passing nullptr lets Windows choose a high address,
    // which has no effect on small guest pointers.
    //
    // It starts at 0x10000, not 0: 64-bit Windows permanently reserves the first
    // 64 KB and rejects any mapping there with ERROR_INVALID_PARAMETER (87).
    // That is a hard platform limit, so guest addresses below 0x10000 can never
    // be made valid. See docs/STATUS.md.
    g_low_scratch = VirtualAlloc(reinterpret_cast<void*>(0x10000),
                                 kScratchSize,
                                 MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (g_low_scratch == nullptr)
    {
        std::fprintf(stderr,
            "[cod3] warning: could not map the 0x%zX byte low-memory scratch "
            "region (Win32 error %lu); low-address faults remain fatal.\n",
            kScratchSize, GetLastError());
    }

    // The stack sits above the image so r1 cannot point into .text, and the
    // heap above that. Both are addressed as guest VAs.
    g_guest_stack_base = static_cast<uint32_t>(stackBase);
    g_guest_stack_top = static_cast<uint32_t>(stackBase + kGuestStackSize - 0x10000);
    g_guest_heap_base = static_cast<uint32_t>(heapBase);
    g_guest_heap_size = static_cast<uint32_t>(kGuestHeapSize);

    g_heap_ptr = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(heapBase));
    g_heap_size = kGuestHeapSize;

    // Verify the layout is monotonic. A truncated or miscomputed stack top is
    // invisible until the CRT faults on its first store, so it is checked here
    // where the cause is obvious.
    if (!(g_guest_stack_base < g_guest_stack_top) ||
        !(g_guest_stack_top < g_guest_heap_base) ||
        static_cast<uint32_t>(heapBase + kGuestHeapSize) <= g_guest_heap_base)
    {
        std::fprintf(stderr,
            "[cod3] guest memory layout is broken:\n"
            "       image 0x%08X..0x%08X\n"
            "       stack 0x%08X..0x%08X\n"
            "       heap  0x%08X..0x%08X\n",
            static_cast<uint32_t>(imageBase), static_cast<uint32_t>(stackBase),
            g_guest_stack_base, g_guest_stack_top,
            g_guest_heap_base, static_cast<uint32_t>(heapBase + kGuestHeapSize));
        return false;
    }

    // Record exported symbols so XexGetProcedureAddress can resolve them.
    // Image::symbols is a multiset<Symbol>, ordered by address.
    g_exported_symbols.clear();
    for (const auto& symbol : image.symbols)
    {
        if (symbol.address != 0)
            g_exported_symbols.push_back(static_cast<uint32_t>(symbol.address));
    }
    std::sort(g_exported_symbols.begin(), g_exported_symbols.end());

    // Capture the XEX optional header table from the container.
    //
    // The whole container header region is copied into guest memory so that
    // pointer-valued header fields (TLS info, heap sizes, ...) can be handed
    // back to the guest as real readable guest addresses.
    g_xex_header_fields.clear();
    g_xex_header_pointers.clear();

    const uint8_t* xex = g_image_storage.data();
    const size_t xexSize = g_image_storage.size();

    if (xexSize >= 0x18)
    {
        const uint32_t headerCount = be32_at(xex, 0x14);

        // Copy the header block into the guest arena so pointer fields resolve.
        uint32_t headerGuestBase = 0;
        if (g_heap_ptr != nullptr)
        {
            headerGuestBase = g_guest_heap_base;
            std::memcpy(reinterpret_cast<void*>(
                            static_cast<uintptr_t>(headerGuestBase)),
                        xex, xexSize < 0x40000 ? xexSize : 0x40000);
        }

        for (uint32_t i = 0; i < headerCount; ++i)
        {
            const size_t entry = 0x18 + static_cast<size_t>(i) * 8;
            if (entry + 8 > xexSize)
                break;

            const uint32_t field = be32_at(xex, entry);
            const uint32_t raw = be32_at(xex, entry + 4);
            const uint32_t kind = field & 0xFFu;

            uint32_t value;
            if (kind == 0x00)
            {
                // Value stored inline in the header entry.
                value = raw;
            }
            else if (kind == 0x01)
            {
                // Pointer to a big-endian u32 stored right after the entry.
                const size_t valueOff = entry + 4;
                if (valueOff + 4 > xexSize)
                    continue;
                value = (headerGuestBase != 0)
                            ? (headerGuestBase + static_cast<uint32_t>(valueOff))
                            : be32_at(xex, valueOff);
            }
            else
            {
                // Offset into the container.
                value = (headerGuestBase != 0)
                            ? (headerGuestBase + raw)
                            : raw;
            }

            g_xex_header_fields[field] = value;
        }
    }

    std::fprintf(stderr,
        "[cod3] captured %zu XEX optional headers for the title to query\n",
        g_xex_header_fields.size());

    // Publish the loader-populated globals the CRT walks during boot.
    cod3_populate_loader_globals();

    // Dump the heap method-table slots the CRT consults while allocating. They
    // decide whether the allocator's retry loop terminates, so being able to
    // read them at fault time is worth more than re-deriving the addresses.
    if (std::getenv("COD3_DUMP_HEAP_SLOTS") != nullptr)
    {
        struct { uint32_t va; const char* what; } slots[] = {
            { 0x8291F438, "heap method slot (published by the runtime)" },
            { 0x8291F6C8, "method pointer read by sub_8235A688" },
            { 0x8291F6CC, "gate checked before that call" },
            { 0x82BEA9C0, "page heap descriptor" },
            { 0x82BEAAE4, "descriptor written by sub_8234BE28 (-21788)" },
            { 0x82BEAAE8, "descriptor written by sub_8234BE28 (-21784)" },
        };
        std::fprintf(stderr, "[cod3] heap/descriptor slots:\n");
        for (const auto& s : slots)
        {
            std::fprintf(stderr, "    0x%08X = 0x%08X   %s\n", s.va,
                         cod3_load_u32(s.va), s.what);
        }

        // Re-read these on demand. At load time the page heap has not been
        // built yet, so the interesting values only exist once the CRT has run.
        cod3_set_heap_slot_dumper(
            []()
            {
                std::fprintf(stderr, "[cod3] heap/descriptor slots (live):\n");
                struct { uint32_t va; const char* what; } live[] = {
                    { 0x8291F6C8, "method pointer read by sub_8235A688" },
                    { 0x8291F6CC, "gate checked before that call" },
                    { 0x82BEA9C0, "page heap descriptor" },
                    { 0x82BEAAE4, "descriptor written by sub_8234BE28 (-21788)" },
                    { 0x82BEAAE8, "descriptor written by sub_8234BE28 (-21784)" },
                };
                for (const auto& s : live)
                {
                    std::fprintf(stderr, "    0x%08X = 0x%08X   %s\n", s.va,
                                 cod3_load_u32(s.va), s.what);
                }
            });
    }

    std::fprintf(stderr,
        "[cod3] loaded guest image at 0x%08X (base=0), image 0x%08X bytes, "
        "entry 0x%08X, %zu exported symbols\n",
        kGuestImageBase, static_cast<uint32_t>(image.size),
        static_cast<uint32_t>(image.entry_point), g_exported_symbols.size());

    // Dump what we actually loaded. The standalone extractor in tools/ produces
    // a shorter image than the XEX declares, so anything decoded against it can
    // disagree with the bytes the recompiler compiled. This path is the same
    // one XenonRecomp used, so it is the ground truth for offline analysis.
    if (const char* dump = std::getenv("COD3_DUMP_IMAGE"); dump != nullptr)
    {
        if (FILE* f = std::fopen(dump, "wb"))
        {
            const size_t written =
                std::fwrite(image.data.get(), 1, image.size, f);
            std::fclose(f);
            std::fprintf(stderr,
                "[cod3] dumped %zu of %zu image bytes to %s\n",
                written, static_cast<size_t>(image.size), dump);
        }
        else
        {
            std::fprintf(stderr, "[cod3] could not write %s\n", dump);
        }
    }

    return true;
}

// -----------------------------------------------------------------------------
// Guest function dispatch
// -----------------------------------------------------------------------------
//
// Recompiled functions are exposed in a flat, address-ordered table. The guest
// addresses are absolute virtual addresses in the Xbox 360's own address space,
// so a binary search over the table resolves one to a host function pointer.

namespace {

struct GuestCallResult
{
    bool found;
    PPCFunc* fn;
};

GuestCallResult find_guest_function(uint32_t address)
{
    // PPCFuncMappings is generated in ascending address order and terminated by
    // a {0, nullptr} sentinel.
    size_t lo = 0;
    size_t hi = 0;
    while (PPCFuncMappings[hi].host != nullptr)
        ++hi;

    while (lo < hi)
    {
        const size_t mid = lo + (hi - lo) / 2;
        const size_t g = PPCFuncMappings[mid].guest;
        if (g == address)
            return { true, PPCFuncMappings[mid].host };
        if (g < address)
            lo = mid + 1;
        else
            hi = mid;
    }
    return { false, nullptr };
}

} // namespace

uint32_t cod3_resolve_private_export(uint32_t ordinal)
{
    // The XEX keeps private exports in a flat ordinal table. Walk the symbol
    // map XenonUtils built from the XEX headers and match on ordinal order.
    // Ordinals below the private range are kernel/XAM exports and never map
    // to a recompiled function.
    if (ordinal < 0x10000u)
        return 0;

    static std::unordered_map<uint32_t, uint32_t>* table = nullptr;
    if (table == nullptr)
    {
        table = new std::unordered_map<uint32_t, uint32_t>();

        // Build the table from the XEX export data that XenonUtils parsed.
        // IMAGE_SYM_FUNCTION entries carry the address; exports are ordered by
        // ordinal in the image's symbol table.
        std::vector<uint32_t> addrs;
        for (const auto& kv : g_exported_symbols)
            addrs.push_back(kv);
        std::sort(addrs.begin(), addrs.end());

        uint32_t nextOrdinal = 0x10000u;
        for (uint32_t addr : addrs)
        {
            while (find_guest_function(addr).found)
                (*table)[nextOrdinal++] = addr;
        }
    }

    auto it = table->find(ordinal);
    return (it == table->end()) ? 0u : it->second;
}

void cod3_run_guest_function(uint32_t address, PPCContext& ctx)
{
    const GuestCallResult r = find_guest_function(address);
    if (!r.found)
    {
        std::fprintf(stderr,
            "[cod3] no recompiled function for guest address 0x%08X\n", address);
        return;
    }

    // base is null because the guest image is mapped at its real virtual
    // address; the callee forms every reference as base + guest_va.
    cod3_record_guest_call(address);
    r.fn(ctx, g_base);
}

// Record the arguments a guest function was entered with, and the address it
// was called from.
//
// The host stack cannot be mapped back to a guest call chain reliably: the
// generated code tail-calls freely, so several frames collapse onto the same
// symbol. The link register, however, is set by the caller immediately before
// the call and still holds the return address when the callee faults, which is
// exactly the information a stack walk cannot recover.
void cod3_trace_guest_entry(uint32_t address, const PPCContext& ctx)
{
    static int printed = 0;
    if (std::getenv("COD3_TRACE_ENTRIES") == nullptr || printed >= 400)
        return;
    ++printed;
    std::fprintf(stderr,
        "[entry] 0x%08X  lr=0x%08X  r3=0x%08X r4=0x%08X r5=0x%08X r6=0x%08X\n",
        address, static_cast<uint32_t>(ctx.lr), ctx.r3.u32, ctx.r4.u32,
        ctx.r5.u32, ctx.r6.u32);
}

// Resolve an indirect call, tracing it when COD3_TRACE_ENTRIES is set.
// Defined next to cod3_lookup_indirect, which it delegates to.
PPCFunc* cod3_lookup_indirect_call(uint32_t guest, const PPCContext& ctx);

// Where every translated function spills its incoming link register.
//
// Every recompiled guest function begins with `mflr r12` followed by
// `stw r12,-8(r1)`, and most open their frame with `stwu r1,-N(r1)`, so the
// saved link register is the word at r1-8 in that frame. Recording the frame
// size per function lets cod3_frame_link_register() find r1 again.
static std::unordered_map<uint32_t, int32_t> g_frame_sizes;

void cod3_record_frame_size(uint32_t guest, int32_t size)
{
    g_frame_sizes[guest] = size;
}

void cod3_frame_link_register(const void* returnAddress, uint32_t* outLr)
{
    *outLr = 0;
    uint32_t guest = 0;
    uint64_t distance = 0;
    if (!cod3_attribute_host_pc(returnAddress, &guest, &distance))
        return;

    const auto it = g_frame_sizes.find(guest);
    if (it == g_frame_sizes.end())
        return;

    // The host return address sits inside this frame; the guest stack pointer
    // is a little below it, and the saved link register is 8 bytes below that.
    // Scanning back is bounded by the frame size so a mismatch cannot run away.
    const int32_t frame = it->second;
    if (frame <= 0 || frame > 4096)
        return;

    const auto* base = static_cast<const uint8_t*>(returnAddress);
    for (int32_t off = 8; off <= frame + 64; off += 8)
    {
        uint32_t candidate = 0;
        std::memcpy(&candidate, base - off, sizeof(candidate));
        if (candidate >= kGuestImageBase &&
            candidate < kGuestImageBase + 0x1000000u)
        {
            *outLr = candidate;
            return;
        }
    }
}

// -----------------------------------------------------------------------------
// Loader-populated globals
// -----------------------------------------------------------------------------
//
// Before the entry point runs, the Xbox 360 kernel populates a block of globals
// in the title's .rdata describing the loaded XEX. The CRT walks this chain
// during boot (verified in sub_82349CC0):
//
//   [0x82001288] -> A
//   [A + 0x00]   -> B
//   [B + 0x58]   -> image handle
//   RtlImageXexHeaderField(handle, 0x20401) -> pointer;  [pointer] must be 0
//
// On hardware every step is populated by the loader, so all of it reads as zero
// in the raw XEX and the CRT bails out immediately on `cmplwi r11,0`. Building
// the chain here is what lets the boot path continue.
//
// The offsets are recovered from the guest's own code, not guessed.

constexpr uint32_t kLoaderGlobalsSlot = 0x82001288;

void cod3_populate_loader_globals()
{
    // g_base is intentionally null (the image is mapped at its real address),
    // so readiness is judged on the guest layout, not on g_base.
    if (g_guest_heap_base == 0 || kGuestImageBase == 0)
        return;

    auto* heap = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(g_guest_heap_base));

    // Two 4 KiB pages for the descriptor chain, taken from the front of the
    // heap arena. The VM bump allocator starts above this region.
    auto* structA = heap;              // pointed at by [0x82001288]
    auto* structB = heap + 0x1000;     // pointed at by [A + 0]

    std::memset(structA, 0, 0x1000);
    std::memset(structB, 0, 0x1000);

    // Guest memory is big-endian, so every value written here goes through the
    // byte-swapping helpers. Writing natively yields 0x0000CA83 where the guest
    // expects 0x83CA0000, and the CRT then dereferences nonsense.

    // [A + 0] -> B
    cod3_store_u32(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(structA)),
                   static_cast<uint32_t>(reinterpret_cast<uintptr_t>(structB)));

    // [B + 0x58] holds the "image handle" the CRT passes to
    // RtlImageXexHeaderField. The runtime does not use the handle for
    // validation, but it must be a plausible non-zero value.
    cod3_store_u32(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(structB) + 0x58),
                   kGuestImageBase);

    // The kernel also publishes the entry point and image size here; the CRT
    // and the debug layer both read them.
    cod3_store_u32(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(structB) + 0x10),
                   g_guest_entry_point);
    cod3_store_u32(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(structB) + 0x14),
                   kGuestImageBase);

    // Publish the chain. g_base is null, so the guest address is the host address.
    const uintptr_t slotHost = kLoaderGlobalsSlot;
    const uintptr_t imageEnd = kGuestImageBase + 0xC98000;
    if (slotHost + 4 > imageEnd)
    {
        std::fprintf(stderr,
            "[cod3] loader globals slot 0x%08X is outside the mapped image "
            "(ends 0x%08X); the CRT will bail out during boot.\n",
            static_cast<uint32_t>(kLoaderGlobalsSlot),
            static_cast<uint32_t>(imageEnd));
        return;
    }

    cod3_store_u32(kLoaderGlobalsSlot,
                   static_cast<uint32_t>(reinterpret_cast<uintptr_t>(structA)));

    // Read it back through the same helpers to prove the round trip is correct.
    // A mismatch here means every other guest write in the runtime is wrong too.
    const uint32_t verify = cod3_load_u32(kLoaderGlobalsSlot);
    const uint32_t expectA = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(structA));
    if (verify != expectA)
    {
        std::fprintf(stderr,
            "[cod3] FATAL: loader globals round trip failed: wrote 0x%08X, "
            "read 0x%08X. Guest memory byte order handling is wrong.\n",
            expectA, verify);
    }

    std::fprintf(stderr,
        "[cod3] loader globals published: [0x%08X] -> 0x%08X -> 0x%08X, "
        "image handle 0x%08X\n",
        kLoaderGlobalsSlot,
        static_cast<uint32_t>(reinterpret_cast<uintptr_t>(structA)),
        static_cast<uint32_t>(reinterpret_cast<uintptr_t>(structB)),
        kGuestImageBase);
}

// Look up an XEX optional header field captured from the container.
//
// `field` is one of the XEX_HEADER_* ids (Xex2HeaderKeys in XenonUtils/xex.h).
// The returned value is a POINTER to the field's data, matching what
// RtlImageXexHeaderField hands the guest.
bool cod3_xex_header_field(uint32_t field, uint32_t* outValue)
{
    auto it = g_xex_header_fields.find(field);
    if (it == g_xex_header_fields.end())
        return false;
    if (outValue != nullptr)
        *outValue = it->second;
    return true;
}

// A small pool of zeroed guest pages used to answer header fields that this
// title does not carry.
//
// The Xbox 360 kernel returns a pointer to a default value rather than failing,
// and the CRT dereferences the result unconditionally. Returning a real,
// readable zeroed page lets that code run correctly for the fields retail COD3
// omits (notably XEX_HEADER_DEFAULT_HEAP_SIZE).
uint32_t cod3_xex_default_field_slot(uint32_t field)
{
    static std::unordered_map<uint32_t, uint32_t> slots;
    static uint32_t nextFree = 0;

    auto it = slots.find(field);
    if (it != slots.end())
        return it->second;

    // Carve a page from the tail of the guest heap arena.
    const uint32_t pageSize = 0x1000;
    const uint32_t heapEnd = g_guest_heap_base + g_guest_heap_size;

    if (nextFree == 0)
        nextFree = g_guest_heap_base + 0x1000000; // past the live VM region

    // Keep well clear of both ends of the arena.
    if (static_cast<uint32_t>(nextFree + pageSize) >= heapEnd ||
        nextFree < g_guest_heap_base + 0x800000)
    {
        nextFree = g_guest_heap_base + 0x800000;
    }

    const uint32_t slot = nextFree;
    nextFree += pageSize;

    std::memset(reinterpret_cast<void*>(static_cast<uintptr_t>(slot)), 0, pageSize);

    // Not every absent field defaults to zero - the CRT reads some of them as
    // "is this feature enabled" flags, and answering zero makes it skip setup
    // it then depends on.
    //
    // XEX_HEADER_XAPI_HEAP_SIZE (0x00020401) is the case that matters. In
    // sub_82349CC0 the CRT queries it and:
    //
    //     lwz  r30,0(r3)            ; the returned value
    //     cmplwi r30,0
    //     bne  <skip page heap>
    //     li   r3,1 ; b <return 1>  ; zero means "no page heap required"
    //
    // Returning 1 tells the caller the page heap is unnecessary, so it never
    // runs sub_82347340 and the heap descriptor global at 0x82BEA9C0 stays
    // null. The very next allocation then reads that null descriptor and the
    // title faults at sub_823478F0 dereferencing offset 0x14.
    //
    // A non-zero answer takes the other branch and the page heap really is
    // created, which is what the rest of the CRT expects. 16 MiB is the XDK's
    // default page-heap reservation.
    constexpr uint32_t XEX_HEADER_XAPI_HEAP_SIZE = 0x00020401u;
    if (field == XEX_HEADER_XAPI_HEAP_SIZE)
    {
        const uint32_t value = 16u * 1024u * 1024u;
        cod3_store_u32(slot, value);
    }

    slots.emplace(field, slot);
    return slot;
}

static std::vector<uint32_t> g_guest_trace;
// Depth at which entry tracing starts, so the useful tail is what gets printed.
static int g_guest_trace_depth = 0;
static bool g_guest_trace_enabled = false;

void cod3_set_guest_trace(bool enabled)
{
    g_guest_trace_enabled = enabled;
    if (enabled)
        g_guest_trace.reserve(4096);
    else
        g_guest_trace.clear();
}

void cod3_record_guest_call(uint32_t address)
{
    if (g_guest_trace_enabled)
    {
        if (g_guest_trace.size() >= 4000)
            return; // stop growing rather than corrupting memory
        g_guest_trace.push_back(address);
    }
}

// -----------------------------------------------------------------------------
// Import tracing
// -----------------------------------------------------------------------------
//
// The runtime's single most useful diagnostic. Once the port gets far enough
// to run, every kernel API it reaches that is still a stub is recorded here,
// so the trace is a prioritised work list: whatever the title actually calls is
// what has to be implemented next.

static std::unordered_map<std::string, uint64_t> g_import_counts;
static std::vector<std::pair<std::string, uint64_t>> g_import_order;
static CRITICAL_SECTION g_trace_lock;
static bool g_trace_lock_ready = false;

bool cod3_import_trace_enabled()
{
    static const bool enabled = []() {
        const char* v = std::getenv("COD3_QUIET_IMPORTS");
        return !(v != nullptr && v[0] == '1');
    }();
    return enabled;
}

void cod3_trace_unimplemented(const char* name)
{
    if (!g_trace_lock_ready)
    {
        InitializeCriticalSection(&g_trace_lock);
        g_trace_lock_ready = true;
    }

    EnterCriticalSection(&g_trace_lock);
    auto it = g_import_counts.find(name);
    if (it == g_import_counts.end())
    {
        g_import_counts.emplace(name, 1);
        g_import_order.emplace_back(name, 1);
    }
    else
    {
        ++it->second;
        for (auto& entry : g_import_order)
        {
            if (entry.first == name)
            {
                ++entry.second;
                break;
            }
        }
    }
    LeaveCriticalSection(&g_trace_lock);

    if (cod3_import_trace_enabled())
    {
        std::fprintf(stderr, "[cod3] kernel import not implemented: %s\n", name);
    }
}

void cod3_dump_import_trace()
{
    if (g_import_counts.empty())
    {
        std::fprintf(stderr, "[cod3] the title never called a kernel API\n");
        return;
    }

    auto entries = g_import_order;
    std::sort(entries.begin(), entries.end(),
              [](const auto& a, const auto& b) {
                  if (a.second != b.second) return a.second > b.second;
                  return a.first < b.first;
              });

    std::fprintf(stderr, "\n[cod3] kernel APIs reached (%zu distinct):\n",
                 entries.size());
    for (const auto& [name, count] : entries)
    {
        std::fprintf(stderr, "[cod3]   %-46s %llu call(s)\n",
                     name.c_str(), static_cast<unsigned long long>(count));
    }
}

// -----------------------------------------------------------------------------
// Host display mode
// -----------------------------------------------------------------------------

void cod3_host_display_mode(uint32_t* width, uint32_t* height,
                            uint32_t* refreshHz, uint32_t* pixelFormat)
{
    // Default to 1280x720, which is the highest mode the retail Xbox 360
    // pipeline supports natively; the host window will be resized if the NGL
    // layer chooses a different mode later.
    *width = 1280;
    *height = 720;
    *refreshHz = 60;

    if (pixelFormat != nullptr)
    {
        // Xbox 360 video format: packed RGB 8:8:8 with a 2-bit alpha field,
        // which is what the NGL layer expects to find in the mode descriptor.
        *pixelFormat = 3;
    }
}
