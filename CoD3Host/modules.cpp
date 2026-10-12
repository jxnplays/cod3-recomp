#include "modules.h"
#include "kernel.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <vector>

#include <file.h>
#include <image.h>
#include <xex.h>

#include <Windows.h>

// The function tables of the level libraries the build linked in.
#define COD3_LEVEL(name) extern PPCFuncMapping PPCFuncMappings_##name[];
#include "cod3_levels.inc"
#undef COD3_LEVEL

namespace
{
    // A level's recompiled code, as the build registered it.
    struct Level
    {
        const char* name;
        const PPCFuncMapping* mappings;
    };

    // One entry per level library the build linked in.
    const Level kLevels[] = {
#define COD3_LEVEL(name) { #name, PPCFuncMappings_##name },
#include "cod3_levels.inc"
#undef COD3_LEVEL
        { nullptr, nullptr }
    };

    struct Module
    {
        std::string name;
        uint32_t handle = 0;
        uint32_t base = 0;
        uint32_t size = 0;
        uint32_t entry = 0;
        std::vector<PPCFunc*> table;              // by (address - base) / 4
        std::vector<std::pair<uint32_t, uint32_t>> exports;   // ordinal, address
    };

    std::mutex g_mutex;
    Module* g_loaded = nullptr;   // at most one, all levels share a base
    uint32_t g_nextHandle = 0x00080001;

    // The level's name from the DLL's path: sp\saint_lo\saint_lo.dll.
    std::string LevelName(const std::string& guestPath)
    {
        std::string name = guestPath;
        const size_t slash = name.find_last_of("\\/");
        if (slash != std::string::npos) name = name.substr(slash + 1);
        const size_t dot = name.find('.');
        if (dot != std::string::npos) name = name.substr(0, dot);
        for (char& c : name) c = char(tolower(uint8_t(c)));
        return name;
    }

    const Level* FindLevel(const std::string& name)
    {
        for (const Level* level = kLevels; level->name != nullptr; level++)
            if (name == level->name) return level;
        return nullptr;
    }

    // The XEX2 export table: three magic words, the module number, the
    // version, the image base (high half), the count, the first ordinal,
    // and one offset from the base per ordinal.
    void ReadExports(Module& module, const uint8_t* file)
    {
        const auto* header = reinterpret_cast<const Xex2Header*>(file);
        const auto* security = reinterpret_cast<const Xex2SecurityInfo*>(file + header->securityOffset);
        const uint32_t table = security->exportTable;
        if (table == 0 || !Modules::Contains(table)) return;

        const uint32_t magic0 = Guest::Read32(Guest::Base, table);
        const uint32_t magic1 = Guest::Read32(Guest::Base, table + 4);
        if (magic0 != 0x48000000u || magic1 != 0x00485645u)
        {
            printf("modules: export table at 0x%08X has magic %08X %08X, not read\n",
                table, magic0, magic1);
            return;
        }
        const uint32_t imageBase = Guest::Read32(Guest::Base, table + 32) << 16;
        const uint32_t count = Guest::Read32(Guest::Base, table + 36);
        const uint32_t first = Guest::Read32(Guest::Base, table + 40);
        for (uint32_t i = 0; i < count && i < 4096; i++)
        {
            const uint32_t offset = Guest::Read32(Guest::Base, table + 44 + i * 4);
            if (offset == 0) continue;
            module.exports.push_back({ first + i, imageBase + offset });
        }
    }
}

bool Modules::Contains(uint32_t address)
{
    const Module* module = g_loaded;
    return module != nullptr && address >= module->base && address < module->base + module->size;
}

PPCFunc* Modules::Lookup(uint32_t address)
{
    const Module* module = g_loaded;
    if (module == nullptr || address < module->base || address >= module->base + module->size)
        return nullptr;
    return module->table[(address - module->base) / 4];
}

const char* Modules::LoadedName()
{
    const Module* module = g_loaded;
    return module != nullptr ? module->name.c_str() : "";
}

