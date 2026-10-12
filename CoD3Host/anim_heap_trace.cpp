// Traces of the animation system, all behind COD3_TRACEANIMHEAP=1: what was
// needed to find why soldiers stood in their bind pose after a restart (the
// title's memset leaving its last bytes, see native_crt.cpp), kept for the
// next time something in it goes wrong.
//
// - AnimHeap, four megabytes made once at start (sub_82400098), holds the
//   decoded animation cache at 0x82914EE0. Its allocate, sub_82522CB8,
//   refuses past four megabytes and the cache then drops its least recently
//   used entry and tries again: a full heap is its normal state after about
//   a hundred seconds of a level, not a fault. Live blocks are kept with
//   their caller; COD3_ANIMHEAPLIMIT=KB makes the heap smaller.
// - The animation trees (entity +552) and the info pool at 0x82A59DB0 their
//   slots point into: which trees play nothing, the last calls each tree was
//   given, and the whole pool checked for a record held twice or held and
//   free at once, every three seconds, around the restart's letting go of
//   every animation (sub_824F7F90), and after every call into a tree from a
//   level start until the first damage, which is printed with its call.
// - Name resolution of tree entries to animation classes (sub_824FAF40), the
//   resource directory's animation files, the zone manager.

#include "anim_heap_trace.h"
#include "kernel.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <Windows.h>

namespace
{
    // A read of a guest address that may be anything (a field that holds a
    // pointer in some objects and text in others): through ReadProcessMemory,
    // so an address with nothing behind it answers false instead of faulting
    // into the guest's handlers, which would commit a page there.
    bool SafeRead(uint8_t* base, uint32_t address, void* out, size_t size)
    {
        SIZE_T got = 0;
        return address >= 0x10000 && ReadProcessMemory(GetCurrentProcess(), base + address, out, size, &got) && got == size;
    }

    uint32_t SafeRead32(uint8_t* base, uint32_t address)
    {
        uint8_t bytes[4];
        if (!SafeRead(base, address, bytes, 4)) return 0;
        return (uint32_t(bytes[0]) << 24) | (uint32_t(bytes[1]) << 16) | (uint32_t(bytes[2]) << 8) | bytes[3];
    }

    void NotePoolThread(uint32_t caller);
    void AfterTreeCall(const char* what, uint32_t tree, uint32_t index, uint32_t caller);
}

namespace
{
    constexpr uint32_t UsedOffset = 1168;     // the sub-heap's byte count, the heap +8 +1160
    constexpr uint32_t Limit = 0x400000;

    bool Wanted()
    {
        static const bool wanted = getenv("COD3_TRACEANIMHEAP") != nullptr;
        return wanted;
    }

    struct Block
    {
        uint32_t size;
        uint32_t caller;
        uint32_t generation;
    };

    std::mutex s_lock;
    std::unordered_map<uint32_t, Block> s_live;
    uint32_t s_generation = 0;
    uint32_t s_heap = 0;
    uint64_t s_allocations = 0, s_frees = 0, s_refusals = 0;
    uint32_t s_highWater = 0;
}

// AnimHeap::Allocate(size)
extern "C" PPC_FUNC(__imp__sub_82522CB8);
PPC_FUNC(sub_82522CB8)
{
    const uint32_t heap = ctx.r3.u32;
    const uint32_t size = ctx.r4.u32;
    const uint32_t caller = uint32_t(ctx.lr);

    // COD3_ANIMHEAPLIMIT=KB: a smaller heap, to see what a full one does
    // without playing a level long enough to fill four megabytes.
    static const uint32_t limit = [] {
        const char* text = getenv("COD3_ANIMHEAPLIMIT");
        return text != nullptr ? uint32_t(strtoul(text, nullptr, 10)) << 10 : 0u;
    }();
    if (limit != 0 && Guest::Read32(base, heap + UsedOffset) + size > limit)
        ctx.r3.u64 = 0;
    else
        __imp__sub_82522CB8(ctx, base);
    const uint32_t block = ctx.r3.u32;
    const uint32_t used = Guest::Read32(base, heap + UsedOffset);

    if (block == 0)
    {
        if (!Wanted()) return;
        static int told = 0;
        std::lock_guard<std::mutex> hold(s_lock);
        s_refusals++;
        if (told++ < 50)
        {
            printf("animheap: REFUSED %u bytes from %08X, %u of %u bytes in use, %zu blocks live\n",
                size, caller, used, Limit, s_live.size());
            fflush(stdout);
        }
        return;
    }
    if (!Wanted()) return;

    std::lock_guard<std::mutex> hold(s_lock);
    s_heap = heap;
    s_allocations++;
    s_live[block] = Block{ size, caller, s_generation };
    if (used >= s_highWater + 0x40000)
    {
        s_highWater = used & ~0x3FFFFu;
        printf("animheap: %u KB in use (%zu blocks), level start %u\n", used >> 10, s_live.size(), s_generation);
        fflush(stdout);
    }
}

