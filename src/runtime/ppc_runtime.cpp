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

// Top of the guest stack, as a guest virtual address. See cod3_load_image().
uint32_t g_guest_stack_top = 0;

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
    // We reserve extra space *past* the image for the guest stack. The guest
    // addresses stack memory as `base + guest_va`, so the stack has to live in
    // the same allocation - pointing r1 into the image itself would have the
    // guest overwrite its own .text with return addresses.
    constexpr size_t kGuestStackSize = 16 * 1024 * 1024;

    void* mem = VirtualAlloc(nullptr, image.size + kGuestStackSize,
                             MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (mem == nullptr)
    {
        std::fprintf(stderr,
            "[cod3] failed to reserve 0x%zX bytes for the guest image + stack\n",
            image.size + kGuestStackSize);
        return false;
    }

    std::memcpy(mem, image.data.get(), image.size);
    g_base = static_cast<uint8_t*>(mem);
    g_guest_entry_point = image.entry_point;

    // PowerPC stacks grow downwards, so r1 starts at the top of the region.
    // Leave one guard page at the very top so an overflow faults cleanly instead
    // of silently walking into unrelated heap.
    g_guest_stack_top = static_cast<uint32_t>(image.size + kGuestStackSize - 0x10000);

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
        "[cod3] loaded guest image: base=0x%08X size=0x%08X entry=0x%08X "
        "(%zu exported symbols)\n",
        static_cast<uint32_t>(image.base), static_cast<uint32_t>(image.size),
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

    r.fn(ctx, g_base);
}

// -----------------------------------------------------------------------------
// Import tracing
// -----------------------------------------------------------------------------

static std::unordered_map<std::string, uint64_t> g_import_counts;
static std::vector<std::string> g_import_first_seen;
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
        g_import_first_seen.push_back(name);
    }
    else
    {
        ++it->second;
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

    std::vector<std::pair<std::string, uint64_t>> entries(
        g_import_counts.begin(), g_import_counts.end());
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