uint32_t Modules::Load(PPCContext& ctx, uint8_t* base, const std::string& guestPath)
{
    const std::string name = LevelName(guestPath);
    const Level* level = FindLevel(name);
    if (level == nullptr)
    {
        printf("modules: %s is not one of the recompiled levels; the level cannot start\n",
            guestPath.c_str());
        return 0;
    }

    const std::filesystem::path hostPath = Kernel::ResolveGuestPath(guestPath);
    if (hostPath.empty())
    {
        printf("modules: %s is not in the installed game\n", guestPath.c_str());
        return 0;
    }
    const auto file = LoadFile(hostPath.string().c_str());
    if (file.empty())
    {
        printf("modules: cannot read %s\n", hostPath.string().c_str());
        return 0;
    }

    // The image, decrypted and expanded the way the recompiler saw it, laid
    // into guest memory at its own base. ParseImage has already replaced the
    // code of every import thunk with nops and a return and named the thunk.
    Image image = Image::ParseImage(file.data(), file.size());
    if (image.data == nullptr || image.size == 0)
    {
        printf("modules: %s did not parse as a XEX\n", hostPath.string().c_str());
        return 0;
    }

    std::unique_lock<std::mutex> lock(g_mutex);
    if (g_loaded != nullptr)
    {
        printf("modules: %s is still loaded while %s is asked for; replacing it\n",
            g_loaded->name.c_str(), name.c_str());
        delete g_loaded;
        g_loaded = nullptr;
    }

    auto* module = new Module();
    module->name = name;
    module->handle = g_nextHandle;
    g_nextHandle += 4;
    module->base = uint32_t(image.base);
    module->size = image.size;
    module->entry = uint32_t(image.entry_point);

    // How much the loader actually expanded. With the basic compression
    // these DLLs use, the buffer is the sum of the blocks, which is not
    // the image size the security header states: the last run of zeros
    // is left out. Copying the stated size read past the buffer.
    size_t expanded = image.size;
    const auto* format = reinterpret_cast<const Xex2OptFileFormatInfo*>(
        getOptHeaderPtr(file.data(), XEX_HEADER_FILE_FORMAT_INFO));
    if (format != nullptr && format->compressionType == XEX_COMPRESSION_BASIC)
    {
        const auto* blocks = reinterpret_cast<const Xex2FileBasicCompressionBlock*>(format + 1);
        const size_t count = (format->infoSize / sizeof(Xex2FileBasicCompressionInfo)) - 1;
        expanded = 0;
        for (size_t i = 0; i < count; i++) expanded += blocks[i].dataSize + blocks[i].zeroSize;
    }
    const size_t copied = std::min<size_t>(expanded, image.size);
    memcpy(Guest::Base + module->base, image.data.get(), copied);
    if (copied < image.size) memset(Guest::Base + module->base + copied, 0, image.size - copied);

    // The indirect call table for the module: its recompiled functions, and
    // the host stub behind every import thunk, by the thunk's name.
    module->table.assign(size_t(module->size / 4), nullptr);
    size_t functions = 0;
    for (const PPCFuncMapping* mapping = level->mappings; mapping->guest != 0; mapping++)
    {
        const uint32_t address = uint32_t(mapping->guest);
        if (address < module->base || address >= module->base + module->size) continue;
        module->table[(address - module->base) / 4] = mapping->host;
        functions++;
    }
    size_t imports = 0, unbound = 0;
    for (const auto& symbol : image.symbols)
    {
        if (symbol.name.empty()) continue;
        std::string importName = symbol.name;
        if (importName.rfind("__imp__", 0) == 0) importName = importName.substr(7);
        PPCFunc* host = Kernel::FindImport(importName.c_str());
        const uint32_t address = uint32_t(symbol.address);
        if (host == nullptr || address < module->base || address >= module->base + module->size)
        {
            printf("modules: no host stub for the import %s\n", importName.c_str());
            unbound++;
            continue;
        }
        module->table[(address - module->base) / 4] = host;
        imports++;
    }

    g_loaded = module;
    ReadExports(*module, file.data());

    printf("modules: %s loaded at 0x%08X, %u bytes, entry 0x%08X, %zu functions, "
           "%zu imports bound%s, %zu exports\n",
        name.c_str(), module->base, module->size, module->entry, functions, imports,
        unbound != 0 ? " (some not)" : "", module->exports.size());
    for (const auto& entry : module->exports)
        printf("modules:   export %u at 0x%08X\n", entry.first, entry.second);
    fflush(stdout);

    // The entry point is the DLL's main, called with the module handle, the
    // process attach reason and no reserved argument, the way the loader
    // calls it. Its own static initialisers run from there, and may ask
    // about the module, so the lock is not held across it.
    lock.unlock();
    if (PPCFunc* entry = Lookup(module->entry))
    {
        ctx.r3.u32 = module->handle;
        ctx.r4.u32 = 1;   // DLL_PROCESS_ATTACH
        ctx.r5.u32 = 0;
        entry(ctx, base);
        const uint32_t result = ctx.r3.u32;
        printf("modules: %s entry point returned 0x%08X\n", name.c_str(), result);
        fflush(stdout);
    }
    else
    {
        printf("modules: %s has no recompiled function at its entry point 0x%08X\n",
            name.c_str(), module->entry);
    }
    return module->handle;
}

void Modules::Unload(PPCContext& ctx, uint8_t* base, uint32_t handle)
{
    std::unique_lock<std::mutex> lock(g_mutex);
    if (g_loaded == nullptr || g_loaded->handle != handle) return;

    // The DLL's main with DLL_PROCESS_DETACH, before it goes, as the loader
    // calls it: its static objects are destroyed there, and they took their
    // places in the title's own lists and tables when the attach built them.
    const uint32_t entry = g_loaded->entry;
    const std::string name = g_loaded->name;
    lock.unlock();
    if (PPCFunc* routine = Lookup(entry))
    {
        ctx.r3.u32 = handle;
        ctx.r4.u32 = 0;   // DLL_PROCESS_DETACH
        ctx.r5.u32 = 0;
        routine(ctx, base);
        printf("modules: %s entry point returned 0x%08X for the detach\n", name.c_str(), ctx.r3.u32);
    }
    lock.lock();
    if (g_loaded == nullptr || g_loaded->handle != handle) return;
    printf("modules: %s unloaded\n", g_loaded->name.c_str());
    fflush(stdout);
    delete g_loaded;
    g_loaded = nullptr;
}

uint32_t Modules::Export(uint32_t handle, uint32_t ordinal)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_loaded == nullptr || g_loaded->handle != handle) return 0;
    for (const auto& entry : g_loaded->exports)
        if (entry.first == ordinal) return entry.second;
    return 0;
}