// AnimHeap::Free(block)
extern "C" PPC_FUNC(__imp__sub_82522CB0);
PPC_FUNC(sub_82522CB0)
{
    const uint32_t block = ctx.r4.u32;
    if (Wanted() && block != 0)
    {
        std::lock_guard<std::mutex> hold(s_lock);
        s_frees++;
        if (s_live.erase(block) == 0)
        {
            static int told = 0;
            if (told++ < 20) { printf("animheap: free of %08X, not a live block, from %08X\n", block, uint32_t(ctx.lr)); fflush(stdout); }
        }
    }
    __imp__sub_82522CB0(ctx, base);
}

namespace
{
    std::atomic<uint32_t> s_fileLoads{ 0 }, s_fileFlushes{ 0 };
    std::atomic<uint32_t> s_resolved{ 0 }, s_unresolved{ 0 }, s_stale{ 0 };
}

// An animation file's load, sub_82173808(directory, request): the request
// holds the name at +4; the file's buffer becomes the nalAnimFile.
extern "C" PPC_FUNC(__imp__sub_82173808);
PPC_FUNC(sub_82173808)
{
    const uint32_t request = ctx.r4.u32;
    __imp__sub_82173808(ctx, base);
    const uint32_t loads = s_fileLoads.fetch_add(1);
    if (Wanted() && loads < 20)
    {
        printf("animcache: file \"%s\" loaded at %08X\n", reinterpret_cast<const char*>(base + request + 4), ctx.r3.u32);
        fflush(stdout);
    }
}

// An animation file let go, sub_82173AD8(directory, file, mode): mode 0
// takes one from the count of users at +68 and goes on only when it reaches
// zero; any other mode forces it. Going on takes the file out of the
// directory and frees its buffer.
extern "C" PPC_FUNC(__imp__sub_82173AD8);
PPC_FUNC(sub_82173AD8)
{
    const uint32_t file = ctx.r4.u32;
    const uint32_t mode = ctx.r5.u32;
    const uint32_t users = file != 0 ? Guest::Read32(base, file + 68) : 0;
    const uint32_t flags = file != 0 ? Guest::Read32(base, file + 4) : 0;
    const uint32_t caller = uint32_t(ctx.lr);
    __imp__sub_82173AD8(ctx, base);
    static std::atomic<int> told{ 0 };
    if (Wanted() && file != 0 && (mode != 0 || int32_t(users) <= 1) && told.fetch_add(1) < 400)
    {
        printf("animcache: file %08X let go (mode %u, %d users before, flags %08X -> %08X) from %08X\n",
            file, mode, int32_t(users), flags, Guest::Read32(base, file + 4), caller);
        fflush(stdout);
    }
}

// An animation file's decoded data given back to the cache, sub_82170DA8(file).
extern "C" PPC_FUNC(__imp__sub_82170DA8);
PPC_FUNC(sub_82170DA8)
{
    s_fileFlushes++;
    __imp__sub_82170DA8(ctx, base);
}

namespace
{
    // The decoded animation cache at 0x82914EE0: the heap, then the least
    // and the most recently used entry. An entry is the owner's slot that
    // points at it, the next and previous entries, its size, and its chunks.
    // An entry whose slot no longer points back belongs to an owner that is
    // gone; freeing it later writes a zero into whatever took that memory.
    constexpr uint32_t CacheAddress = 0x82914EE0;

    void CheckCache(uint8_t* base, const char* why)
    {
        uint32_t entries = 0, stale = 0, ownerless = 0, bytes = 0;
        uint32_t firstStale = 0, firstSlot = 0;
        for (uint32_t entry = Guest::Read32(base, CacheAddress + 4); entry != 0 && entries < 100000;
             entry = Guest::Read32(base, entry + 4))
        {
            entries++;
            bytes += Guest::Read32(base, entry + 12);
            const uint32_t slot = Guest::Read32(base, entry);
            if (slot == 0) { ownerless++; continue; }
            if (Guest::Read32(base, slot) != entry)
            {
                if (stale++ == 0) { firstStale = entry; firstSlot = slot; }
            }
        }
        printf("animcache: %s: %u entries (%u KB of headers), %u owned by nothing, %u whose owner no longer points back",
            why, entries, bytes >> 10, ownerless, stale);
        if (stale != 0) printf(" (first %08X, slot %08X holds %08X)", firstStale, firstSlot, Guest::Read32(base, firstSlot));
        printf("\n");
        fflush(stdout);
    }
}

