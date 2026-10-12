// The native path's draw state, recorded at the title's draw calls and held
// against the register file (native_state.h, docs/native-d3d11-plan.md).
//
// Where the device keeps what, from the decompiled library: the dirty masks
// at +0 .. +32 say which of these go out before a draw, and the copies they
// go out from are
//   +1024   the fetch constants, 0x4800.., 192 words
//   +1792   the vertex float constants, 0x4000.., 1024 words
//   +5888   the pixel float constants, 0x4400.., 1024 words
//   +9984   the boolean and loop constants, 0x4900.., 40 words
//   +10240  0x2000..0x200F (the surfaces)
//   +10304  0x2080..0x2082 (the window offset and scissor)
//   +10316  0x2100..0x2114
//   +10400  0x2180..0x2184
//   +10420  0x2200..0x220B
//   +10468  0x2280..0x2294
//   +10552  0x2300..0x2325
//   +10704  0x2380..0x2387
// and the shader objects at +12416 (pixel) and +12420 (vertex).
//
// The shadows are not the whole of it. The title's own renderer writes
// packets straight into the device's command buffer: a material's state
// is a block of register writes and SET_CONSTANTs prepared beforehand
// (sub_821563A8 and its kin copy it in after room from sub_82156B38), and
// it clears the dirty bits of what it wrote, so the shadows keep older
// values. Float constants come from memory too (LOAD_ALU_CONSTANT) and
// from room in the command buffer the title fills (sub_822F8D38). And when
// the device draws in two passes (a Z pass and the colour pass, +10808 bit
// 6), the shader load writes the Z pass its own mode, program control and
// position-only vertex program, predicated on its bins.
//
// So each device has a model of what its own command stream has set,
// read from the stream in the order it was written: at each draw call up
// to the draw, and at each kick (sub_822F2818) to the end of the segment.
// It knows each register's last value, per float4 constant whether it is
// to come from memory, and per pass the programs and what predicated
// packets set. A draw's record is the model at its packet, the shadows
// standing in for what the stream has not said yet.

