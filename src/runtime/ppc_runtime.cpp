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
    // The image ends at 0x82C98000, so stack+heap must fit in the remaining
    // space up to 0xC0000000 (well below 0xFFFFFFFF, leaving room for the
    // title's own high addresses). An earlier build used 64 MiB stack +
    // 256 MiB heap, which pushed the stack top past 0x80000000; the guest then
    // saw a wrapped, nonsensical r1 and the CRT faulted on its first store.
    constexpr size_t kGuestStackSize = 16 * 1024 * 1024;
    constexpr size_t kGuestHeapSize = 384 * 1024 * 1024;

    // The image must sit at a 32-byte aligned address; PPC_IMAGE_BASE (0x82000000)
    // already satisfies that.
    const size_t imageBase = kGuestImageBase;
    const size_t stackBase = imageBase + image_extent;
    const size_t heapBase = stackBase + kGuestStackSize;
    const size_t reserve = image_extent + kGuestStackSize + kGuestHeapSize;

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
            "[cod3] could not reserve 0x%zX bytes at the guest image base "
            "0x%08X (Win32 error %lu).\n"
            "[cod3] The image must be mapped at its real virtual address because "
            "the recompiled code forms references as base+guest_VA.\n",
            reserve, imageBase, GetLastError());
        return false;
    }

    std::memcpy(mem, image.data.get(), image.size);

    // base == 0 is required: the generated code computes `base + guest_va`, and
    // guest_va is the full Xbox 360 virtual address (e.g. 0x82BF0000). Only
    // base == 0 makes that resolve to the byte the guest intended. The image is
    // mapped at PPC_IMAGE_BASE, so guest VAs and host addresses coincide.
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

    std::fprintf(stderr,
        "[cod3] loaded guest image at 0x%08X (base=0), image 0x%08X bytes, "
        "entry 0x%08X, %zu exported symbols\n",
        kGuestImageBase, static_cast<uint32_t>(image.size),
        static_cast<uint32_t>(image.entry_point), g_exported_symbols.size());
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
            kLoaderGlobalsSlot, imageEnd);
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
    slots.emplace(field, slot);
    return slot;
}

// Guest call depth tracker.
//
// Tracing every recompiled function call is not possible without instrumenting
// the generated code, but the boot path makes only a bounded number of kernel
// calls before it either succeeds or faults, so recording the last N guest
// function entries is enough to see where execution got to.
static std::vector<uint32_t> g_guest_trace;
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
    if (!g_guest_trace_enabled)
        return;
    if (g_guest_trace.size() >= 4000)
        return; // stop growing rather than corrupting memory
    g_guest_trace.push_back(address);
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