namespace AnimHeapTrace
{
    void LevelStart(uint8_t* base)
    {
        if (!Wanted()) return;
        void StartTreeScan(uint8_t * base);
        StartTreeScan(base);
        CheckCache(base, "level start");
        void WatchPool(uint8_t * base);
        WatchPool(base);
        void CheckPoolNow(uint8_t * base, const char* why);
        CheckPoolNow(base, "level start");
        printf("animcache: %u animation files loaded and %u flushed so far\n", s_fileLoads.load(), s_fileFlushes.load());
        printf("animtree: %u entries resolved, %u unresolved, %u stale so far; generation %u\n", s_resolved.load(),
            s_unresolved.load(), s_stale.load(), Guest::Read32(base, 0x825D19E8));
        std::lock_guard<std::mutex> hold(s_lock);
        const uint32_t used = s_heap != 0 ? Guest::Read32(base, s_heap + UsedOffset) : 0;
        printf("animheap: level start %u: %u KB in use, %zu blocks live, %llu allocated, %llu freed, %llu refused\n",
            s_generation, used >> 10, s_live.size(), (unsigned long long)s_allocations,
            (unsigned long long)s_frees, (unsigned long long)s_refusals);

        // What is still held, by the level start that took it and its caller.
        std::map<std::pair<uint32_t, uint32_t>, std::pair<uint32_t, uint64_t>> held;
        for (const auto& [block, info] : s_live)
        {
            auto& entry = held[{ info.generation, info.caller }];
            entry.first++;
            entry.second += info.size;
        }
        int lines = 0;
        for (const auto& [key, count] : held)
        {
            if (lines++ >= 40) break;
            printf("animheap:   from level start %u, caller %08X: %u blocks, %llu bytes\n",
                key.first, key.second, count.first, (unsigned long long)count.second);
        }
        fflush(stdout);
        s_generation++;
    }
}

// The animation tree's "set this animation", sub_824FE120(tree, index,
// weight, rate, time, ...). The tree's +8 is the list of animations it was
// made from, their count at +4; an index past the count is reported by the
// title ("playing animation that doesn't exist in nextgen animtree") and
// played as animation 0. Here the tree, its list and the caller, too.
namespace
{
    // The last few calls into the animation tree each tree was given, for the
    // scan below: only the outermost, since each of these calls the others.
    struct TreeCall
    {
        char what;          // s set, k set knob, c clear, a the fourth
        uint32_t index;
        float weight;
        uint32_t caller;
        DWORD when;
    };
    struct History
    {
        uint32_t calls;
        TreeCall last[8];
    };
    std::mutex s_setLock;
    std::unordered_map<uint32_t, History> s_history;
    thread_local int t_treeDepth = 0;

    struct TreeCallScope
    {
        TreeCallScope(uint32_t tree, char what, uint32_t index, float weight, uint32_t caller)
            : tree(tree), what(what), index(index), caller(caller)
        {
            if (t_treeDepth++ != 0 || !Wanted()) return;
            std::lock_guard<std::mutex> hold(s_setLock);
            History& history = s_history[tree];
            history.last[history.calls % 8] = TreeCall{ what, index, weight, caller, GetTickCount() };
            history.calls++;
        }
        ~TreeCallScope()
        {
            if (--t_treeDepth != 0) return;
            AfterTreeCall(what == 's' ? "setting an animation" : what == 'k' ? "setting a knob" : what == 'c' ? "clearing an animation"
                : "sub_82500A40", tree, index, caller);
        }
        uint32_t tree;
        char what;
        uint32_t index;
        uint32_t caller;
    };

    std::string DescribeHistory(uint32_t tree)
    {
        History history{};
        {
            std::lock_guard<std::mutex> hold(s_setLock);
            auto found = s_history.find(tree);
            if (found != s_history.end()) history = found->second;
        }
        char text[96];
        snprintf(text, sizeof text, "%u calls;", history.calls);
        std::string result = text;
        const uint32_t shown = history.calls < 8 ? history.calls : 8;
        for (uint32_t i = history.calls - shown; i < history.calls; i++)
        {
            const TreeCall& call = history.last[i % 8];
            snprintf(text, sizeof text, " %c%u w%.2f @%08X -%.1fs", call.what, call.index, call.weight, call.caller,
                (GetTickCount() - call.when) / 1000.0);
            result += text;
        }
        return result;
    }
}

extern "C" PPC_FUNC(__imp__sub_824FE410);
PPC_FUNC(sub_824FE410)
{
    TreeCallScope scope(ctx.r3.u32, 'k', ctx.r4.u32, float(ctx.f1.f64), uint32_t(ctx.lr));
    __imp__sub_824FE410(ctx, base);
}

extern "C" PPC_FUNC(__imp__sub_82500B80);
PPC_FUNC(sub_82500B80)
{
    TreeCallScope scope(ctx.r3.u32, 'c', ctx.r4.u32, float(ctx.f1.f64), uint32_t(ctx.lr));
    __imp__sub_82500B80(ctx, base);
}

extern "C" PPC_FUNC(__imp__sub_82500A40);
PPC_FUNC(sub_82500A40)
{
    TreeCallScope scope(ctx.r3.u32, 'a', ctx.r4.u32, float(ctx.f1.f64), uint32_t(ctx.lr));
    __imp__sub_82500A40(ctx, base);
}