#include "native_state.h"
#include "kernel.h"
#include "gpu.h"
#include "render.h"
#include "render_shaders.h"
#include "render_state.h"
#include "settings.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace
{
    struct Block { uint32_t first, count, offset; };
    constexpr Block Blocks[] = {
        { 0x2000, 16, 10240 }, { 0x2080, 3, 10304 }, { 0x2100, 21, 10316 }, { 0x2180, 5, 10400 },
        { 0x2200, 12, 10420 }, { 0x2280, 21, 10468 }, { 0x2300, 38, 10552 }, { 0x2380, 8, 10704 },
        { 0x4800, 192, 1024 }, { 0x4000, 1024, 1792 }, { 0x4400, 1024, 5888 }, { 0x4900, 40, 9984 },
    };
    constexpr uint32_t ShadowWords = 16 + 3 + 21 + 5 + 12 + 21 + 38 + 8 + 192 + 1024 + 1024 + 40;

    // Where a register is in a record's copy; ShadowWords for none. A table
    // by register: the stream writes thousands of registers a frame.
    struct CopyIndexTable
    {
        uint16_t at[0x5000];
        // How many registers from this one on are in the copy one after
        // another (a packet's run of them is then one run of the copy).
        uint16_t run[0x5000];
        CopyIndexTable()
        {
            for (uint32_t reg = 0; reg < 0x5000; reg++) at[reg] = uint16_t(ShadowWords);
            uint32_t next = 0;
            for (const Block& block : Blocks)
                for (uint32_t i = 0; i < block.count; i++) at[block.first + i] = uint16_t(next++);
            for (uint32_t reg = 0x5000; reg-- > 0;)
                run[reg] = at[reg] >= ShadowWords ? 0
                         : reg + 1 < 0x5000 && at[reg + 1] == at[reg] + 1 ? uint16_t(run[reg + 1] + 1) : 1;
        }
    };
    const CopyIndexTable g_copyIndex;
    inline uint32_t CopyIndex(uint32_t reg) { return reg < 0x5000 ? g_copyIndex.at[reg] : ShadowWords; }
    inline uint32_t RunFrom(uint32_t reg) { return reg < 0x5000 ? g_copyIndex.run[reg] : 0; }

    // And the other way: a copy's register, and its shadow in the device.
    struct CopyTable
    {
        uint16_t reg[ShadowWords];
        uint16_t offset[ShadowWords];
        CopyTable()
        {
            uint32_t next = 0;
            for (const Block& block : Blocks)
                for (uint32_t i = 0; i < block.count; i++, next++)
                {
                    reg[next] = uint16_t(block.first + i);
                    offset[next] = uint16_t(block.offset + i * 4);
                }
        }
    };
    const CopyTable g_copy;

    // The copies are kept in chunks of 64 words: a draw's record copies
    // only the chunks its stream changed since the device's draw before and
    // shares the rest with the records before it. Nearly all of a copy is
    // constants, and a draw changes a few of them.
    constexpr uint32_t ChunkWords = 64;
    constexpr uint32_t Chunks = (ShadowWords + ChunkWords - 1) / ChunkWords;
    static_assert(Chunks <= 64, "a chunk mask is 64 bits");
    constexpr uint32_t ChunkLength(uint32_t chunk) { return chunk + 1 < Chunks ? ChunkWords : ShadowWords - chunk * ChunkWords; }

    constexpr uint32_t DeviceWrite = 40, DeviceSegmentEnd = 44;

    constexpr uint32_t OpDrawIndx = 0x22, OpDrawIndx2 = 0x36, OpImLoad = 0x27, OpImLoadImmediate = 0x2B;
    constexpr uint32_t OpSetConstant = 0x2D, OpLoadAluConstant = 0x2F, OpSetBinMaskLow = 0x60;

    // The two passes of a device drawing in bins: the replay selects bins
    // 0 and 31 for the Z pass, bin 1 for the colour pass (Gpu::BinSelect).
    // The colour pass's state is also that of everything not drawn in bins.
    constexpr uint32_t ColourPass = 0, ZPass = 1;
    constexpr uint32_t ZBins = 0x80000001u, ColourBins = 0x00000002u;

    bool Wanted(const char* name)
    {
        const char* text = getenv(name);
        return text != nullptr && text[0] != 0 && text[0] != '0';
    }
    bool CheckingNow()
    {
        static const bool on = Wanted("COD3_NATIVECHECK");
        return on;
    }
    // The native path: 0 off, 1 the draws from their records (N2), 2 the
    // buffers run from their native lists as well (N3). COD3_NATIVE=0|1|2,
    // else the settings' "Native command lists" (2). Read once: the records
    // have to be made from the start.
    int NativeMode()
    {
        static const int mode = []() {
            const char* t = getenv("COD3_NATIVE");
            if (t != nullptr && t[0] != 0) return t[0] == '2' ? 2 : t[0] == '0' ? 0 : 1;
            return Settings::Get().nativeCommands ? 2 : 0;
        }();
        return mode;
    }
    bool DrawingNow() { return NativeMode() != 0; }
    bool ExecutingNow() { return NativeMode() == 2; }
    // Whether draws are recorded at all.
    bool RecordingNow()
    {
        static const bool on = CheckingNow() || DrawingNow() || ExecutingNow();
        return on;
    }

    // The library's own physical address (the window at 0xE0000000 is 4 KB on).
    uint32_t Physical(uint32_t address)
    {
        return ((((address >> 20) & 0xFFF) + 512) & 0x1000) + (address & 0x1FFFFFFF);
    }

    // A register, by the index a SET_CONSTANT or LOAD_ALU_CONSTANT gives.
    bool ConstantIndex(uint32_t offsetAndType, uint32_t& index)
    {
        index = offsetAndType & 0x7FF;
        switch ((offsetAndType >> 16) & 0xFF)
        {
        case 0: index += 0x4000; return true;
        case 1: index += 0x4800; return true;
        case 2: index += 0x4900; return true;
        case 3: index += 0x4908; return true;
        case 4: index += 0x2000; return true;
        default: return false;
        }
    }

    struct Program { uint32_t address = 0, dwords = 0; uint64_t hash = 0; };

    Program LoadedProgram(uint8_t* base, uint32_t physical, uint32_t dwords)
    {
        Program program{ physical, dwords, 0 };
        if (dwords != 0 && dwords <= 0x4000) program.hash = RenderShaders::HashMicrocode(base + Guest::PhysicalAlias(physical), dwords);
        return program;
    }

    // What a device's command stream has set, in the order it was written.
    struct DeviceModel
    {
        bool started = false;
        uint32_t scanned = 0;                // the last word read
        uint32_t segmentEnd = 0;             // the end of the segment it is in (device+44)
        uint32_t binMask = ~0u;              // SET_BIN_MASK_LO's last
        uint32_t registers[ShadowWords];
        bool known[ShadowWords] = {};
        uint32_t unknown = ShadowWords;      // how many are not known yet
        uint32_t constantMemory[512] = {};   // per float4 (vertex, then pixel): the physical address it is loaded from, 0 none
        uint64_t fromMemory[8] = {};         // which of those are not nought, a bit each: a draw's record lists them
        Program vertex[2], pixel[2];         // by pass
        // Registers whose value in a pass is not the registers' own: by copy
        // index, the passes (bit 0 colour, 1 Z) and the values; the indices
        // that have any, perhaps more than once once some were cleared
        // (`passListStale`); how many have each. The registers hold what the
        // colour pass sees, so most of these are the Z pass's.
        uint8_t passMask[ShadowWords] = {};
        uint32_t passValue[2][ShadowWords];
        std::vector<uint16_t> passList;
        uint32_t passCount[2] = {};
        bool passListStale = false;
        // The chunks changed since the last record, where each one's content
        // went last (an arena serial), and how many words of each are not
        // known yet.
        uint64_t dirty = ~0ull;
        uint64_t chunkSerial[Chunks] = {};
        uint8_t unknownIn[Chunks];
        DeviceModel() { for (uint32_t c = 0; c < Chunks; c++) unknownIn[c] = uint8_t(ChunkLength(c)); }
    };
    std::mutex g_modelMutex;
    std::unordered_map<uint32_t, std::unique_ptr<DeviceModel>> g_devices;

    DeviceModel& ModelOf(uint32_t device)
    {
        std::unique_ptr<DeviceModel>& model = g_devices[device];
        if (!model) model.reset(new DeviceModel);
        return *model;
    }

    // A draw packet's state.
    struct Record
    {
        uint32_t lastWordValue = 0;
        uint32_t initiator = 0;
        uint32_t device = 0;
        uint64_t chunks[Chunks];   // the copy, by chunk: arena serials
        uint64_t oldest = 0;       // the oldest of them
        std::vector<std::pair<uint16_t, uint32_t>> constantMemory;   // float4 index: physical address
        Program vertex[2], pixel[2];
        std::vector<std::pair<uint32_t, uint32_t>> passRegisters[2];
    };

    // The records, a ring of them, found by their packet's last word.
    constexpr uint32_t RecordCount = 8192;
    std::mutex g_mutex;
    std::unique_ptr<Record[]> g_records;
    std::vector<uint32_t> g_keys;
    uint32_t g_next = 0;

    // The slots by key (under g_mutex): eight ways a set, nothing allocated.
    // A way holds a key and its slot + 1; it is right while that slot's key
    // is still the key.
    constexpr uint32_t KeySetBits = 14;
    struct Way { uint32_t key = 0, slot = 0; };
    constexpr uint32_t KeyWays = 8;
    Way g_keyTable[1u << KeySetBits][KeyWays];
    Way* KeySet(uint32_t key) { return g_keyTable[((key >> 2) * 0x9E3779B1u) >> (32 - KeySetBits)]; }
    int32_t FindSlot(uint32_t key)
    {
        if (!g_records) return -1;
        const Way* set = KeySet(key);
        for (uint32_t i = 0; i < KeyWays; i++)
            if (const Way& way = set[i]; way.slot != 0 && way.key == key && g_keys[way.slot - 1] == key) return int32_t(way.slot - 1);
        return -1;
    }
    void PutSlot(uint32_t key, uint32_t slot)
    {
        // The key's own way, else one that is empty or gone, else the oldest.
        Way* set = KeySet(key);
        Way* victim = nullptr;
        for (uint32_t i = 0; i < KeyWays && victim == nullptr; i++)
            if (set[i].slot != 0 && set[i].key == key) victim = &set[i];
        for (uint32_t i = 0; i < KeyWays && victim == nullptr; i++)
            if (set[i].slot == 0 || g_keys[set[i].slot - 1] != set[i].key) victim = &set[i];
        if (victim == nullptr)
        {
            uint32_t oldest = 0;
            for (uint32_t i = 0; i < KeyWays; i++)
            {
                const uint32_t age = (g_next - set[i].slot) % RecordCount;   // how many records ago it was made
                if (victim == nullptr || age > oldest) { victim = &set[i]; oldest = age; }
            }
        }
        victim->key = key;
        victim->slot = slot + 1;
    }

    // The chunks, a ring of them by serial (under g_mutex). A chunk is there
    // until ArenaChunks more have been made after it; a record keeps none
    // older than half of that (Keep copies those again), so it lasts at
    // least ArenaChunks / 2 chunks after it is made.
    constexpr uint64_t ArenaChunks = 1u << 17;   // 32 MB
    std::unique_ptr<uint32_t[]> g_arena;
    // The next serial. Taken before the chunk is written: whoever reads a
    // chunk outside g_mutex looks at this after the read, and a chunk being
    // written over is then already out of reach.
    std::atomic<uint64_t> g_arenaNext{ 0 };
    uint32_t* ArenaAt(uint64_t serial) { return g_arena.get() + (serial % ArenaChunks) * ChunkWords; }
    // Whether a record's chunks are all still there, with `slack` chunks to spare.
    bool Whole(uint64_t oldest, uint64_t slack = 0) { return g_arenaNext.load() - oldest + slack <= ArenaChunks; }

    // A chunk's words by register: runs of consecutive registers.
    struct Run { uint16_t reg, copy, count; };
    struct ChunkRuns
    {
        Run runs[Chunks][8];
        uint8_t count[Chunks] = {};
        ChunkRuns()
        {
            uint32_t copy = 0;
            for (const Block& block : Blocks)
                for (uint32_t i = 0; i < block.count; i++, copy++)
                {
                    const uint32_t c = copy / ChunkWords;
                    Run* last = count[c] != 0 ? &runs[c][count[c] - 1] : nullptr;
                    if (last != nullptr && last->reg + last->count == block.first + i) last->count++;
                    else runs[c][count[c]++] = Run{ uint16_t(block.first + i), uint16_t(copy), 1 };
                }
        }
    };
    const ChunkRuns g_chunkRuns;

    // Every packet of the devices' streams as it was read, by physical
    // address: for each word whether a packet started there, and whether the
    // command processor still has to run it when the draws come from
    // records; and the header it started with. A buffer is run from these
    // as far as its packets follow each other here unchanged. In pages of
    // 64 KB, made as the streams reach them: written by the scan before the
    // kick, read by the command processor after it.
    enum : uint8_t { NoStart = 0, StateStart = 1, RunStart = 2 };
    struct StreamPage { uint32_t header[0x4000]; uint8_t start[0x4000]; };
    std::atomic<StreamPage*> g_streamPages[0x20000000 >> 16];

    StreamPage* PageOf(uint32_t physical, bool make)
    {
        std::atomic<StreamPage*>& slot = g_streamPages[(physical & 0x1FFFFFFF) >> 16];
        StreamPage* page = slot.load(std::memory_order_acquire);
        if (page != nullptr || !make) return page;
        StreamPage* made = new StreamPage{};
        if (slot.compare_exchange_strong(page, made, std::memory_order_acq_rel)) return made;
        delete made;
        return page;
    }

    uint32_t PacketDwords(uint32_t header)
    {
        const uint32_t type = header >> 30;
        return type == 0 || type == 3 ? ((header >> 16) & 0x3FFF) + 2 : type == 1 ? 3 : 1;
    }

    struct StreamItem { uint32_t physical; uint32_t header; bool run; };
    struct RunTally { uint64_t native = 0, uncovered = 0, unrecorded = 0, items = 0, skipped = 0, tails = 0, drawTails = 0; };
    RunTally g_runs;   // the command processor's
    std::chrono::steady_clock::time_point g_runsReported = std::chrono::steady_clock::now();

    // The packets read, in order, over what they cover.
    void Spanned(const std::vector<StreamItem>& items)
    {
        for (const StreamItem& item : items)
        {
            // The packet's first word marked, the rest of it as no start, a
            // page's part at a time.
            const uint32_t dwords = PacketDwords(item.header);
            for (uint32_t i = 0; i < dwords;)
            {
                const uint32_t at = item.physical + i * 4;
                StreamPage* page = PageOf(at, true);
                const uint32_t word = (at & 0xFFFF) >> 2;
                const uint32_t here = std::min(dwords - i, 0x4000u - word);
                if (i == 0)
                {
                    page->header[word] = item.header;
                    page->start[word] = item.run ? RunStart : StateStart;
                    memset(page->start + word + 1, NoStart, here - 1);
                }
                else
                    memset(page->start + word, NoStart, here);
                i += here;
            }
        }
    }

    // Whether a packet still has to be run by the command processor when
    // its draws come from records: everything but the state the records
    // hold (register writes inside a draw's registers, constants, programs)
    // and fillers.
    bool StillRun(uint8_t* base, uint32_t at, uint32_t header)
    {
        // COD3_NATIVEALL=1: every packet stays, for telling the lists from what they leave out.
        static const bool all = getenv("COD3_NATIVEALL") != nullptr;
        if (all) return true;
        const uint32_t type = header >> 30;
        const uint32_t count = ((header >> 16) & 0x3FFF) + 1;
        if (type == 2) return false;
        if (type == 0)
        {
            const uint32_t first = header & 0x7FFF;
            const bool one = (header & 0x8000) != 0;
            if (RunFrom(first) >= (one ? 1 : count)) return false;
            for (uint32_t i = 0; i < (one ? 1 : count); i++)
                if (CopyIndex(first + i) >= ShadowWords) return true;
            return false;
        }
        if (type != 3) return true;
        const uint32_t opcode = (header >> 8) & 0x7F;
        if (opcode == OpLoadAluConstant || opcode == OpImLoad || opcode == OpImLoadImmediate) return false;
        if (opcode == OpSetConstant)
        {
            uint32_t index;
            if (!ConstantIndex(Guest::Read32(base, at + 4), index)) return true;
            if (RunFrom(index) + 1 >= count) return false;
            for (uint32_t i = 0; i + 1 < count; i++)
                if (CopyIndex(index + i) >= ShadowWords) return true;
            return false;
        }
        return true;
    }

    // What the check found, since the last report.
    struct Mismatch { uint64_t count = 0; uint32_t native = 0, file = 0, writer = 0, pass = 0, draw = 0; uint32_t around[48] = {}; };
    struct Tally
    {
        uint64_t recorded = 0, unparsed = 0, resyncs = 0, unknown = 0;
        uint64_t checked = 0, unmatched = 0, stale = 0, equal = 0;
        uint64_t passes[2] = {};
        uint64_t initiatorDiffers = 0;
        uint64_t vertexDiffers = 0, pixelDiffers = 0, colourDraws = 0;
        uint64_t memoryConstants = 0;
        uint64_t opcodes[128] = {};               // the type 3 packets the devices' streams hold
        uint64_t type0 = 0, type2 = 0;
        std::map<uint32_t, Mismatch> registers;
    };
    Tally g_tally;
    std::chrono::steady_clock::time_point g_lastReport = std::chrono::steady_clock::now();

    const char* RegisterName(uint32_t reg)
    {
        switch (reg)
        {
        case 0x2000: return "SURFACE_INFO"; case 0x2001: return "COLOR_INFO0"; case 0x2002: return "DEPTH_INFO";
        case 0x2003: return "COLOR_INFO1"; case 0x2004: return "COLOR_INFO2"; case 0x2005: return "COLOR_INFO3";
        case 0x2080: return "WINDOW_OFFSET"; case 0x2081: return "WINDOW_SCISSOR_TL"; case 0x2082: return "WINDOW_SCISSOR_BR";
        case 0x2100: return "MAX_VTX_INDX"; case 0x2101: return "MIN_VTX_INDX"; case 0x2102: return "INDX_OFFSET";
        case 0x2103: return "MULTI_PRIM_RESET"; case 0x2104: return "COLOR_MASK"; case 0x2105: return "BLEND_RED";
        case 0x2106: return "BLEND_GREEN"; case 0x2107: return "BLEND_BLUE"; case 0x2108: return "BLEND_ALPHA";
        case 0x210D: return "STENCILREFMASK"; case 0x210E: return "ALPHA_REF";
        case 0x210F: return "VPORT_XSCALE"; case 0x2110: return "VPORT_XOFFSET"; case 0x2111: return "VPORT_YSCALE";
        case 0x2112: return "VPORT_YOFFSET"; case 0x2113: return "VPORT_ZSCALE"; case 0x2114: return "VPORT_ZOFFSET";
        case 0x2180: return "SQ_PROGRAM_CNTL"; case 0x2181: return "SQ_CONTEXT_MISC";
        case 0x2200: return "DEPTHCONTROL"; case 0x2201: return "BLENDCONTROL0"; case 0x2202: return "COLORCONTROL";
        case 0x2203: return "HIZCONTROL"; case 0x2204: return "CLIP_CNTL"; case 0x2205: return "SU_SC_MODE_CNTL";
        case 0x2206: return "VTE_CNTL"; case 0x2208: return "MODECONTROL"; case 0x2280: return "POINT_SIZE";
        case 0x2318: return "COPY_CONTROL";
        default: return nullptr;
        }
    }

    void Report()
    {
        const auto now = std::chrono::steady_clock::now();
        if (now - g_lastReport < std::chrono::seconds(5)) return;
        g_lastReport = now;
        Tally& t = g_tally;
        printf("native state, the last five seconds: %llu draw packets recorded (%llu scans not parsed, %llu resyncs, %llu registers not yet known)\n",
            (unsigned long long)t.recorded, (unsigned long long)t.unparsed, (unsigned long long)t.resyncs, (unsigned long long)t.unknown);
        printf("  %llu draws checked (%llu colour pass, %llu Z pass): %llu without a record, %llu with a stale one, %llu the same in every register\n",
            (unsigned long long)t.checked, (unsigned long long)t.passes[0], (unsigned long long)t.passes[1],
            (unsigned long long)t.unmatched, (unsigned long long)t.stale, (unsigned long long)t.equal);
        printf("  initiator differs %llu; vertex program differs %llu, pixel program differs %llu of %llu colour draws; %llu constants from memory\n",
            (unsigned long long)t.initiatorDiffers, (unsigned long long)t.vertexDiffers, (unsigned long long)t.pixelDiffers,
            (unsigned long long)t.colourDraws, (unsigned long long)t.memoryConstants);
        std::vector<std::pair<uint32_t, Mismatch>> list(t.registers.begin(), t.registers.end());
        std::sort(list.begin(), list.end(), [](const auto& a, const auto& b) { return a.second.count > b.second.count; });
        uint32_t constants[3] = {}, fetches = 0, bools = 0;
        for (const auto& [reg, m] : list)
        {
            if (reg >= 0x4000 && reg < 0x4400) constants[0]++;
            else if (reg >= 0x4400 && reg < 0x4800) constants[1]++;
            else if (reg >= 0x4800 && reg < 0x48C0) fetches++;
            else if (reg >= 0x4900) bools++;
        }
        printf("  packets in the devices' streams: type 0 x%llu, type 2 x%llu, type 3:", (unsigned long long)t.type0, (unsigned long long)t.type2);
        for (uint32_t op = 0; op < 128; op++)
            if (t.opcodes[op] != 0) printf(" %02X x%llu", op, (unsigned long long)t.opcodes[op]);
        printf("\n");
        printf("  registers that differed: %zu (vertex constants %u, pixel constants %u, fetch constants %u, boolean and loop %u)\n",
            list.size(), constants[0], constants[1], fetches, bools);
        int shown = 0;
        for (const auto& [reg, m] : list)
        {
            if (shown++ >= 16) break;
            const char* name = RegisterName(reg);
            printf("    %04X %-18s x%-8llu e.g. %s pass: native %08X, register file %08X, written by the packet at %08X:",
                reg, name != nullptr ? name : "", (unsigned long long)m.count, m.pass == ZPass ? "Z" : "colour", m.native, m.file, m.writer);
            // The words around the writer: a few for a constant, more for a register.
            const uint32_t from = reg >= 0x4000 ? 28 : 0, to = reg >= 0x4000 ? 40 : 48;
            for (uint32_t i = from; i < to; i++) printf("%s%s%08X", reg < 0x4000 && i % 16 == 0 ? "\n        " : "", i == 32 ? " [" : " ", m.around[i]);
            if (reg < 0x4000) printf("\n        the draw's last word at %08X", m.draw);
            printf("\n");
        }
        fflush(stdout);
        t = Tally{};
    }

    // A record for one draw packet, from the device's model as it is at the
    // packet, the shadows standing in for what it does not know.
    // (`passes`: the passes the draw packet runs in, by its predicate.)
    void Keep(uint8_t* base, uint32_t device, uint32_t lastWord, uint32_t initiator, uint32_t passes, DeviceModel& model)
    {
        const uint32_t key = Physical(lastWord);
        // The pass values still in force, the list kept short: gone through
        // only when some were cleared since.
        if (model.passListStale)
        {
            size_t kept = 0;
            for (uint16_t at : model.passList)
                if ((model.passMask[at] & 3) != 0 && (model.passMask[at] & 0x80) == 0)
                {
                    model.passMask[at] |= 0x80;   // listed once
                    model.passList[kept++] = at;
                }
            model.passList.resize(kept);
            for (uint16_t at : model.passList) model.passMask[at] &= 0x7F;
            model.passListStale = false;
        }

        // Made once, under the lock the readers hold (not taken again for
        // every draw: the command processor holds it for each of its own).
        static std::once_flag made;
        std::call_once(made, []() {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_records.reset(new Record[RecordCount]);
            g_keys.assign(RecordCount, 0);
            g_arena.reset(new uint32_t[ArenaChunks * ChunkWords]);
        });

        // The record is made here, the changed chunks going into the arena
        // (one writer: this is under g_modelMutex), and only put into its
        // slot under g_mutex, which the command processor takes for every
        // draw it makes.
        static Record staging;
        staging.lastWordValue = Guest::Read32(base, lastWord);
        staging.initiator = initiator;
        staging.device = device;
        uint32_t unknown = 0;
        staging.oldest = ~0ull;
        for (uint32_t c = 0; c < Chunks; c++)
        {
            if (((model.dirty >> c) & 1) != 0 || model.unknownIn[c] != 0 || g_arenaNext.load(std::memory_order_relaxed) - model.chunkSerial[c] > ArenaChunks / 2)
            {
                const uint64_t serial = g_arenaNext.fetch_add(1);
                uint32_t* to = ArenaAt(serial);
                const uint32_t from = c * ChunkWords;
                if (model.unknownIn[c] == 0) memcpy(to, model.registers + from, ChunkLength(c) * 4);
                else
                    for (uint32_t i = 0; i < ChunkLength(c); i++)
                    {
                        if (model.known[from + i]) to[i] = model.registers[from + i];
                        else { to[i] = Guest::Read32(base, device + g_copy.offset[from + i]); unknown++; }
                    }
                model.chunkSerial[c] = serial;
            }
            staging.chunks[c] = model.chunkSerial[c];
            staging.oldest = std::min(staging.oldest, model.chunkSerial[c]);
        }
        model.dirty = 0;
        // The constants from memory, by their bits: nearly every draw has
        // some (a shader's own literals), and looking at all 512 for them
        // was a good part of what a record cost.
        staging.constantMemory.clear();
        for (uint32_t w = 0; w < 8; w++)
            for (uint64_t bits = model.fromMemory[w]; bits != 0; bits &= bits - 1)
            {
                const uint32_t i = w * 64 + uint32_t(__builtin_ctzll(bits));
                staging.constantMemory.emplace_back(uint16_t(i), model.constantMemory[i]);
            }
        for (uint32_t pass = 0; pass < 2; pass++)
        {
            staging.vertex[pass] = model.vertex[pass];
            staging.pixel[pass] = model.pixel[pass];
            staging.passRegisters[pass].clear();
        }
        // The Z pass's values only for a draw that runs in the Z pass: one
        // predicated to the colour pass alone never does (the command
        // processor runs it only when the bin select shares a bit with its
        // mask, and then picks the colour pass).
        const bool colour = model.passCount[ColourPass] != 0;
        const bool z = (passes & 2) != 0 && model.passCount[ZPass] != 0;
        if (colour || z)
            for (uint16_t at : model.passList)
            {
                if (colour && (model.passMask[at] & 1) != 0) staging.passRegisters[ColourPass].emplace_back(g_copy.reg[at], model.passValue[ColourPass][at]);
                if (z && (model.passMask[at] & 2) != 0) staging.passRegisters[ZPass].emplace_back(g_copy.reg[at], model.passValue[ZPass][at]);
            }

        std::lock_guard<std::mutex> lock(g_mutex);
        const uint32_t slot = g_next++ % RecordCount;
        Record& record = g_records[slot];
        record.lastWordValue = staging.lastWordValue;
        record.initiator = staging.initiator;
        record.device = staging.device;
        memcpy(record.chunks, staging.chunks, sizeof(record.chunks));
        record.oldest = staging.oldest;
        record.constantMemory.swap(staging.constantMemory);
        for (uint32_t pass = 0; pass < 2; pass++)
        {
            record.vertex[pass] = staging.vertex[pass];
            record.pixel[pass] = staging.pixel[pass];
            record.passRegisters[pass].swap(staging.passRegisters[pass]);
        }
        g_keys[slot] = key;
        PutSlot(key, slot);
        g_tally.recorded++;
        g_tally.unknown += unknown;
    }

    // The passes a predicated packet runs in, under that bin mask.
    uint32_t PassesOf(uint32_t header, uint32_t binMask)
    {
        if ((header & 1) == 0) return 3;
        return ((binMask & ColourBins) != 0 ? 1u : 0u) | ((binMask & ZBins) != 0 ? 2u : 0u);
    }

    // A pass's own value for a register, and one taken back.
    inline void SetPassValue(DeviceModel& model, uint32_t at, uint32_t pass, uint32_t value)
    {
        model.passValue[pass][at] = value;
        const uint8_t bit = uint8_t(1u << pass);
        if ((model.passMask[at] & bit) != 0) return;
        if ((model.passMask[at] & 3) == 0) model.passList.push_back(uint16_t(at));
        model.passMask[at] |= bit;
        model.passCount[pass]++;
    }
    inline void ClearPassValue(DeviceModel& model, uint32_t at, uint32_t pass)
    {
        const uint8_t bit = uint8_t(1u << pass);
        if ((model.passMask[at] & bit) == 0) return;
        model.passMask[at] &= uint8_t(~bit);
        model.passCount[pass]--;
        if ((model.passMask[at] & 3) == 0) model.passListStale = true;
    }

    // Whether a float constant is loaded from memory at the draw.
    inline bool FromMemory(const DeviceModel& model, uint32_t reg)
    {
        return reg >= 0x4000 && reg < 0x4800 && model.constantMemory[(reg - 0x4000) >> 2] != 0;
    }

    // A register written, in the passes given.
    inline void Written(DeviceModel& model, uint32_t reg, uint32_t value, uint32_t passes)
    {
        const uint32_t at = CopyIndex(reg);
        if (at >= ShadowWords) return;
        if (passes == 3)
        {
            model.registers[at] = value;
            model.dirty |= 1ull << (at / ChunkWords);
            if (!model.known[at]) { model.known[at] = true; model.unknown--; model.unknownIn[at / ChunkWords]--; }
            if (model.passMask[at] != 0) { ClearPassValue(model, at, ColourPass); ClearPassValue(model, at, ZPass); }
            if (reg >= 0x4000 && reg < 0x4800)
            {
                const uint32_t constant = (reg - 0x4000) >> 2;
                uint32_t& memory = model.constantMemory[constant];
                if (memory != 0) { memory = 0; model.fromMemory[constant >> 6] &= ~(1ull << (constant & 63)); }
            }
            return;
        }
        // A write for the colour pass alone goes into the registers, which
        // hold what that pass sees, the Z pass keeping the value it had as
        // its own: the title writes a draw's vertex constants so (some
        // seventy words a draw in the forest), and as pass values every
        // draw's record copied them all and every draw put them over its
        // image. Not when the old value is not known yet, or the constant
        // comes from memory (put over the registers at the draw): then it
        // is the colour pass's own value.
        if (passes == 1 && model.known[at] && !FromMemory(model, reg))
        {
            if ((model.passMask[at] & 2) == 0) SetPassValue(model, at, ZPass, model.registers[at]);
            ClearPassValue(model, at, ColourPass);
            model.registers[at] = value;
            model.dirty |= 1ull << (at / ChunkWords);
            return;
        }
        for (uint32_t pass = 0; pass < 2; pass++)
            if (passes & (1u << pass)) SetPassValue(model, at, pass, value);
    }

    // A packet's run of `n` registers from `reg` (its words at `words`, in
    // guest order), as Written does them one by one, a run at a time: the
    // common case, every register known and the run one run of the copy.
    // False when it is not that case.
    bool WrittenRun(DeviceModel& model, uint32_t reg, const uint8_t* words, uint32_t n, uint32_t passes)
    {
        if (n == 0) return true;
        if ((passes != 3 && passes != 1) || model.unknown != 0 || RunFrom(reg) < n) return false;
        const uint32_t at = CopyIndex(reg);
        const uint32_t firstChunk = at / ChunkWords, lastChunk = (at + n - 1) / ChunkWords;
        const uint64_t chunks = (lastChunk - firstChunk == 63 ? ~0ull : (1ull << (lastChunk - firstChunk + 1)) - 1) << firstChunk;
        if (passes == 3)
        {
            for (uint32_t i = 0; i < n; i++)
            {
                uint32_t w;
                memcpy(&w, words + i * 4, 4);
                model.registers[at + i] = __builtin_bswap32(w);
            }
            if (model.passCount[ColourPass] + model.passCount[ZPass] != 0)
                for (uint32_t i = 0; i < n; i++)
                    if (model.passMask[at + i] != 0) { ClearPassValue(model, at + i, ColourPass); ClearPassValue(model, at + i, ZPass); }
            if (reg < 0x4800 && reg + n > 0x4000)
            {
                const uint32_t first = (std::max(reg, 0x4000u) - 0x4000) >> 2, last = (std::min(reg + n, 0x4800u) - 1 - 0x4000) >> 2;
                for (uint32_t constant = first; constant <= last; constant++)
                    if (model.constantMemory[constant] != 0)
                    {
                        model.constantMemory[constant] = 0;
                        model.fromMemory[constant >> 6] &= ~(1ull << (constant & 63));
                    }
            }
        }
        else
            for (uint32_t i = 0; i < n; i++)
            {
                uint32_t w;
                memcpy(&w, words + i * 4, 4);
                const uint32_t value = __builtin_bswap32(w);
                if (FromMemory(model, reg + i)) { SetPassValue(model, at + i, ColourPass, value); continue; }
                if ((model.passMask[at + i] & 2) == 0) SetPassValue(model, at + i, ZPass, model.registers[at + i]);
                ClearPassValue(model, at + i, ColourPass);
                model.registers[at + i] = value;
            }
        model.dirty |= chunks;
        return true;
    }

    // The device's packets after model.scanned up to `last` (the last word
    // to read), into the model; a record at each draw.
    void Scan(uint8_t* base, uint32_t device, DeviceModel& model, uint32_t last)
    {
        uint32_t at = model.scanned + 4;
        const uint32_t first = at;
        static std::vector<StreamItem> items;   // under g_modelMutex
        items.clear();
        while (at <= last)
        {
            const uint32_t header = Guest::Read32(base, at);
            if (ExecutingNow()) items.push_back(StreamItem{ Physical(at), header, StillRun(base, at, header) });
            const uint32_t type = header >> 30;
            const uint32_t count = ((header >> 16) & 0x3FFF) + 1;
            uint32_t length = 1;
            if (type == 0)
            {
                const bool one = (header & 0x8000) != 0;
                // The words up to the last one to read.
                const uint32_t words = last >= at + 4 ? std::min(count, (last - at) / 4) : 0;
                if (one || !WrittenRun(model, header & 0x7FFF, base + at + 4, words, 3))
                    for (uint32_t i = 0; i < words; i++)
                        Written(model, (header & 0x7FFF) + (one ? 0 : i), Guest::Read32(base, at + 4 + i * 4), 3);
                length = 1 + count;
            }
            else if (type == 1) length = 3;
            else if (type == 3)
            {
                const uint32_t opcode = (header >> 8) & 0x7F;
                if (CheckingNow()) { std::lock_guard<std::mutex> lock(g_mutex); g_tally.opcodes[opcode]++; }
                const uint32_t passes = PassesOf(header, model.binMask);
                length = 1 + count;
                auto word = [&](uint32_t i) { return Guest::Read32(base, at + 4 + i * 4); };
                if (opcode == OpSetBinMaskLow) model.binMask = word(0);
                else if (opcode == OpSetConstant)
                {
                    uint32_t index;
                    if (ConstantIndex(word(0), index) && !WrittenRun(model, index, base + at + 8, count - 1, passes))
                        for (uint32_t i = 0; i + 1 < count; i++) Written(model, index + i, word(1 + i), passes);
                }
                else if (opcode == OpLoadAluConstant)
                {
                    uint32_t index;
                    if (ConstantIndex(word(1), index) && index >= 0x4000 && index < 0x4800)
                    {
                        const uint32_t address = word(0) & ~3u, dwords = word(2) & 0xFFF;
                        for (uint32_t i = 0; i < dwords; i += 4)
                            if (index + i < 0x4800)
                            {
                                const uint32_t constant = (index + i - 0x4000) >> 2;
                                model.constantMemory[constant] = address + i * 4;
                                model.fromMemory[constant >> 6] |= 1ull << (constant & 63);
                                // Loaded in both passes: what either pass had
                                // of its own is gone.
                                if (passes == 3)
                                    for (uint32_t k = 0; k < 4; k++)
                                    {
                                        const uint32_t at = CopyIndex(index + i + k);
                                        if (at < ShadowWords && model.passMask[at] != 0)
                                        {
                                            ClearPassValue(model, at, ColourPass);
                                            ClearPassValue(model, at, ZPass);
                                        }
                                    }
                            }
                    }
                }
                else if (opcode == OpImLoad || opcode == OpImLoadImmediate)
                {
                    const bool pixel = (word(0) & 1) != 0;
                    const Program program = opcode == OpImLoad
                        ? LoadedProgram(base, word(0) & ~3u, word(1) & 0xFFFF)
                        : LoadedProgram(base, Physical(at + 12), word(1) & 0xFFFF);
                    for (uint32_t pass = 0; pass < 2; pass++)
                        if (passes & (1u << pass)) (pixel ? model.pixel : model.vertex)[pass] = program;
                }
                else if (opcode == OpDrawIndx || opcode == OpDrawIndx2)
                    Keep(base, device, at + count * 4, word(opcode == OpDrawIndx ? 1 : 0), passes, model);
            }
            at += length * 4;
        }
        if (at != last + 4)
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_tally.unparsed++;
        }
        else if (ExecutingNow()) Spanned(items);
        model.scanned = last;
    }

    // Whether the model can read on from where it is to `last`: the same
    // segment of the command buffer, forward.
    bool Continues(uint8_t* base, uint32_t device, const DeviceModel& model, uint32_t last)
    {
        return model.started && Guest::Read32(base, device + DeviceSegmentEnd) == model.segmentEnd &&
               last >= model.scanned && last - model.scanned < 0x400000;
    }

    // The model goes on from the device's write pointer, in its segment.
    void Restart(uint8_t* base, uint32_t device, DeviceModel& model, uint32_t at)
    {
        model.started = true;
        model.scanned = at;
        model.segmentEnd = Guest::Read32(base, device + DeviceSegmentEnd);
    }
}

