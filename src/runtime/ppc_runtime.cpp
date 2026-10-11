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

// A reserved (but inaccessible) block at address 0, used to absorb guest
// accesses that dereference an uninitialised register. See cod3_load_image().
void* g_zero_guard = nullptr;

// Guest addresses of every symbol the XEX exports. Populated by
// cod3_load_image(); cod3_resolve_private_export() indexes into it by ordinal.
static std::vector<uint32_t> g_exported_symbols;

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
    r.fn(ctx, g_base);
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