extern "C" PPC_FUNC(__imp__sub_824FE120);
PPC_FUNC(sub_824FE120)
{
    const uint32_t tree = ctx.r3.u32;
    const uint32_t index = ctx.r4.u32;
    const uint32_t list = Guest::Read32(base, tree + 8);
    const uint32_t count = list != 0 ? Guest::Read32(base, list + 4) : 0;
    if (index > count)
    {
        static std::atomic<int> told{ 0 };
        if (told.fetch_add(1) < 60)
        {
            const uint32_t name = Guest::Read32(base, list);
            const bool named = name >= 0x10000 && name < 0xC0000000u && base[name] >= 32 && base[name] < 127;
            printf("animtree: index %u played on tree %08X whose list %08X has %u (list +0 %08X%s%s%s) from %08X, stack %08X %08X\n",
                index, tree, list, count, name, named ? " \"" : "", named ? reinterpret_cast<const char*>(base + name) : "",
                named ? "\"" : "", uint32_t(ctx.lr), Guest::Read32(base, Guest::Read32(base, ctx.r1.u32) - 8),
                Guest::Read32(base, Guest::Read32(base, Guest::Read32(base, ctx.r1.u32)) - 8));
            fflush(stdout);
        }
    }
    TreeCallScope scope(tree, 's', index, float(ctx.f1.f64), uint32_t(ctx.lr));
    __imp__sub_824FE120(ctx, base);
}

// The stream zone manager made, sub_82457FE0(memory): once a session, or
// once a level? Its address goes to *(0x82A2AB74). COD3_TRACEANIMHEAP.
extern "C" PPC_FUNC(__imp__sub_82457FE0);
PPC_FUNC(sub_82457FE0)
{
    const uint32_t object = ctx.r3.u32;
    const uint32_t caller = uint32_t(ctx.lr);
    __imp__sub_82457FE0(ctx, base);
    if (Wanted()) { printf("zones: manager made at %08X from %08X\n", object, caller); fflush(stdout); }
}

// A zone's unload steps, sub_82462648(zone): state +200 goes 4 -> 5 -> 7,
// then the script hears "unloaded_<zone>".
extern "C" PPC_FUNC(__imp__sub_82462648);
PPC_FUNC(sub_82462648)
{
    const uint32_t zone = ctx.r3.u32;
    const uint32_t before = Guest::Read32(base, zone + 200);
    __imp__sub_82462648(ctx, base);
    const uint32_t after = Guest::Read32(base, zone + 200);
    static std::atomic<int> told{ 0 };
    if (Wanted() && before != after && told.fetch_add(1) < 200)
    {
        printf("zones: zone %08X unload step, state %u -> %u\n", zone, before, after);
        fflush(stdout);
    }
}

// An animation tree entry finds its animation class, sub_824FAF40(entry):
// the entry's name at +0 is looked up in the resource manager as type 5
// and the class kept at +8; a failed lookup is not tried again until the
// generation at 0x825D19E8 moves on (the entry keeps the one it tried at
// +16). An animation class starts with the table at 0x82027D84, which the
// lookup checks; a kept class that no longer does is a stale pointer.
namespace
{
    constexpr uint32_t GenerationAddress = 0x825D19E8;
    constexpr uint32_t AnimClassTable = 0x82027D84;

    // An entry's name is a hash of the animation's name (sub_820CEBB8).
    std::string EntryName(uint8_t*, uint32_t name)
    {
        char hex[16];
        snprintf(hex, sizeof hex, "#%08X", name);
        return hex;
    }
}

extern "C" PPC_FUNC(__imp__sub_824FAF40);
PPC_FUNC(sub_824FAF40)
{
    const uint32_t entry = ctx.r3.u32;
    const uint32_t kept = Guest::Read32(base, entry + 8);
    const uint32_t tried = Guest::Read32(base, entry + 16);
    const uint32_t generation = Guest::Read32(base, GenerationAddress);
    if (Wanted() && kept != 0 && SafeRead32(base, kept) != AnimClassTable)
    {
        if (s_stale.fetch_add(1) < 40)
        {
            printf("animtree: entry %08X \"%s\" keeps class %08X whose table is %08X, not an animation class\n",
                entry, EntryName(base, Guest::Read32(base, entry)).c_str(), kept, SafeRead32(base, kept));
            fflush(stdout);
        }
    }
    __imp__sub_824FAF40(ctx, base);
    if (!Wanted() || kept != 0 || tried == generation) return;
    const uint32_t found = Guest::Read32(base, entry + 8);
    if (found != 0) { s_resolved++; return; }
    if (s_unresolved.fetch_add(1) < 60)
    {
        printf("animtree: entry %08X \"%s\" finds no animation class (generation %u)\n",
            entry, EntryName(base, Guest::Read32(base, entry)).c_str(), generation);
        fflush(stdout);
    }
}