bool NativeState::Checking() { return CheckingNow(); }

bool NativeState::Drawing() { return DrawingNow(); }

bool NativeState::Executing() { return ExecutingNow(); }

bool NativeState::NativeRun(uint32_t physical, uint32_t dwords, std::vector<Item>& items, uint32_t& coveredDwords)
{
    if (!ExecutingNow() || dwords == 0) return false;
    items.clear();
    const uint32_t first = physical & 0x1FFFFFFF;
    const uint32_t last = first + (dwords - 1) * 4;
    bool done = false;
    [&]() {
        // The packets as they were read, one after another from the start,
        // each still as it was; the draws with their records, looked at
        // together at the end.
        static std::vector<uint32_t> draws;   // the command processor's
        draws.clear();
        uint32_t at = first;
        size_t skipped = 0;
        while (at <= last)
        {
            const StreamPage* page = PageOf(at, false);
            if (page == nullptr) break;
            const uint32_t word = (at & 0xFFFF) >> 2;
            const uint8_t start = page->start[word];
            const uint32_t header = page->header[word];
            if (start == NoStart || Guest::Read32(Guest::Base, Guest::PhysicalAlias(at)) != header) break;
            const uint32_t dwords = PacketDwords(header);
            if (at + dwords * 4 > last + 4) break;
            const uint32_t type = header >> 30, opcode = (header >> 8) & 0x7F;
            if (type == 3 && (opcode == OpDrawIndx || opcode == OpDrawIndx2)) draws.push_back(at + (dwords - 1) * 4);
            if (start == RunStart) items.push_back(Item{ at, dwords });
            else skipped++;
            at += dwords * 4;
        }
        if (at == first) { g_runs.uncovered++; return; }
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            for (const uint32_t key : draws)
            {
                const int32_t slot = FindSlot(key);
                if (slot < 0 || !Whole(g_records[slot].oldest, ArenaChunks / 4) ||
                    g_records[slot].lastWordValue != Guest::Read32(Guest::Base, Guest::PhysicalAlias(key)))
                {
                    g_runs.unrecorded++;
                    items.clear();
                    return;
                }
            }
        }
        // The rest (the end record the kick writes after what was read) is run
        // as it is; but not when it draws, for its draws would find the
        // register file without the state the list left out.
        if (at <= last)
        {
            for (uint32_t tail = at; tail <= last;)
            {
                const uint32_t header = Guest::Read32(Guest::Base, Guest::PhysicalAlias(tail));
                const uint32_t type = header >> 30, opcode = (header >> 8) & 0x7F;
                if (type == 3 && (opcode == OpDrawIndx || opcode == OpDrawIndx2)) { g_runs.drawTails++; items.clear(); return; }
                tail += PacketDwords(header) * 4;
            }
            g_runs.tails++;
        }
        coveredDwords = (at - first) / 4;
        g_runs.native++;
        g_runs.items += items.size();
        g_runs.skipped += skipped;
        done = true;
    }();
    const auto now = std::chrono::steady_clock::now();
    if (now - g_runsReported >= std::chrono::seconds(5))
    {
        printf("native run, the last five seconds: %llu buffers from their lists (%llu packets run, %llu left to the records; %llu with a tail run as it is), "
               "run whole: %llu not read, %llu with a draw unrecorded, %llu with a draw past what was read\n",
            (unsigned long long)g_runs.native, (unsigned long long)g_runs.items, (unsigned long long)g_runs.skipped, (unsigned long long)g_runs.tails,
            (unsigned long long)g_runs.uncovered, (unsigned long long)g_runs.unrecorded, (unsigned long long)g_runs.drawTails);
        fflush(stdout);
        g_runs = RunTally{};
        g_runsReported = now;
    }
    return done;
}