extern "C" PPC_FUNC(__imp__sub_824F1428);
PPC_FUNC(sub_824F1428)
{
    if (Wanted()) { printf("animtree: generation moves on (%u) from %08X\n", Guest::Read32(base, GenerationAddress) + 1, uint32_t(ctx.lr)); fflush(stdout); }
    __imp__sub_824F1428(ctx, base);
}

extern "C" PPC_FUNC(__imp__sub_8245A750);
PPC_FUNC(sub_8245A750)
{
    static std::atomic<int> told{ 0 };
    if (Wanted() && told.fetch_add(1) < 20) { printf("animtree: generation moves on (%u) with a directory change from %08X\n", Guest::Read32(base, GenerationAddress) + 1, uint32_t(ctx.lr)); fflush(stdout); }
    __imp__sub_8245A750(ctx, base);
}

// Every three seconds, the entities the scripts can name (the table at
// 0x82A2DCD0, 4096 pairs of entity and number) that have an animation tree
// at +552: how many play nothing at all. A tree keeps, for each animation of
// its list, a slot at (index + 12) * 2 that is 0 while the animation is
// idle; a tree whose every slot is 0 leaves its model in the bind pose.
namespace
{
    void ScanTrees(uint8_t* base)
    {
        uint32_t trees = 0, idle = 0;
        std::map<uint32_t, std::pair<uint32_t, uint32_t>> byList;   // list -> trees, idle
        std::string idleOnes, busyOnes;
        std::map<uint32_t, uint32_t> skeletons;   // the tree's +16 -> trees
        for (uint32_t i = 0; i < 4096; i++)
        {
            const uint32_t entity = Guest::Read32(base, 0x82A2DCD0 + i * 8);
            if (entity < 0x10000 || entity >= 0xC0000000u) continue;
            const uint32_t tree = Guest::Read32(base, entity + 552);
            if (tree < 0x10000 || tree >= 0xC0000000u) continue;
            const uint32_t list = Guest::Read32(base, tree + 8);
            const uint32_t count = list >= 0x10000 && list < 0xC0000000u ? Guest::Read32(base, list + 4) : 0;
            if (count == 0 || count > 4000) continue;
            uint32_t playing = 0;
            std::string active;
            for (uint32_t a = 0; a < count; a++)
            {
                const uint16_t slot = Guest::Read16(base, tree + (a + 12) * 2);
                if (slot == 0) continue;
                playing++;
                if (active.size() < 120)
                {
                    char text[24];
                    snprintf(text, sizeof text, " %u(slot %u)", a, slot);
                    active += text;
                }
            }
            // A tree that played something at the last scan and plays nothing now.
            static std::unordered_map<uint32_t, std::string> s_wasActive;
            std::string& was = s_wasActive[tree];
            if (playing == 0 && !was.empty())
            {
                printf("animtree: tree %08X of %08X (type %u, +12 %08X +16 %u) went idle; it was playing%s; %s\n",
                    tree, entity, Guest::Read8(base, entity), Guest::Read32(base, tree + 12), Guest::Read32(base, tree + 16),
                    was.c_str(), DescribeHistory(tree).c_str());
            }
            was = active;
            trees++;
            skeletons[Guest::Read32(base, tree + 16)]++;
            auto& group = byList[list];
            group.first++;
            if (playing == 0)
            {
                idle++;
                group.second++;
                if (idleOnes.size() < 1200)
                {
                    char text[96];
                    snprintf(text, sizeof text, "\n    %08X type %u tree %08X +16 %u: ", entity, Guest::Read8(base, entity), tree,
                        Guest::Read32(base, tree + 16));
                    idleOnes += text;
                    idleOnes += DescribeHistory(tree);
                }
            }
            else if (Guest::Read8(base, entity) == 7 && busyOnes.size() < 600)
            {
                char text[96];
                snprintf(text, sizeof text, "\n    %08X type 7 tree %08X +16 %u playing %u: ", entity, tree,
                    Guest::Read32(base, tree + 16), playing);
                busyOnes += text;
                busyOnes += DescribeHistory(tree);
            }
        }
        printf("animtree: scan: %u trees, %u playing nothing;", trees, idle);
        for (const auto& [list, counts] : byList) printf(" list %08X %u/%u", list, counts.second, counts.first);
        printf("; +16:");
        for (const auto& [skeleton, count] : skeletons)
            printf(" %08X x%u (table %08X)", skeleton, count,
                skeleton >= 0x10000 && skeleton < 0xC0000000u ? Guest::Read32(base, skeleton) : 0);
        printf("\n");
        if (idle != 0) printf("animtree: idle:%s\n", idleOnes.c_str());
        static int busyShown = 0;
        if (!busyOnes.empty() && busyShown++ % 5 == 0) printf("animtree: some busy ones:%s\n", busyOnes.c_str());
        fflush(stdout);
    }
}

// The asset banks, once: the resource manager at *(0x82A2AB68) keeps a bank
// per id at +(13 + id) * 4, and the set of banks at *(0x82A2AB6C) a zone per
// id at +(11 + id) * 4 and three ids of its own at +28, +32 and +40 (the last
// is what a tree gets at +16 when its entity names no bank). Each is printed
// with the first words of its object and any text in them, to put names to
// the ids the trees carry (7, 8, 10, 12 in Chambois).
namespace
{
    std::string TextAt(uint8_t* base, uint32_t address)
    {
        uint8_t bytes[48];
        if (address < 0x10000 || address >= 0xC0000000u || !SafeRead(base, address, bytes, sizeof bytes)) return "";
        std::string text;
        for (int i = 0; i < 48; i++)
        {
            const uint8_t c = bytes[i];
            if (c == 0) break;
            if (c < 32 || c >= 127) return "";
            text += char(c);
        }
        return text.size() >= 3 ? text : "";
    }

    void ListBanks(uint8_t* base)
    {
        const uint32_t manager = Guest::Read32(base, 0x82A2AB68);
        const uint32_t set = Guest::Read32(base, 0x82A2AB6C);
        printf("banks: manager %08X, set %08X; set +28 %d +32 %d +40 %d\n", manager, set,
            int32_t(Guest::Read32(base, set + 28)), int32_t(Guest::Read32(base, set + 32)), int32_t(Guest::Read32(base, set + 40)));
        for (uint32_t id = 0; id < 64; id++)
        {
            const uint32_t bank = manager ? Guest::Read32(base, manager + (13 + id) * 4) : 0;
            const uint32_t zone = set ? Guest::Read32(base, set + (11 + id) * 4) : 0;
            if (bank == 0 && zone == 0) continue;
            printf("banks: id %2u bank %08X zone %08X", id, bank, zone);
            if (zone >= 0x10000 && zone < 0xC0000000u)
            {
                printf(" (state +76 %d, +136 %d, +200 %d)", int32_t(Guest::Read32(base, zone + 76)),
                    int32_t(Guest::Read32(base, zone + 136)), int32_t(Guest::Read32(base, zone + 200)));
                for (uint32_t off = 0; off < 256; off += 4)
                {
                    const std::string text = TextAt(base, Guest::Read32(base, zone + off));
                    if (!text.empty()) printf(" +%u:\"%s\"", off, text.c_str());
                    const std::string inline_ = TextAt(base, zone + off);
                    if (!inline_.empty() && off % 16 == 0) printf(" @+%u:\"%s\"", off, inline_.c_str());
                }
            }
            if (bank >= 0x10000 && bank < 0xC0000000u)
            {
                printf(" | bank types %u", Guest::Read32(base, bank + 8));
                for (uint32_t off = 0; off < 64; off += 4)
                {
                    const std::string text = TextAt(base, Guest::Read32(base, bank + off));
                    if (!text.empty()) printf(" +%u:\"%s\"", off, text.c_str());
                }
            }
            printf("\n");
        }
        fflush(stdout);
    }
}

namespace AnimHeapTrace
{
    void StartTreeScan(uint8_t* base)
    {
        static std::atomic<bool> started{ false };
        if (!Wanted() || started.exchange(true)) return;
        std::thread([base]() {
            for (int scan = 0;; scan++)
            {
                Sleep(3000);
                if (scan == 3) ListBanks(base);
                ScanTrees(base);
                void CheckPoolNow(uint8_t * base, const char* why);
                CheckPoolNow(base, "scan");
            }
        }).detach();
    }
}

// The animation info pool at 0x82A59DB0: 52-byte records, record 0 the
// sentinel whose +12 is the head of the free list (+12 next, +10 previous).
// sub_824FCF68(tree, index) takes the head for the tree's slot, and
// sub_824F2348(tree, record) puts a record back with no check that it was
// out: a record put back twice makes a list that hands the same record to
// every later taker. Here each record's state is kept on the side and a
// second free or a second take is printed with the callers.
namespace
{
    std::mutex s_infoLock;
    std::vector<uint8_t> s_infoOut(65536, 0);
    std::atomic<int> s_infoTold{ 0 };

    void InfoReport(uint8_t* base, const PPCContext& ctx, const char* what, uint32_t record, uint32_t tree)
    {
        if (s_infoTold.fetch_add(1) >= 60) return;
        const uint32_t back = Guest::Read32(base, ctx.r1.u32);
        printf("animinfo: %s record %u (tree %08X), os thread %lu, from %08X %08X %08X\n", what, record, tree,
            GetCurrentThreadId(), uint32_t(ctx.lr), Guest::Read32(base, back - 8),
            Guest::Read32(base, Guest::Read32(base, back) - 8));
        fflush(stdout);
    }
}