namespace
{
    // The registers of the draw being made, by register number: only the
    // draw's own (the blocks above) are filled, each draw.
    uint32_t g_image[0x5000];
    // The chunk each part of it was last filled from; ~0 when it is not
    // known to be one.
    uint64_t g_imageChunks[Chunks];
    bool g_imageStarted = false;
    // The words the last draw set over its chunks (constants from memory, a
    // pass's own values), by register, with what the chunk had there: put
    // back before the next draw when that chunk stays, rather than the chunk
    // copied again (and without reading the chunk again from the arena,
    // which by then is out of the cache).
    struct Overridden { uint32_t reg, value; };
    std::vector<Overridden> g_overridden;
}

bool NativeState::Begin(uint32_t lastWordPhysical)
{
    if (!DrawingNow()) return false;
    const uint32_t key = lastWordPhysical & 0x1FFFFFFF;
    // What the image needs from the record, taken under the lock; the
    // chunks are read after it, and still there when the arena says so.
    static uint64_t chunks[Chunks];
    static std::vector<std::pair<uint16_t, uint32_t>> memory;
    static std::vector<std::pair<uint32_t, uint32_t>> passRegisters;
    uint64_t oldest;
    Program vertex, pixel;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        const int32_t slot = FindSlot(key);
        if (slot < 0) return false;
        const Record& record = g_records[slot];
        if (Guest::Read32(Guest::Base, Guest::PhysicalAlias(lastWordPhysical)) != record.lastWordValue || !Whole(record.oldest)) return false;
        const uint32_t select = uint32_t(Gpu::BinSelect());
        const uint32_t pass = (select & ZBins) != 0 && (select & ColourBins) == 0 ? ZPass : ColourPass;
        memcpy(chunks, record.chunks, sizeof(chunks));
        oldest = record.oldest;
        memory.assign(record.constantMemory.begin(), record.constantMemory.end());
        passRegisters.assign(record.passRegisters[pass].begin(), record.passRegisters[pass].end());
        vertex = record.vertex[pass];
        pixel = record.pixel[pass];
    }
    // COD3_NATIVESKIPPS=hex: the draws of pixel programs whose hash starts
    // so are made from the register file (with COD3_NATIVE=1, which keeps
    // it whole), for telling a record's fault from the program's.
    static const char* const skipPixel = getenv("COD3_NATIVESKIPPS");
    if (skipPixel != nullptr)
    {
        char name[24];
        snprintf(name, sizeof(name), "%016llx", (unsigned long long)pixel.hash);
        if (strncmp(name, skipPixel, strlen(skipPixel)) == 0) return false;
    }
    if (!g_imageStarted)
    {
        for (uint64_t& chunk : g_imageChunks) chunk = ~0ull;
        g_imageStarted = true;
    }
    // Last first: a word set twice gets the chunk's value back.
    for (size_t i = g_overridden.size(); i-- > 0;)
    {
        const Overridden& o = g_overridden[i];
        if (g_imageChunks[CopyIndex(o.reg) / ChunkWords] == chunks[CopyIndex(o.reg) / ChunkWords]) g_image[o.reg] = o.value;
    }
    g_overridden.clear();
    for (uint32_t c = 0; c < Chunks; c++)
    {
        if (g_imageChunks[c] == chunks[c]) continue;
        const uint32_t* from = ArenaAt(chunks[c]) - c * ChunkWords;   // by copy index
        for (uint32_t r = 0; r < g_chunkRuns.count[c]; r++)
        {
            const Run& run = g_chunkRuns.runs[c][r];
            memcpy(g_image + run.reg, from + run.copy, run.count * 4u);
        }
        g_imageChunks[c] = chunks[c];
    }
    if (!Whole(oldest))
    {
        // Written over while it was read: the image is anyone's.
        for (uint64_t& chunk : g_imageChunks) chunk = ~0ull;
        return false;
    }
    for (const auto& [index, address] : memory)
    {
        const uint32_t reg = 0x4000 + index * 4u;
        for (uint32_t i = 0; i < 4; i++)
        {
            g_overridden.push_back(Overridden{ reg + i, g_image[reg + i] });
            g_image[reg + i] = Guest::Read32(Guest::Base, Guest::PhysicalAlias(address + i * 4));
        }
    }
    for (const auto& [reg, value] : passRegisters)
    {
        g_overridden.push_back(Overridden{ reg, g_image[reg] });
        g_image[reg] = value;
    }

    // COD3_IMAGECHECK=1: the image, filled a chunk at a time and put back a
    // word at a time, held against one made whole from the record.
    static const bool imageCheck = getenv("COD3_IMAGECHECK") != nullptr;
    if (imageCheck)
    {
        static uint64_t checked = 0, wrong = 0;
        static auto reported = std::chrono::steady_clock::now();
        uint32_t whole[ShadowWords];
        for (uint32_t c = 0; c < Chunks; c++) memcpy(whole + c * ChunkWords, ArenaAt(chunks[c]), ChunkLength(c) * 4);
        for (const auto& [index, address] : memory)
            for (uint32_t i = 0; i < 4; i++) whole[CopyIndex(0x4000 + index * 4u + i)] = Guest::Read32(Guest::Base, Guest::PhysicalAlias(address + i * 4));
        for (const auto& [reg, value] : passRegisters) whole[CopyIndex(reg)] = value;
        bool same = true;
        for (uint32_t at = 0; at < ShadowWords && same; at++) same = whole[at] == g_image[g_copy.reg[at]];
        checked++;
        if (!same) wrong++;
        if (std::chrono::steady_clock::now() - reported > std::chrono::seconds(5))
        {
            printf("native image: %llu draws, %llu not as the record says\n", (unsigned long long)checked, (unsigned long long)wrong);
            fflush(stdout);
            checked = wrong = 0;
            reported = std::chrono::steady_clock::now();
        }
    }
    if (vertex.dwords != 0) Render::UseProgram(false, Guest::PhysicalAlias(vertex.address), vertex.dwords, vertex.hash);
    if (pixel.dwords != 0) Render::UseProgram(true, Guest::PhysicalAlias(pixel.address), pixel.dwords, pixel.hash);
    RenderState::UseRegisters(g_image);
    return true;
}