extern "C" PPC_FUNC(__imp__sub_824FCF68);
PPC_FUNC(sub_824FCF68)
{
    const uint32_t tree = ctx.r3.u32;
    const uint32_t index = ctx.r4.u32;
    PPCContext saved = ctx;
    NotePoolThread(uint32_t(ctx.lr));
    __imp__sub_824FCF68(ctx, base);
    if (!Wanted() || ctx.r3.u32 == 0) return;
    const uint32_t record = Guest::Read16(base, tree + (index + 12) * 2);
    std::lock_guard<std::mutex> hold(s_infoLock);
    if (s_infoOut[record]) InfoReport(base, saved, "SECOND TAKE of", record, tree);
    s_infoOut[record] = 1;
}

extern "C" PPC_FUNC(__imp__sub_824F2348);
PPC_FUNC(sub_824F2348)
{
    const uint32_t record = ctx.r4.u32 & 0xFFFF;
    NotePoolThread(uint32_t(ctx.lr));
    if (Wanted())
    {
        std::lock_guard<std::mutex> hold(s_infoLock);
        if (!s_infoOut[record]) InfoReport(base, ctx, "SECOND FREE of", record, ctx.r3.u32);
        s_infoOut[record] = 0;
    }
    __imp__sub_824F2348(ctx, base);
}

// The animation info pool made anew, sub_824F9498(): all 4096 records put
// on the free list at once. Any tree still in use at that moment keeps
// records the pool now hands to others. Printed with the trees the scripts
// can still name and how many records they hold; the side table of taken
// records starts over with it.
extern "C" PPC_FUNC(__imp__sub_824F9498);
PPC_FUNC(sub_824F9498)
{
    if (Wanted())
    {
        uint32_t trees = 0, held = 0;
        std::string which;
        for (uint32_t i = 0; i < 4096; i++)
        {
            const uint32_t entity = Guest::Read32(base, 0x82A2DCD0 + i * 8);
            if (entity < 0x10000 || entity >= 0xC0000000u) continue;
            const uint32_t tree = Guest::Read32(base, entity + 552);
            if (tree < 0x10000 || tree >= 0xC0000000u) continue;
            const uint32_t list = Guest::Read32(base, tree + 8);
            const uint32_t count = list >= 0x10000 && list < 0xC0000000u ? Guest::Read32(base, list + 4) : 0;
            if (count == 0 || count > 4000) continue;
            uint32_t records = 0;
            for (uint32_t a = 0; a < count; a++)
                if (Guest::Read16(base, tree + (a + 12) * 2) != 0) records++;
            trees++;
            held += records;
            if (records != 0 && which.size() < 600)
            {
                char text[64];
                snprintf(text, sizeof text, " %08X(type %u, %u)", entity, Guest::Read8(base, entity), records);
                which += text;
            }
        }
        printf("animinfo: pool made anew from %08X; %u trees still named by entities, holding %u records:%s\n",
            uint32_t(ctx.lr), trees, held, which.c_str());
        fflush(stdout);
        std::lock_guard<std::mutex> hold(s_infoLock);
        std::fill(s_infoOut.begin(), s_infoOut.end(), uint8_t(0));
    }
    __imp__sub_824F9498(ctx, base);
}

// The whole pool checked: every live tree (the list at 0x825F5190: count,
// then the first tree; a tree's +0 is the next) and every record its slots
// hold, against the free list from the sentinel. A record held twice, or
// held and free at once, is the damage that leaves a model in its bind pose.
namespace
{
    constexpr uint32_t PoolAddress = 0x82A59DB0;
    constexpr uint32_t LiveTrees = 0x825F5190;

    // Returns true when the pool is damaged; prints only when asked to.
    bool CheckPool(uint8_t* base, const char* why, bool print = true)
    {
        std::vector<uint32_t> holder(4096, 0);
        std::vector<uint8_t> free(4096, 0);
        uint32_t trees = 0, held = 0, twice = 0, both = 0, freeCount = 0;
        std::string examples;
        for (uint32_t tree = Guest::Read32(base, LiveTrees + 4); tree >= 0x10000 && tree < 0xC0000000u && trees < 5000;
             tree = Guest::Read32(base, tree))
        {
            if (tree == LiveTrees + 8) break;
            trees++;
            const uint32_t list = Guest::Read32(base, tree + 8);
            const uint32_t count = list >= 0x10000 && list < 0xC0000000u ? Guest::Read32(base, list + 4) : 0;
            if (count > 4000) continue;
            for (uint32_t a = 0; a < count; a++)
            {
                const uint16_t record = Guest::Read16(base, tree + (a + 12) * 2);
                if (record == 0 || record >= 4096) continue;
                held++;
                if (holder[record] != 0)
                {
                    if (twice++ < 4)
                    {
                        char text[96];
                        snprintf(text, sizeof text, " [record %u in %08X and %08X/%u]", record, holder[record], tree, a);
                        examples += text;
                    }
                }
                else holder[record] = tree;
            }
        }
        uint32_t record = Guest::Read16(base, PoolAddress + 12);
        bool loops = false;
        while (record != 0 && record < 4096 && freeCount < 5000)
        {
            if (free[record]) { examples += " [free list loops]"; loops = true; break; }
            free[record] = 1;
            freeCount++;
            if (holder[record] != 0 && both++ < 4)
            {
                char text[64];
                snprintf(text, sizeof text, " [record %u free but held by %08X]", record, holder[record]);
                examples += text;
            }
            record = Guest::Read16(base, PoolAddress + record * 52 + 12);
        }
        if (print)
        {
            printf("animinfo: %s: %u live trees hold %u records; %u held twice, %u free and held, %u free%s\n",
                why, trees, held, twice, both, freeCount, examples.c_str());
            fflush(stdout);
        }
        return twice != 0 || both != 0 || loops;
    }

    // From each level start until the pool first goes bad, it is checked after
    // every outermost call into a tree, and the call that damaged it printed.
    uint8_t* s_base = nullptr;
    std::atomic<bool> s_poolWatch{ false };
    std::atomic<int> s_poolChecks{ 0 };

    void AfterTreeCall(const char* what, uint32_t tree, uint32_t index, uint32_t caller)
    {
        if (!s_poolWatch.load() || s_base == nullptr) return;
        if (s_poolChecks.fetch_add(1) > 200000) { s_poolWatch = false; return; }
        if (!CheckPool(s_base, "", false)) return;
        s_poolWatch = false;
        char why[160];
        snprintf(why, sizeof why, "FIRST DAMAGE after %s on tree %08X index %u from %08X (check %d)", what, tree, index, caller,
            s_poolChecks.load());
        CheckPool(s_base, why);
        const uint32_t list = Guest::Read32(s_base, tree + 8);
        const uint32_t count = Guest::Read32(s_base, list + 4);
        printf("animinfo:   that tree: list %08X (%u animations), +12 %08X, +16 %u; %s\n    bytes:", list, count,
            Guest::Read32(s_base, tree + 12), Guest::Read32(s_base, tree + 16), DescribeHistory(tree).c_str());
        for (uint32_t i = 0; i < 24 + 2 * (count < 40 ? count : 40); i += 4) printf(" %08X", Guest::Read32(s_base, tree + i));
        printf("\n");
        fflush(stdout);
    }
}

namespace AnimHeapTrace
{
    void WatchPool(uint8_t* base)
    {
        s_base = base;
        s_poolChecks = 0;
        s_poolWatch = !CheckPool(base, "", false);
        printf("animinfo: watching the pool from this level start: %s\n", s_poolWatch.load() ? "it is sound" : "it is already damaged");
        fflush(stdout);
    }
}

// A tree made, sub_824FD368(entity, list), and let go, sub_824F2238(tree)
// and sub_824F3F50(tree, ...): checked like the calls into a tree.
extern "C" PPC_FUNC(__imp__sub_824FD368);
PPC_FUNC(sub_824FD368)
{
    const uint32_t caller = uint32_t(ctx.lr);
    t_treeDepth++;
    __imp__sub_824FD368(ctx, base);
    if (--t_treeDepth == 0) AfterTreeCall("making a tree", ctx.r3.u32, 0, caller);
}

extern "C" PPC_FUNC(__imp__sub_824F3F50);
PPC_FUNC(sub_824F3F50)
{
    const uint32_t tree = ctx.r3.u32, caller = uint32_t(ctx.lr);
    t_treeDepth++;
    __imp__sub_824F3F50(ctx, base);
    if (--t_treeDepth == 0) AfterTreeCall("clearing a tree", tree, 0, caller);
}

extern "C" PPC_FUNC(__imp__sub_824F2238);
PPC_FUNC(sub_824F2238)
{
    const uint32_t tree = ctx.r3.u32, caller = uint32_t(ctx.lr);
    t_treeDepth++;
    __imp__sub_824F2238(ctx, base);
    if (--t_treeDepth == 0) AfterTreeCall("freeing a tree", tree, 0, caller);
}

extern "C" PPC_FUNC(__imp__sub_824F7F90);
PPC_FUNC(sub_824F7F90)
{
    if (Wanted()) CheckPool(base, "before the restart lets go of every animation");
    __imp__sub_824F7F90(ctx, base);
    if (Wanted()) CheckPool(base, "after it");
}

namespace AnimHeapTrace
{
    void CheckPoolNow(uint8_t* base, const char* why) { CheckPool(base, why); }
}

// Which threads take and give back records: a pool with no lock of its own
// is only safe while one thread uses it.
namespace
{
    std::mutex s_threadLock;
    std::map<std::pair<DWORD, uint32_t>, uint32_t> s_poolThreads;   // (thread, caller) -> calls

    void NotePoolThread(uint32_t caller)
    {
        if (!Wanted()) return;
        std::lock_guard<std::mutex> hold(s_threadLock);
        const uint32_t calls = ++s_poolThreads[{ GetCurrentThreadId(), caller }];
        if (calls == 1)
        {
            printf("animinfo: pool used by os thread %lu from %08X\n", GetCurrentThreadId(), caller);
            fflush(stdout);
        }
    }
}