void NativeState::End() { RenderState::UseRegisters(nullptr); }

int NativeState::DiffImage(void (*tell)(uint32_t reg, uint32_t mine, uint32_t file))
{
    const std::atomic<uint32_t>* file = Gpu::RegisterFile();
    int differ = 0;
    for (uint32_t at = 0; at < ShadowWords; at++)
    {
        const uint32_t reg = g_copy.reg[at];
        const uint32_t value = file[reg].load(std::memory_order_relaxed);
        if (g_image[reg] == value) continue;
        differ++;
        tell(reg, g_image[reg], value);
    }
    return differ;
}

void NativeState::Recorded(uint8_t* base, uint32_t device, uint32_t before, uint32_t after)
{
    if (!RecordingNow()) return;
    std::lock_guard<std::mutex> modelLock(g_modelMutex);
    DeviceModel& model = ModelOf(device);
    if (!Continues(base, device, model, after))
    {
        // The model lost the stream (or never had it): on from this call.
        if (model.started)
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_tally.resyncs++;
        }
        Restart(base, device, model, before);
        if (after < before || after - before > 0x10000) { model.scanned = after; return; }
    }
    Scan(base, device, model, after);
}

void NativeState::Kicking(uint8_t* base, uint32_t device)
{
    if (!RecordingNow()) return;
    std::lock_guard<std::mutex> modelLock(g_modelMutex);
    DeviceModel& model = ModelOf(device);
    const uint32_t last = Guest::Read32(base, device + DeviceWrite);
    if (Continues(base, device, model, last)) Scan(base, device, model, last);
}

void NativeState::Kicked(uint8_t* base, uint32_t device)
{
    if (!RecordingNow()) return;
    std::lock_guard<std::mutex> modelLock(g_modelMutex);
    DeviceModel& model = ModelOf(device);
    if (!model.started) return;
    Restart(base, device, model, Guest::Read32(base, device + DeviceWrite));
}

void NativeState::Check(uint32_t lastWordPhysical, uint32_t initiator, uint32_t indexBase)
{
    (void)indexBase;
    if (!CheckingNow()) return;
    const uint32_t key = lastWordPhysical & 0x1FFFFFFF;
    const std::atomic<uint32_t>* file = Gpu::RegisterFile();
    std::lock_guard<std::mutex> lock(g_mutex);
    Tally& t = g_tally;
    t.checked++;
    const int32_t slot = FindSlot(key);
    if (slot < 0) { t.unmatched++; Report(); return; }
    const Record& record = g_records[slot];
    if (Guest::Read32(Guest::Base, Guest::PhysicalAlias(lastWordPhysical)) != record.lastWordValue) { t.stale++; Report(); return; }

    // The pass the command processor is in: the Z pass when it runs the
    // Z bins only.
    const uint32_t select = uint32_t(Gpu::BinSelect());
    const uint32_t pass = (select & ZBins) != 0 && (select & ColourBins) == 0 ? ZPass : ColourPass;
    t.passes[pass]++;

    // What the record says the registers are, for this pass.
    if (!Whole(record.oldest)) { t.stale++; Report(); return; }
    uint32_t expected[ShadowWords];
    for (uint32_t c = 0; c < Chunks; c++) memcpy(expected + c * ChunkWords, ArenaAt(record.chunks[c]), ChunkLength(c) * 4);
    for (const auto& [index, address] : record.constantMemory)
    {
        const uint32_t reg = 0x4000 + index * 4u;
        for (uint32_t i = 0; i < 4; i++) expected[CopyIndex(reg + i)] = Guest::Read32(Guest::Base, Guest::PhysicalAlias(address + i * 4));
        t.memoryConstants++;
    }
    for (const auto& [reg, value] : record.passRegisters[pass])
    {
        const uint32_t at = CopyIndex(reg);
        if (at < ShadowWords) expected[at] = value;
    }

    if (record.initiator != initiator) t.initiatorDiffers++;
    if (record.vertex[pass].hash != Render::CurrentProgramHash(false)) t.vertexDiffers++;
    if ((file[0x2208].load(std::memory_order_relaxed) & 7) == 4)
    {
        t.colourDraws++;
        if (record.pixel[pass].hash != Render::CurrentProgramHash(true)) t.pixelDiffers++;
    }

    bool equal = true;
    uint32_t at = 0;
    for (const Block& block : Blocks)
        for (uint32_t i = 0; i < block.count; i++, at++)
        {
            const uint32_t reg = block.first + i;
            const uint32_t value = file[reg].load(std::memory_order_relaxed);
            if (value == expected[at]) continue;
            equal = false;
            Mismatch& m = t.registers[reg];
            if (m.count++ == 0)
            {
                m.native = expected[at];
                m.file = value;
                m.pass = pass;
                m.writer = Gpu::RegisterWriter(reg);
                if (m.writer != 0)
                    for (uint32_t w = 0; w < 48; w++) m.around[w] = Guest::Read32(Guest::Base, Guest::PhysicalAlias(m.writer - 128 + w * 4));
                m.draw = key;
            }
        }
    if (equal) t.equal++;
    Report();
}
