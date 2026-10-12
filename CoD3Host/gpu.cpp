// The GPU side of the runtime: the register file the graphics driver writes to,
// and the command processor that reads what it queues.
//
// The driver's contract is small enough to state. It allocates a ring buffer,
// tells the GPU where it is, writes command packets into it, advances a write
// pointer register, and waits for the GPU to publish how far it has read. Some
// packets ask for an interrupt when they complete.
//
// This implements that bookkeeping and decodes the packet stream. Nothing is
// rasterised: a draw packet is counted and skipped. Turning those into pictures
// needs a shader translator and a graphics backend, which is the work after
// this.

// Ahead of kernel.h, which pulls in ppc_context.h: the memory macros there are
// guarded with #ifndef, so this file has to define its versions first or it
// would redefine them.
#include "cod3_mmio.h"
#include "modules.h"

// The recompiled code has its time base instruction redirected by that header.
// This file is host code and includes the Windows headers, which declare the
// intrinsic it replaces, so the redirection is undone here.
#undef __rdtsc

#include "kernel.h"
#include "gpu.h"
#include <cstdlib>
#include "scheduler.h"
#include "sampler.h"
#include "render.h"
#include "render_internal.h"
#include "d3d_hooks.h"
#include "native_state.h"
#include "render_state.h"
#include "timeline.h"
#include "window.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>
#include <mutex>

#include <Windows.h>

namespace
{
    // The register file: a flat array indexed by the register number, each
    // an atomic word. It was a map keyed by address under a mutex, and the
    // walk of it for every draw's snapshot was most of a draw's time; a
    // thousand draws a frame made it the frame rate.
    constexpr uint32_t RegisterCount = 0x10000;
    std::atomic<uint32_t> g_registerFile[RegisterCount];

    // How many times the constants have been written, in three ranges:
    // the vertex program's floats, the pixel program's, and the booleans
    // and loops. A draw uploads a range only when the count has moved.
    std::atomic<uint64_t> g_constantWrites[3];
    // The lowest and highest word written in each range since it was
    // last taken: a draw's own constants are a few words, and uploading
    // the whole range for them was most of the time in constants.
    std::atomic<uint32_t> g_constantDirtyLow[3] = { 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu };
    std::atomic<uint32_t> g_constantDirtyHigh[3] = { 0, 0, 0 };

    inline void NoteConstantWrite(uint32_t range, uint32_t offset)
    {
        g_constantWrites[range].fetch_add(1, std::memory_order_relaxed);
        uint32_t low = g_constantDirtyLow[range].load(std::memory_order_relaxed);
        while (offset < low && !g_constantDirtyLow[range].compare_exchange_weak(low, offset, std::memory_order_relaxed)) {}
        uint32_t high = g_constantDirtyHigh[range].load(std::memory_order_relaxed);
        while (offset + 1 > high && !g_constantDirtyHigh[range].compare_exchange_weak(high, offset + 1, std::memory_order_relaxed)) {}
    }
    std::mutex g_registerMutex;   // for the trace map below, not the file

    inline bool RegisterIndex(uint32_t address, uint32_t& index)
    {
        if (address < Gpu::ApertureBase || address >= Gpu::ApertureBase + RegisterCount * 4) return false;
        index = (address - Gpu::ApertureBase) / 4;
        return true;
    }

    std::mutex g_statisticsMutex;
    Gpu::Statistics g_statistics;

    // What actually flows through the pipe, by packet type. A frame of GPU
    // work is mostly register writes, so seeing which type 3 opcodes appear is
    // the only way to tell a real command stream from a walk over noise.
    std::mutex g_histogramMutex;
    std::map<uint32_t, uint64_t> g_type3Opcodes;
    uint64_t g_type0 = 0, g_type2 = 0, g_type3 = 0;

    void CountPacket(uint32_t type, uint32_t opcode)
    {
        std::lock_guard<std::mutex> lock(g_histogramMutex);
        if (type == 0) g_type0++;
        else if (type == 2) g_type2++;
        else if (type == 3) { g_type3++; g_type3Opcodes[opcode]++; }
    }

    // Discovery. Every distinct aperture address the driver touches is reported
    // once. This is how RegisterWritePointer was found rather than guessed, and
    // it is what will name the next register when one is needed.
    std::mutex g_discoveryMutex;
    std::map<uint32_t, bool> g_seen;

    // A driver that is waiting for the GPU polls a register in a tight loop.
    // Counting reads per address makes that loop visible, and names the
    // register the title is waiting on.
    std::mutex g_readCountMutex;
    std::map<uint32_t, uint64_t> g_readCounts;
    std::atomic<int> g_reported{ 0 };
    constexpr int ReportLimit = 48;

    void ReportOnce(const char* kind, uint32_t address, uint32_t value)
    {
        {
            std::lock_guard<std::mutex> lock(g_discoveryMutex);
            if (g_seen.find(address) != g_seen.end()) return;
            g_seen[address] = true;
        }
        if (g_reported.fetch_add(1, std::memory_order_relaxed) >= ReportLimit) return;

        printf("mmio: first %s of 0x%08X", kind, address);
        if (value != 0xFFFFFFFF) printf(" = 0x%08X", value);
        printf("\n");
        fflush(stdout);
    }

    // --- PM4 packets -------------------------------------------------------
    // A packet header's top two bits pick the format. Type 0 writes a run of
    // registers, type 2 is filler, type 3 carries an opcode.
    // The type 3 opcodes this title uses. The names are the Xenos command
    // processor's own; the ones that were showing as bare numbers turned out to
    // be shader uploads and binning state, which is exactly what a renderer
    // needs to see.
    constexpr uint32_t OpRegRmw          = 0x21;
    constexpr uint32_t OpDrawIndx        = 0x22;
    constexpr uint32_t OpImLoad          = 0x27;
    constexpr uint32_t OpImLoadImmediate = 0x2B;
    constexpr uint32_t OpSetConstant     = 0x2D;
    constexpr uint32_t OpSetConstant2    = 0x55;
    constexpr uint32_t OpSetShaderConstants = 0x56;
    constexpr uint32_t OpLoadAluConstant = 0x2F;
    constexpr uint32_t OpInvalidateState = 0x3B;
    constexpr uint32_t OpWaitRegMem      = 0x3C;
    constexpr uint32_t OpIndirectBuffer  = 0x3F;
    constexpr uint32_t OpCondWrite       = 0x45;
    constexpr uint32_t OpEventWrite      = 0x46;
    constexpr uint32_t OpMeInit          = 0x48;
    constexpr uint32_t OpInterrupt       = 0x54;
    constexpr uint32_t OpEventWriteShd   = 0x58;
    constexpr uint32_t OpDrawIndx2       = 0x36;
    constexpr uint32_t OpSetBinMaskLo    = 0x60;
    constexpr uint32_t OpSetBinMaskHi    = 0x61;
    constexpr uint32_t OpSetBinSelectLo  = 0x62;
    constexpr uint32_t OpSetBinSelectHi  = 0x63;
    constexpr uint32_t OpSwap            = 0x64;
    constexpr uint32_t OpNop             = 0x10;

    const char* Type3Name(uint32_t opcode)
    {
        switch (opcode)
        {
        case OpNop:             return "NOP";
        case OpRegRmw:          return "REG_RMW";
        case OpDrawIndx:        return "DRAW_INDX";
        case OpImLoad:          return "IM_LOAD";
        case OpImLoadImmediate: return "IM_LOAD_IMMEDIATE";
        case OpSetConstant:     return "SET_CONSTANT";
        case OpSetConstant2:    return "SET_CONSTANT2";
        case OpSetShaderConstants: return "SET_SHADER_CONSTANTS";
        case OpLoadAluConstant: return "LOAD_ALU_CONSTANT";
        case OpDrawIndx2:       return "DRAW_INDX_2";
        case OpInvalidateState: return "INVALIDATE_STATE";
        case OpWaitRegMem:      return "WAIT_REG_MEM";
        case OpIndirectBuffer:  return "INDIRECT_BUFFER";
        case OpCondWrite:       return "COND_WRITE";
        case OpEventWrite:      return "EVENT_WRITE";
        case OpMeInit:          return "ME_INIT";
        case OpInterrupt:       return "INTERRUPT";
        case OpEventWriteShd:   return "EVENT_WRITE_SHD";
        case OpSetBinMaskLo:    return "SET_BIN_MASK_LO";
        case OpSetBinMaskHi:    return "SET_BIN_MASK_HI";
        case OpSetBinSelectLo:  return "SET_BIN_SELECT_LO";
        case OpSetBinSelectHi:  return "SET_BIN_SELECT_HI";
        case OpSwap:            return "SWAP";
        default:                return nullptr;
        }
    }

    // Predicated packets. A type 3 packet with bit 0 of its header set runs
    // only when the bin select and the bin mask share a bit (the SET_BIN_*
    // packets set them), and a predicated swap never runs; that is Xenia's
    // rule. The title's driver loads a character's depth only vertex program
    // behind such a predicate right after the colour pass's program: running
    // every load left the depth only one current, and the colour pass drew
    // the soldier with no interpolators, black. COD3_NOPREDICATE=1 runs every
    // packet again, to compare.
    std::atomic<uint64_t> g_binMask{ ~0ull }, g_binSelect{ ~0ull };
    std::atomic<uint64_t> g_predicatedOut{ 0 };
    bool BinPass(uint32_t header, uint32_t opcode)
    {
        static const bool off = getenv("COD3_NOPREDICATE") != nullptr;
        if ((header & 1) != 0 && RenderInternal::FrameLogged())
            printf("frame: predicated %s (%02X), select %016llX mask %016llX\n", Type3Name(opcode) ? Type3Name(opcode) : "?", opcode,
                (unsigned long long)g_binSelect.load(), (unsigned long long)g_binMask.load());
        if (off || (header & 1) == 0) return true;
        if ((g_binSelect.load(std::memory_order_relaxed) & g_binMask.load(std::memory_order_relaxed)) != 0 && opcode != OpSwap)
            return true;
        g_predicatedOut.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    void SetBin(uint32_t opcode, uint32_t value)
    {
        std::atomic<uint64_t>& target = (opcode == 0x60 || opcode == 0x61) ? g_binMask : g_binSelect;
        const uint64_t old = target.load(std::memory_order_relaxed);
        if (opcode == 0x60 || opcode == 0x62) target.store((old & 0xFFFFFFFF00000000ull) | value, std::memory_order_relaxed);
        else target.store((old & 0xFFFFFFFFull) | (uint64_t(value) << 32), std::memory_order_relaxed);
    }

    // What the title is actually drawing, accumulated as the stream is decoded.
    const char* PrimitiveName(uint32_t type)
    {
        switch (type)
        {
        case 1:  return "point list";
        case 2:  return "line list";
        case 3:  return "line strip";
        case 4:  return "triangle list";
        case 5:  return "triangle fan";
        case 6:  return "triangle strip";
        case 8:  return "rectangle list";
        case 13: return "quad list";
        default: return "unknown primitive";
        }
    }

    struct DrawInfo
    {
        std::map<uint32_t, uint64_t> primitives;   // type -> count
        uint64_t indices = 0;
        uint64_t vertexShaders = 0;
        uint64_t pixelShaders = 0;
        uint64_t shaderDwords = 0;
        uint32_t lastVertexShader = 0;
        uint32_t lastPixelShader = 0;
    };

    std::mutex g_drawMutex;
    DrawInfo g_draws;

    // DRAW_INDX carries a viz query word first; DRAW_INDX_2 starts straight at
    // the initiator. Both pack the primitive type in the low six bits and the
    // index count in the top sixteen.
    // The first few draw packets are printed raw. A decode that looks odd is
    // either the title doing something odd or the reader being off by a word,
    // and only the raw bytes tell the two apart.
    std::atomic<int> g_drawsDumped{ 0 };

    void DumpDraw(const char* which, uint32_t header, uint32_t w1, uint32_t w2)
    {
        if (g_drawsDumped.fetch_add(1) >= 2) return;
        printf("draw: %s header %08X payload %08X %08X -> count %u, prim %u\n",
            which, header, w1, w2, w1 >> 16, w1 & 0x3F);
        fflush(stdout);
    }

    // Where a draw is going. The registers that name the render target and the
    // resolve destination are not guessed: on the first draw of each primitive
    // type the whole render backend block is printed, and the addresses in it
    // are matched against what VdSwap reports as the front buffer.
    std::mutex g_stateMutex;
    std::map<uint32_t, int> g_statedPrimitives;

    void DumpDrawState(uint32_t primitive)
    {
        {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            if (g_statedPrimitives[primitive]++ >= 1) return;
        }

        const uint32_t mode = Gpu::ReadRegister(Gpu::ApertureBase + 0x2208 * 4) & 7;
        static const char* const modeNames[8] = {
            "ignore", "?", "?", "?", "colour and depth", "depth only", "copy", "?" };

        printf("\nstate at the first %s draw, render backend in %s mode:\n",
            PrimitiveName(primitive), modeNames[mode]);
        printf("  programs: vertex %016llx, pixel %016llx\n",
            (unsigned long long)Render::CurrentProgramHash(false),
            (unsigned long long)Render::CurrentProgramHash(true));

        // The float constant file holds shader inputs. Vertex programs read the
        // first 256 entries and pixel programs the second 256, so a pixel
        // program's c0 is entry 256. A program that only exports a constant
        // colour is reading exactly this.
        for (int half = 0; half < 2; half++)
        {
            printf("  %s c0..c1:", half == 0 ? "vertex" : "pixel ");
            for (uint32_t i = 0; i < 8; i++)
            {
                const uint32_t bits = Gpu::ReadRegister(
                    Gpu::ApertureBase + (0x4000 + half * 1024 + i) * 4);
                float value;
                memcpy(&value, &bits, 4);
                printf(" %.4f", value);
            }
            printf("\n");
        }

        // What the draw reads its vertices from, and the first of them.
        const uint32_t fetch0 = Gpu::ReadRegister(Gpu::ApertureBase + 0x4800 * 4);
        const uint32_t fetch1 = Gpu::ReadRegister(Gpu::ApertureBase + 0x4801 * 4);
        if ((fetch0 & 3) == 3)
        {
            const uint32_t address = Guest::PhysicalAlias(fetch0 & ~3u);
            const uint32_t dwords = (fetch1 >> 2) & 0x3FFFFF;
            printf("  vertices at 0x%08X, %u dwords:", address, dwords);
            for (uint32_t i = 0; i < 12 && i < dwords; i++)
            {
                const uint32_t bits = Guest::Read32(Guest::Base, address + i * 4);
                float value;
                memcpy(&value, &bits, 4);
                printf(" %.3f", value);
            }
            printf("\n");
        }


        // 0x2000 to 0x23FF is the render backend: surface, colour and depth
        // info, mode control, and the copy block that names the resolve
        // destination. Everything non zero is printed, because which of these
        // matters is exactly what has to be worked out.
        // The registers that decide where and how the draw lands. These were
        // found by dumping the whole file at a draw and matching values against
        // things already known: 0x210F to 0x2112 held 520, 520, -312, 312,
        // which is a viewport for a 1040 by 624 target, and 0x2319 held the
        // address VdSwap names as the front buffer.
        static const struct { uint32_t index; const char* name; } interesting[] = {
            { 0x2000, "surface pitch and samples" },
            { 0x2001, "colour buffer in EDRAM" },
            { 0x2002, "depth buffer in EDRAM" },
            { 0x2082, "window scissor" },
            { 0x210F, "viewport x scale" },
            { 0x2110, "viewport x offset" },
            { 0x2111, "viewport y scale" },
            { 0x2112, "viewport y offset" },
            { 0x2180, "shader register counts" },
            { 0x2200, "depth control" },
            { 0x2208, "render backend mode" },
            { 0x2318, "copy control" },
            { 0x2319, "copy destination" },
            { 0x231A, "copy destination pitch and height" },
            { 0x231B, "copy destination format" },
        };
        for (const auto& entry : interesting)
        {
            const uint32_t value = Gpu::ReadRegister(Gpu::ApertureBase + entry.index * 4);
            if (value != 0)
                printf("  %-34s %04X = %08X\n", entry.name, entry.index, value);
        }
        fflush(stdout);
    }

    bool TraceConstants();

    void PrintRecent();

    void DecodeDraw(uint32_t initiator, uint32_t indexBase = 0, uint32_t indexWord = 0)
    {
        const uint32_t primitive = initiator & 0x3F;
        {
            std::lock_guard<std::mutex> lock(g_drawMutex);
            g_draws.primitives[primitive]++;
            g_draws.indices += (initiator >> 16);
        }
        DumpDrawState(primitive);

        {
            static const char* const wantedProgram = getenv("COD3_TRACEDRAWVS");
            // COD3_TRACEDRAWVSALL=1: one short line per draw of that program
            // instead, with the swap it is in and a few constants, for
            // finding the draw whose constants are not the others'.
            static const bool everyDraw = getenv("COD3_TRACEDRAWVSALL") != nullptr;
            if (wantedProgram != nullptr && everyDraw && Kernel::Stats().filesOpened.load(std::memory_order_relaxed) >= 40)
            {
                char name[24];
                snprintf(name, sizeof(name), "%016llx", (unsigned long long)Render::CurrentProgramHash(false));
                if (strncmp(name, wantedProgram, strlen(wantedProgram)) == 0)
                {
                    auto f = [](uint32_t index) { const uint32_t bits = Gpu::ReadRegister(Gpu::ApertureBase + (0x4000 + index) * 4); float v; memcpy(&v, &bits, 4); return v; };
                    printf("drawvs: swap %llu count %u c12 %g %g %g %g c15 %g %g %g %g c84 %g %g %g %g c85 %g %g c255 %g %g %g %g\n",
                        (unsigned long long)Kernel::Stats().swapsReached.load(std::memory_order_relaxed), initiator >> 16,
                        f(48), f(49), f(50), f(51), f(60), f(61), f(62), f(63), f(336), f(337), f(338), f(339), f(340), f(341), f(1020), f(1021), f(1022), f(1023));
                }
            }
        }

        // COD3_TRACEDRAWS=1: every draw in a level, with the initiator, the
        // mode, the programs and the depth control, a few thousand of them.
        if (TraceConstants())
        {
            printf("draw: prim %u count %u source %u mode %u depth %08X mask %08X vs_%016llx ps_%016llx\n",
                primitive, initiator >> 16, (initiator >> 6) & 3, Gpu::ReadRegister(Gpu::ApertureBase + 0x2208 * 4) & 7,
                Gpu::ReadRegister(Gpu::ApertureBase + 0x2200 * 4), Gpu::ReadRegister(Gpu::ApertureBase + 0x2104 * 4),
                (unsigned long long)Render::CurrentProgramHash(false), (unsigned long long)Render::CurrentProgramHash(true));
            // COD3_TRACEDRAWVS=hex: the packets before the first draws of that
            // vertex program, and its constants c80..c95, for finding which
            // packet sets what the program reads.
            static const char* const wantedProgram = getenv("COD3_TRACEDRAWVS");
            static int shown = 0;
            if (wantedProgram != nullptr && shown < 4)
            {
                char name[24];
                snprintf(name, sizeof(name), "%016llx", (unsigned long long)Render::CurrentProgramHash(false));
                if (strncmp(name, wantedProgram, strlen(wantedProgram)) == 0)
                {
                    shown++;
                    printf("draw: c80..95 now:");
                    for (uint32_t i = 80 * 4; i < 96 * 4; i++)
                    {
                        const uint32_t bits = Gpu::ReadRegister(Gpu::ApertureBase + (0x4000 + i) * 4);
                        float f; memcpy(&f, &bits, 4);
                        printf("%s %g", (i % 4) == 0 ? " |" : "", f);
                    }
                    printf("\n");
                    PrintRecent();
                }
            }
        }

        // A resolve is spelled as a rectangle list draw with the render backend
        // in copy mode. It is the only draw in the stream that this runtime can
        // carry out completely, because it moves pixels rather than making them.
        const uint32_t mode = RenderState::Register(0x2208) & 7;
        // COD3_NATIVEDIFF=1 (with COD3_NATIVE=1, which keeps the register
        // file whole): a draw made from its record whose image is not the
        // register file, register by register, the first few.
        static const bool nativeDiff = getenv("COD3_NATIVEDIFF") != nullptr;
        if (nativeDiff && RenderState::UsingRecord())
        {
            static std::atomic<int> told{ 0 };
            static thread_local uint64_t pixel;
            pixel = Render::CurrentProgramHash(true);
            if (told.load() < 80)
                NativeState::DiffImage([](uint32_t reg, uint32_t mine, uint32_t file) {
                    if (told.fetch_add(1) < 80)
                        printf("native: a draw (ps %016llx) has %04X = %08X from its record, %08X in the register file\n",
                            (unsigned long long)pixel, reg, mine, file);
                });
        }
        if (primitive == 8 && mode == 6) Render::Resolve();
        else Render::Draw(initiator, indexBase, indexWord);
    }

    // A fence.
    //
    // EVENT_WRITE_SHD tells the GPU to write a value into memory once it has
    // reached this point in the stream. The graphics driver then spins on that
    // memory to know the work is done. Counting the packet and moving on
    // leaves the driver waiting for a word that never changes, which is what
    // held the title on its loading screen with every worker thread parked.
    //
    // The address is physical and its low two bits are a byte order selector
    // rather than part of the address.
    std::atomic<uint64_t> g_fences{ 0 };
    std::atomic<uint64_t> g_fenceCounter{ 0 };

    // The last few packets, so a wait that is abandoned can be seen in the
    // company it keeps: what was queued just before it is what should have
    // satisfied it.
    struct Recent
    {
        uint32_t header = 0;
        uint32_t words[5] = {};
        bool used = false;   // a zero header is a real packet, not an empty slot
    };
    constexpr int RecentCount = 28;
    Recent g_recent[RecentCount];
    int g_recentAt = 0;

    void Remember(uint32_t header, const uint32_t* words, uint32_t available)
    {
        Recent& slot = g_recent[g_recentAt];
        g_recentAt = (g_recentAt + 1) % RecentCount;
        slot.header = header;
        slot.used = true;
        for (uint32_t i = 0; i < 5; i++)
            slot.words[i] = (words != nullptr && i < available) ? words[i] : 0;
    }

    void PrintRecent()
    {
        printf("  the packets just before it, oldest first:\n");
        for (int i = 0; i < RecentCount; i++)
        {
            const Recent& slot = g_recent[(g_recentAt + i) % RecentCount];
            if (slot.header == 0 && slot.words[0] == 0 && slot.words[1] == 0 && !slot.used) continue;
            const uint32_t type = slot.header >> 30;
            if (type == 3)
            {
                const char* name = Type3Name((slot.header >> 8) & 0x7F);
                if (name) printf("    %-18s", name);
                else printf("    opcode 0x%02X        ", (slot.header >> 8) & 0x7F);
            }
            else if (type == 0)
                printf("    reg 0x%04X x%-4u [%08X]", slot.header & 0x7FFF,
                    ((slot.header >> 16) & 0x3FFF) + 1, slot.header);
            else
                printf("    type %u [%08X]     ", type, slot.header);
            for (uint32_t w = 0; w < 5; w++) printf(" %08X", slot.words[w]);
            printf("\n");
        }
        fflush(stdout);
    }

    // Where conditional writes would go: how many were asked for, and how many
    // the condition allowed.
    std::mutex g_condTargetMutex;
    std::map<uint32_t, std::pair<uint64_t, uint64_t>> g_condTargets;

    // Where fences have gone, and how many to each.
    std::mutex g_fenceTargetMutex;
    std::map<uint32_t, uint64_t> g_fenceTargets;

    // Which path a register write is coming by, for the report below.
    thread_local const char* t_writeSource = "unknown";

    // For the native state's check (native_state.cpp): the packet being
    // run (its header's physical address, 0 for none), and for each
    // register the packet that wrote it last.
    thread_local uint32_t t_packetAt = 0;
    std::atomic<uint32_t> g_registerWriter[0x5000];

    // The display's colour table (the gamma ramp D3D sets), which the
    // console's display controller puts the picture through on its way
    // out. The title writes all 256 entries at start and again as a level
    // loads: 0x1922 DC_LUT_RW_INDEX then 0x1925 DC_LUT_30_COLOR, ten bits
    // a channel, blue low and red high, with 0x1927 DC_LUT_WRITE_EN_MASK
    // naming the channels a write changes (4 red, 2 green, 1 blue). Its
    // curve darkens the lower half (32 of 255 comes out as 66 of 1023),
    // and without it the picture was brighter and flatter than the
    // console's: the sky too pale for the fog to meet it at the horizon.
    std::mutex g_lutMutex;
    uint32_t g_lut[256];
    uint32_t g_lutIndex = 0, g_lutMask = 7;
    uint64_t g_lutVersion = 0;
    bool g_lutWritten = false;

    void DisplayLutWrite(uint32_t index, uint32_t value)
    {
        // COD3_TRACELUT=N: the first N writes to these registers.
        static const int traceLut = []() { const char* t = getenv("COD3_TRACELUT"); return t ? int(strtol(t, nullptr, 10)) : 0; }();
        static std::atomic<int> traced{ 0 };
        if (traceLut > 0 && traced.fetch_add(1) < traceLut)
            printf("gpu: lut register %04X = %08X (%s)\n", index, value, t_writeSource ? t_writeSource : "a register write");
        std::lock_guard<std::mutex> lock(g_lutMutex);
        switch (index)
        {
        case 0x1922: g_lutIndex = value & 0xFF; break;
        case 0x1927: g_lutMask = value & 7; break;
        case 0x1925:
        {
            if (!g_lutWritten)
            {
                // Until the title writes one, the table is the identity.
                for (uint32_t i = 0; i < 256; i++) { const uint32_t v = (i << 2) | (i >> 6); g_lut[i] = (v << 20) | (v << 10) | v; }
                g_lutWritten = true;
            }
            uint32_t entry = g_lut[g_lutIndex];
            if (g_lutMask & 1) entry = (entry & ~0x3FFu) | (value & 0x3FFu);
            if (g_lutMask & 2) entry = (entry & ~(0x3FFu << 10)) | (value & (0x3FFu << 10));
            if (g_lutMask & 4) entry = (entry & ~(0x3FFu << 20)) | (value & (0x3FFu << 20));
            g_lut[g_lutIndex] = entry;
            g_lutIndex = (g_lutIndex + 1) & 0xFF;
            g_lutVersion++;
            break;
        }
        case 0x1923: case 0x1924:
        {
            // The sequential and the piecewise linear ways of writing it,
            // which this title does not use: said once if one ever comes.
            static bool said = false;
            if (!said) { said = true; printf("gpu: the display colour table was written through %04X, which is not applied\n", index); }
            break;
        }
        default: break;
        }
    }

    // Interrupts the command stream has asked for and nobody has raised yet.
    std::atomic<uint32_t> g_pendingInterrupts{ 0 };

    // Raising one where it stands in the stream. The handler is guest code
    // and needs the command thread's own context, which lives on the video
    // side; it installs this so the packet can be acted on the moment it is
    // decoded rather than when the buffer ends. That order matters: the
    // driver puts a wait immediately after the interrupt for a value the
    // handler writes, and a handler that runs after the wait has given up is
    // a handler that ran for nothing.
    void (*g_raiseInterrupt)(uint32_t cpuMask) = nullptr;

    void RaiseInterruptNow(uint32_t cpuMask)
    {
        if (g_raiseInterrupt != nullptr) g_raiseInterrupt(cpuMask);
        else g_pendingInterrupts.fetch_add(1, std::memory_order_relaxed);
    }

    // The swap packet VdSwap writes into the driver's reservation: the
    // picture changes here, when the GPU reaches it, so a frame is shown
    // once its draws are done and never before.
    void SwapPacket(uint32_t magic, uint32_t surface, uint32_t width, uint32_t height)
    {
        if (magic != 0x50415753u) return;
        if (surface == 0 || width <= 1 || height <= 1 || width > 4096 || height > 4096) return;
        {
            // The size, whenever it changes: a picture that jumps between
            // two sizes is a picture that jumps on the screen.
            static uint32_t lastWidth = 0, lastHeight = 0;
            static int announced = 0;
            if ((width != lastWidth || height != lastHeight) && announced++ < 40)
            {
                printf("video: swap of %08X at %ux%u\n", surface, width, height);
                fflush(stdout);
            }
            lastWidth = width; lastHeight = height;
        }
        {
            // How far the title's frames run ahead of the GPU's: the swaps
            // VdSwap has been called for less the swaps reached here. Dynamic
            // data the title double buffers is overwritten when this passes
            // one, and what the GPU draws then is half of the next frame.
            static uint64_t reached = 0;
            static int64_t lastAhead = -1;
            static int announced = 0;
            reached++;
            const int64_t ahead = int64_t(Kernel::Stats().swaps.load(std::memory_order_relaxed)) - int64_t(reached);
            if (ahead != lastAhead && announced++ < 60)
            {
                printf("video: the title is %lld swaps ahead of the GPU at swap %llu\n", (long long)ahead, (unsigned long long)reached);
                fflush(stdout);
            }
            lastAhead = ahead;
            Kernel::Stats().swapsReached.store(reached, std::memory_order_release);
        }
        Window::SetFrontBuffer(Guest::PhysicalAlias(surface), width, height);
        Render::Swap(surface, width, height);
    }

    void WriteFence(uint32_t initiator, uint32_t address, uint32_t value)
    {
        const uint32_t target = address & ~3u;
        if (target == 0) return;

        // With the top bit set the GPU writes its own progress counter rather
        // than the value the packet carries.
        const uint32_t written = ((initiator >> 31) & 1)
            ? uint32_t(g_fenceCounter.fetch_add(1, std::memory_order_relaxed))
            : value;

        Guest::WritePhysical32(target, written);
        g_fences.fetch_add(1, std::memory_order_relaxed);
        Timeline::Mark("fence", target, written);

        // Every destination, counted. The line below only ever printed the
        // first twelve of a run, which is enough to see where fences start
        // going and no use at all for seeing where they go later.
        {
            std::lock_guard<std::mutex> guard(g_fenceTargetMutex);
            g_fenceTargets[target]++;
        }

        static std::atomic<int> announced{ 0 };
        if (announced.fetch_add(1) < 12)
        {
            printf("gpu: fence write of 0x%08X to physical 0x%08X\n", written, target);
            fflush(stdout);
        }

        // COD3_TRACEFENCE=1: every fence, with the ring position, for the
        // title's own waits on them.
        static const bool traceFences = []() {
            const char* text = getenv("COD3_TRACEFENCE");
            return text != nullptr && text[0] != 0 && text[0] != '0';
        }();
        if (traceFences)
        {
            printf("gpu: fence 0x%08X -> 0x%08X at ring %u\n", written, target, Gpu::WritePointer());
        }
    }

    // WAIT_REG_MEM holds the command processor until a value satisfies a
    // condition. It is how the title keeps one piece of work behind another,
    // and skipping it lets everything after it run early: fences are written
    // before the work they stand for has happened, and the driver is told its
    // work is done when it is not.
    //
    // Waiting here is bounded, and tightly. Everything earlier in the stream
    // has already happened by the time this runs, so a wait on the GPU's own
    // progress is satisfied at once; the ones that are not are waiting on the
    // guest, and holding the whole stream up for those starves the title of
    // frames. Two milliseconds was enough to do exactly that.
    std::atomic<uint64_t> g_waits{ 0 };
    std::atomic<uint64_t> g_waitTimeouts{ 0 };
    std::atomic<uint64_t> g_waitNanoseconds{ 0 };

    bool ConditionHolds(uint32_t function, uint32_t left, uint32_t reference)
    {
        switch (function)
        {
        case 0: return false;
        case 1: return left < reference;
        case 2: return left <= reference;
        case 3: return left == reference;
        case 4: return left != reference;
        case 5: return left >= reference;
        case 6: return left > reference;
        default: return true;
        }
    }

    void WaitForValue(uint32_t control, uint32_t pollAddress, uint32_t reference,
                      uint32_t mask)
    {
        const uint32_t function = control & 0x7;
        const bool memory = ((control >> 4) & 1) != 0;

        g_waits.fetch_add(1, std::memory_order_relaxed);

        // The wait after a swap: on the flag in the second word of the
        // scratch block the driver named (register 0x1DD), which the
        // title's vertical blank handler clears a blank after the swap.
        // That is the frame rate's ceiling, sixty a second; with the frame
        // rate unlocked while a level is played it is let through at once,
        // and the next frame starts as soon as the title has it.
        if (memory && Kernel::FrameRateUnlocked())
        {
            const uint32_t block = Gpu::ReadRegister(Gpu::ApertureBase + 0x1DD * 4) & ~3u;
            if (block != 0 && (pollAddress & ~3u) == block + 4) return;
        }

        // How long the command processor will wait for a value the title is
        // supposed to write. The console waits as long as it takes; giving up
        // early lets the next packet run against state that is not ready yet,
        // which is a corruption rather than a stall. The bound is here because
        // a wait that never ends would hang the whole run, and it can be moved
        // from the environment so the right value can be measured rather than
        // guessed.
        static const long limit = []() -> long {
            const char* text = getenv("COD3_GPUWAIT");
            // Five seconds: the console waits as long as it takes, and a wait
            // given up is the next packet run against a buffer the CPU is
            // still writing, which ended in a corrupt stream and a hang.
            const long value = (text != nullptr) ? strtol(text, nullptr, 10) : 5000000;
            return (value > 0) ? value : 5000000;
        }();

        const auto started = std::chrono::steady_clock::now();
        const auto deadline = started + std::chrono::microseconds(limit);
        Timeline::Mark("cp wait", pollAddress, reference);
        struct Marked { uint32_t address; ~Marked() { Timeline::Mark("cp waited", address); } } marked{ pollAddress };
        struct Accounted { std::chrono::steady_clock::time_point since; ~Accounted() {
            g_waitNanoseconds.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - since).count()), std::memory_order_relaxed); } } accounted{ started };
        for (;;)
        {
            const uint32_t current = memory
                ? Guest::Read32(Guest::Base, Guest::PhysicalAlias(pollAddress & ~3u))
                : Gpu::ReadRegister(Gpu::ApertureBase + (pollAddress & 0xFFFF) * 4);

            if (ConditionHolds(function, current & mask, reference)) return;
            if (std::chrono::steady_clock::now() > deadline)
            {
                g_waitTimeouts.fetch_add(1, std::memory_order_relaxed);
                Timeline::Mark("cp gave up", pollAddress, current);

                // What it was waiting for, once per distinct address. A wait
                // the command processor abandons is the title's own driver
                // being told the work is done when it is not, and that is what
                // it eventually reports as a hung GPU. Naming the address and
                // the value says which fence never arrived.
                {
                    static std::mutex mutex;
                    static std::map<uint32_t, bool> seen;
                    bool first = false;
                    {
                        std::lock_guard<std::mutex> guard(mutex);
                        // Keyed by address and value together, so a wait on
                        // the same fence for the next number is still shown.
                        const uint32_t key = pollAddress ^ (reference << 20);
                        if (seen.size() < 16 && seen.find(key) == seen.end())
                        {
                            seen[key] = true;
                            first = true;
                        }
                    }
                    if (first)
                    {
                        printf("gpu: gave up waiting on %s 0x%08X: it holds "
                               "0x%08X, the wait wants 0x%08X under mask "
                               "0x%08X, test %u\n",
                            memory ? "memory" : "register", pollAddress,
                            current, reference, mask, function);
                        PrintRecent();
                        // The moments before it, if they are being kept.
                        Timeline::Report();
                    }
                }
                return;
            }
            std::this_thread::yield();
        }
    }

    // LOAD_ALU_CONSTANT copies registers from memory: the address of the
    // values, then which block they belong in and where in it they start,
    // then how many. The films set their pixel shader's colour matrix this
    // way, where everything else in the title writes constants inline.
    // Where a constant packet's offset and type word points: the ALU
    // constants, the fetch constants, the booleans, the loops, or the
    // registers themselves. The booleans and loops sit above the fetch
    // constants at 0x4900 and 0x4908, where the programs read them.
    bool ConstantIndex(uint32_t offsetAndType, uint32_t& index)
    {
        index = offsetAndType & 0x7FF;
        switch ((offsetAndType >> 16) & 0xFF)
        {
        case 0:  index += 0x4000; return true;   // ALU constants
        case 1:  index += 0x4800; return true;   // fetch constants
        case 2:  index += 0x4900; return true;   // boolean constants
        case 3:  index += 0x4908; return true;   // loop constants
        case 4:  index += 0x2000; return true;   // registers
        default: return false;
        }
    }

    // COD3_TRACECONST=1: every constant packet twenty seconds into a level,
    // a few thousand of them, with where it lands and its first words.
    bool TraceConstants()
    {
        static const bool wanted = []() {
            const char* text = getenv("COD3_TRACECONST");
            return text != nullptr && text[0] != 0 && text[0] != '0';
        }();
        if (!wanted) return false;
        static int64_t levelSince = 0;
        const int64_t now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        if (levelSince == 0 && Kernel::Stats().filesOpened.load(std::memory_order_relaxed) >= 40) levelSince = now;
        if (levelSince == 0 || now - levelSince < 14) return false;
        static std::atomic<int> lines{ 0 };
        return lines.fetch_add(1) < 30000;
    }

    void LoadConstantsFromMemory(uint32_t address, uint32_t offsetAndType, uint32_t count)
    {
        uint32_t index;
        if (!ConstantIndex(offsetAndType, index)) return;
        count &= 0xFFF;
        if (TraceConstants())
        {
            const uint32_t source = Guest::PhysicalAlias(address & ~3u);
            printf("const: load %08X -> %04X x%u:", offsetAndType, index, count);
            for (uint32_t i = 0; i < count && i < 8; i++) { const uint32_t v = Guest::Read32(Guest::Base, source + i * 4); float f; memcpy(&f, &v, 4); printf(" %g", f); }
            printf("\n");
        }
        const uint32_t source = Guest::PhysicalAlias(address & ~3u);
        t_writeSource = "a LOAD_ALU_CONSTANT packet";
        for (uint32_t i = 0; i < count; i++)
            Gpu::WriteRegister(Gpu::ApertureBase + (index + i) * 4, Guest::Read32(Guest::Base, source + i * 4));
    }

    // COND_WRITE polls a register or memory until a condition holds and then
    // writes a value. The command processor here runs a buffer to completion
    // before the guest looks at anything it produced, so the condition is
    // evaluated once and the write happens if it holds.
    void ConditionalWrite(uint32_t control, uint32_t pollAddress, uint32_t reference,
                          uint32_t mask, uint32_t writeAddress, uint32_t writeValue)
    {
        const uint32_t function = control & 0x7;
        const bool memory = ((control >> 4) & 1) != 0;

        // Where the write goes is a separate question from where the value
        // being tested comes from, and it has its own bit. Every one of these
        // in this title writes to a register, and every one of them was being
        // written to guest memory instead: two thousand writes a run to
        // physical address 0x1920, which is the first few kilobytes of the
        // address space and belongs to something else entirely.
        const bool writeToMemory = ((control >> 8) & 1) != 0;

        const uint32_t current = memory
            ? Guest::Read32(Guest::Base, Guest::PhysicalAlias(pollAddress & ~3u))
            : Gpu::ReadRegister(Gpu::ApertureBase + (pollAddress & 0xFFFF) * 4);

        const uint32_t left = current & mask;
        bool holds = false;
        switch (function)
        {
        case 0: holds = false; break;
        case 1: holds = left < reference; break;
        case 2: holds = left <= reference; break;
        case 3: holds = left == reference; break;
        case 4: holds = left != reference; break;
        case 5: holds = left >= reference; break;
        case 6: holds = left > reference; break;
        default: holds = true; break;
        }

        // Where a conditional write would go, whether or not the condition let
        // it. A write that never happens is invisible otherwise, and a
        // condition that is never true is exactly the shape of a missing piece.
        {
            std::lock_guard<std::mutex> guard(g_condTargetMutex);
            auto& entry = g_condTargets[
                writeToMemory ? (writeAddress & ~3u) : (writeAddress & 0xFFFF)];
            entry.first++;
            if (holds) entry.second++;
        }

        if (!holds) return;

        if (writeToMemory)
        {
            if ((writeAddress & ~3u) != 0)
                Guest::WritePhysical32(writeAddress & ~3u, writeValue);
        }
        else
        {
            t_writeSource = "a conditional write";
            Gpu::WriteRegister(
                Gpu::ApertureBase + (writeAddress & 0xFFFF) * 4, writeValue);
        }
    }

    // COD3_TRACESHADERLOAD=N: the first N shader uploads once a level is
    // loading, with where in the instruction memory they go, and the writes
    // of SQ_PROGRAM_CNTL and of 0x21F6 and 0x21F7, which name the programs'
    // bases on the console's relatives. This title never writes those two
    // and loads every program at 0: the program last loaded is the one that
    // runs, once the predicated loads are left out.
    int ShaderLoadTraceLimit()
    {
        static const int limit = []() { const char* t = getenv("COD3_TRACESHADERLOAD"); return t ? int(strtol(t, nullptr, 10)) : 0; }();
        return limit;
    }
    std::atomic<int> g_shaderLoadsTraced{ 0 };
    void TraceShaderLoad(bool pixel, uint32_t startSize, uint32_t sizeDwords)
    {
        if (ShaderLoadTraceLimit() <= 0 || Kernel::Stats().filesOpened.load(std::memory_order_relaxed) < 40) return;
        if (g_shaderLoadsTraced.fetch_add(1) >= ShaderLoadTraceLimit()) return;
        printf("gpu: load %s start %u size %u hash %016llx\n", pixel ? "ps" : "vs", startSize >> 16, sizeDwords,
            (unsigned long long)Render::CurrentProgramHash(pixel));
    }

    // A shader upload. The first word says which pipeline stage, the second
    // packs where it goes and how long it is; the microcode follows inline.
    void DecodeShaderLoad(uint32_t typeWord, uint32_t startSize, uint32_t address)
    {
        const uint32_t sizeDwords = startSize & 0xFFFF;
        const bool pixel = (typeWord & 1) != 0;
        {
            std::lock_guard<std::mutex> lock(g_drawMutex);
            g_draws.shaderDwords += sizeDwords;
            if (pixel)
            {
                g_draws.pixelShaders++;
                g_draws.lastPixelShader = address;
            }
            else
            {
                g_draws.vertexShaders++;
                g_draws.lastVertexShader = address;
            }
        }
        Render::ShaderLoaded(pixel, address, sizeDwords, startSize >> 16);
        TraceShaderLoad(pixel, startSize, sizeDwords);
    }

    // IM_LOAD is the same upload by reference: the packet names where the
    // microcode already is instead of carrying it. The address is physical.
    void DecodeShaderLoadIndirect(uint32_t addressWord, uint32_t startSize)
    {
        DecodeShaderLoad(addressWord & 3,
                         startSize,
                         Guest::PhysicalAlias(addressWord & ~3u));
    }
}

// --- The aperture -----------------------------------------------------------

void Mmio::Store32(uint32_t address, uint32_t value)
{
    ReportOnce("write", address, value);
    t_writeSource = "the CPU through the aperture";

    // Through the same door the command stream uses. This used to drop the
    // value straight into the register map, which is fine for most registers
    // and wrong for the scratch ones: those are mirrored into memory on every
    // write, and the driver clears one from the CPU after each swap and then
    // waits, in the stream, for the clear to show up in memory. Written here
    // directly it never did, and the wait was abandoned every frame.
    Gpu::WriteRegister(address, value);
}

uint32_t Mmio::Load32(uint32_t address)
{
    // Guest code that spins on a register is spinning, not blocking, so this is
    // where it gets the chance to hand its hardware thread over.
    Scheduler::Checkpoint();
    ReportOnce("read", address, 0xFFFFFFFF);
    {
        std::lock_guard<std::mutex> lock(g_readCountMutex);
        g_readCounts[address]++;
    }
    uint32_t index;
    return RegisterIndex(address, index) ? g_registerFile[index].load(std::memory_order_relaxed) : 0;
}

void Gpu::SnapshotRegisters(uint32_t firstIndex, uint32_t count, uint32_t* out)
{
    // A copy of the range, word by word: the rasteriser reads registers for
    // every pixel of every band on every core, and the Direct3D backend for
    // every draw.
    for (uint32_t i = 0; i < count; i++)
        out[i] = firstIndex + i < RegisterCount ? g_registerFile[firstIndex + i].load(std::memory_order_relaxed) : 0;
}

uint64_t Gpu::BinSelect() { return g_binSelect.load(std::memory_order_relaxed); }

uint32_t Gpu::ReadRegister(uint32_t address)
{
    uint32_t index;
    return RegisterIndex(address, index) ? g_registerFile[index].load(std::memory_order_relaxed) : 0;
}

const std::atomic<uint32_t>* Gpu::RegisterFile() { return g_registerFile; }

uint32_t Gpu::RegisterWriter(uint32_t index)
{
    return index < 0x5000 ? g_registerWriter[index].load(std::memory_order_relaxed) : 0;
}

bool Gpu::DisplayGamma(uint32_t table[256], uint64_t& version)
{
    std::lock_guard<std::mutex> lock(g_lutMutex);
    version = g_lutVersion;
    if (!g_lutWritten) return false;
    memcpy(table, g_lut, sizeof(g_lut));
    return true;
}

uint64_t Gpu::ConstantWrites(uint32_t range) { return range < 3 ? g_constantWrites[range].load(std::memory_order_relaxed) : 0; }

bool Gpu::TakeConstantSpan(uint32_t range, uint32_t& first, uint32_t& end)
{
    if (range >= 3) return false;
    first = g_constantDirtyLow[range].exchange(0xFFFFFFFFu, std::memory_order_relaxed);
    end = g_constantDirtyHigh[range].exchange(0, std::memory_order_relaxed);
    return first < end;
}

namespace
{
    void (*g_submitHook)() = nullptr;
}

void Gpu::SetSubmitHook(void (*hook)()) { g_submitHook = hook; }

namespace
{
    std::map<uint32_t, uint64_t> g_registerWrites;   // under g_registerMutex
}

void Gpu::WriteRegister(uint32_t address, uint32_t value)
{
    uint32_t mirrorTo = 0;
    if (address == RegisterWritePointer && g_submitHook != nullptr) g_submitHook();
    {
        uint32_t fileIndex;
        if (!RegisterIndex(address, fileIndex)) return;
        if (fileIndex >= 0x1920 && fileIndex < 0x1930) DisplayLutWrite(fileIndex, value);
        // COD3_VSCONST=N:x,y,z,w overrides vertex constant N with the given
        // values whenever it is written: an experiment's knob, for telling
        // whether a value the title writes is the one its program expects.
        {
            static const struct Override { int index; uint32_t words[4]; bool on; } override = []() {
                Override o{ -1, { 0, 0, 0, 0 }, false };
                const char* text = getenv("COD3_VSCONST");
                if (text == nullptr) return o;
                float f[4] = { 0, 0, 0, 0 };
                if (sscanf(text, "%d:%f,%f,%f,%f", &o.index, &f[0], &f[1], &f[2], &f[3]) >= 2)
                {
                    for (int i = 0; i < 4; i++) memcpy(&o.words[i], &f[i], 4);
                    o.on = true;
                }
                return o;
            }();
            if (override.on && fileIndex >= 0x4000 + uint32_t(override.index) * 4 && fileIndex < 0x4000 + uint32_t(override.index) * 4 + 4)
                value = override.words[fileIndex - (0x4000 + uint32_t(override.index) * 4)];
        }
        g_registerFile[fileIndex].store(value, std::memory_order_relaxed);
        if (fileIndex < 0x5000 && NativeState::Checking())
            g_registerWriter[fileIndex].store(t_writeSource == nullptr || strstr(t_writeSource, "packet") == nullptr ? 0 : t_packetAt, std::memory_order_relaxed);
        if (fileIndex >= 0x4000 && fileIndex < 0x4400) NoteConstantWrite(0, fileIndex - 0x4000);
        else if (fileIndex >= 0x4400 && fileIndex < 0x4800) NoteConstantWrite(1, fileIndex - 0x4400);
        else if (fileIndex >= 0x4900 && fileIndex < 0x4930) NoteConstantWrite(2, fileIndex - 0x4900);

        if (ShaderLoadTraceLimit() > 0 && (fileIndex == 0x21F6 || fileIndex == 0x21F7 || fileIndex == 0x2180)
            && Kernel::Stats().filesOpened.load(std::memory_order_relaxed) >= 40
            && g_shaderLoadsTraced.load(std::memory_order_relaxed) < ShaderLoadTraceLimit())
            printf("gpu: register %04X = %08X\n", fileIndex, value);

        // COD3_TRACEREG=1: every register written once a level is loading,
        // and how often, for the ones this runtime does not act on.
        static const bool traceRegisters = []() {
            const char* text = getenv("COD3_TRACEREG");
            return text != nullptr && text[0] != 0 && text[0] != '0';
        }();
        if (traceRegisters && Kernel::Stats().filesOpened.load(std::memory_order_relaxed) >= 40)
        {
            std::lock_guard<std::mutex> lock(g_registerMutex);
            const uint32_t index = (address - ApertureBase) / 4;
            if (index < 0x2400 && (index < 0x4000 || index >= 0x4800))
                g_registerWrites[index]++;
            if (index == 0x2307 || index == 0x2308 || index == 0x2205 || index == 0x2200 || index == 0x2002)
            {
                static std::map<uint64_t, bool> seen;
                if (seen.size() < 60 && seen.emplace((uint64_t(index) << 32) | value, true).second)
                    printf("gpu: register %04X written with %08X in the level\n", index, value);
            }
        }

        // The scratch registers are mirrored into memory.
        //
        // Eight registers at 0x578 are the command processor's scratch space,
        // and the hardware copies each one into memory whenever it changes:
        // SCRATCH_ADDR says where the block starts and SCRATCH_UMSK which of
        // the eight are copied. The driver writes them from the stream and
        // then waits, in the stream, for the copy to show up in memory.
        //
        // This is the write nothing else was making. After every swap the
        // driver waited at 0x1A780000 for a four and at 0x1A780004 for a one,
        // no packet targeted either, and the wait was abandoned; with the
        // fence writes just before it going to 0x1A770000, the block sixty
        // four kilobytes on had no obvious author. It is SCRATCH_REG0 and
        // SCRATCH_REG1, mirrored to where SCRATCH_ADDR points.
        const uint32_t index = (address - ApertureBase) / 4;

        // Where the scratch block is and which registers are mirrored, every
        // time either changes: a block that moves under the driver's feet is
        // a handler reading the old one.
        if (index == 0x1DC || index == 0x1DD)
        {
            printf("gpu: scratch %s = 0x%08X\n",
                index == 0x1DD ? "block address" : "mirror mask", value);

            // A block address that is not one: the scratch block lives in a
            // sixty four kilobyte allocation below the 512 MB of physical
            // memory, so anything else here is packet data being taken for a
            // register write. What came just before says which packet was
            // sized wrong.
            if (index == 0x1DD && value != 0 &&
                ((value & 0xFFFF) != 0 || value >= 0x20000000u))
            {
                printf("gpu: that is not a block address; it came by %s\n", t_writeSource);
                PrintRecent();
            }
            fflush(stdout);
        }

        if (index >= 0x578 && index < 0x580)
        {
            const uint32_t which = index - 0x578;
            const uint32_t mask = g_registerFile[0x1DC].load(std::memory_order_relaxed);
            const uint32_t block = g_registerFile[0x1DD].load(std::memory_order_relaxed);
            if (((mask >> which) & 1) != 0 && block != 0)
                mirrorTo = (block & ~3u) + which * 4;
        }
    }

    if (mirrorTo != 0)
    {
        Guest::WritePhysical32(mirrorTo, value);
        Timeline::Mark("scratch", mirrorTo, value);

        static std::atomic<int> announced{ 0 };
        if (announced.fetch_add(1) < 40)
        {
            printf("gpu: scratch register %u = 0x%08X, mirrored to physical "
                   "0x%08X\n", (address - ApertureBase) / 4 - 0x578, value, mirrorTo);
            fflush(stdout);
        }
    }
}

uint32_t Gpu::WritePointer() { return ReadRegister(RegisterWritePointer); }

Gpu::Statistics Gpu::Stats()
{
    std::lock_guard<std::mutex> lock(g_statisticsMutex);
    return g_statistics;
}

void Gpu::ReportPolling()
{
    std::lock_guard<std::mutex> lock(g_readCountMutex);
    uint32_t busiest = 0;
    uint64_t most = 0;
    for (const auto& [address, count] : g_readCounts)
        if (count > most) { most = count; busiest = address; }

    if (most < 1000) return;   // not a spin

    static uint64_t reportedAt = 0;
    if (most - reportedAt < 100000) return;
    reportedAt = most;

    printf("gpu: the driver has read register 0x%08X %llu times, it is polling "
           "for a value\n", busiest, (unsigned long long)most);
    fflush(stdout);
}

// --- The command processor --------------------------------------------------

namespace
{
    // Executes a run of packets that is not the ring: an indirect buffer names
    // an address and a length, and its contents are ordinary packets.
    void ExecuteBuffer(uint32_t base, uint32_t dwords, Gpu::Statistics& local, int depth);

    // The indirect buffer packet most recently followed, raw, for reports.
    thread_local uint32_t t_lastIndirectAddress = 0;
}
uint32_t Gpu::CurrentIndirectBuffer() { return t_lastIndirectAddress; }
namespace
{
    thread_local uint32_t t_lastIndirectSize = 0;
    thread_local uint32_t t_lastIndirectSizeAt = 0;   // guest address of the size word

    // The whole walk shares one deadline. Almost every draw in this title's
    // stream is inside an indirect buffer, so a budget that only covered the
    // ring's own packets was a budget on nothing: one buffer would run for
    // seconds and the vertical blank would not happen until it finished.
    thread_local std::chrono::steady_clock::time_point g_deadline{};
    thread_local bool g_haveDeadline = false;

    bool OutOfTime()
    {
        return g_haveDeadline && std::chrono::steady_clock::now() > g_deadline;
    }
}

uint32_t Gpu::ProcessRing(uint32_t ringBase, uint32_t ringSizeDwords,
                          uint32_t readPointer, uint32_t writePointer,
                          uint32_t budgetMicroseconds)
{
    const auto started = std::chrono::steady_clock::now();
    g_haveDeadline = budgetMicroseconds != 0;
    if (g_haveDeadline)
        g_deadline = started + std::chrono::microseconds(budgetMicroseconds);
    if (ringSizeDwords == 0 || writePointer == readPointer) return readPointer;
    if (writePointer >= ringSizeDwords) return readPointer;

    Statistics local{};
    uint32_t cursor = readPointer;
    int guard = 0;

    auto ReadDword = [&](uint32_t index) {
        return Guest::Read32(Guest::Base, ringBase + (index % ringSizeDwords) * 4);
    };

    while (cursor != writePointer)
    {
        // A walk that does not land exactly on the write pointer has lost
        // sync, and continuing would read the rest of the ring as commands. A
        // few dozen packets past the write pointer is already wrong.
        if (++guard > 4096)
        {
            static bool reported = false;
            if (!reported)
            {
                reported = true;
                printf("gpu: lost sync walking the ring, stopping at dword %u of %u\n",
                    cursor, ringSizeDwords);
                fflush(stdout);
            }
            return writePointer;
        }

        // Stopping part way is what the hardware looks like from the driver's
        // side anyway: the read pointer simply has not got there yet.
        if ((guard & 0xF) == 0 && OutOfTime()) break;

        const uint32_t header = ReadDword(cursor);
        t_packetAt = ringBase + cursor * 4;
        const uint32_t type = header >> 30;
        uint32_t length = 1;

        if (type == 0)
        {
            const uint32_t count = ((header >> 16) & 0x3FFF) + 1;
            const uint32_t baseRegister = (header & 0x7FFF);
            const bool oneRegister = ((header >> 15) & 1) != 0;

            // The same watch as inside the buffers: a write that lands on the
            // command processor's own registers from the ring, with the ring
            // around it.
            if (baseRegister >= 0x1C0 && baseRegister + count > 0x1DC && baseRegister < 0x200)
            {
                static std::atomic<int> announced{ 0 };
                if (announced.fetch_add(1) < 8)
                {
                    const uint32_t from = cursor > 24 ? cursor - 24 : 0;
                    printf("gpu: ring register write of %u from 0x%04X, header 0x%08X, "
                           "at ring dword %u, write pointer %u\n",
                        count, baseRegister, header, cursor, writePointer);
                    printf("  the ring from dword %u:", from);
                    for (uint32_t i = from; i < cursor + 8; i++)
                    {
                        if (((i - from) % 8) == 0) printf("\n    %4u:", i);
                        printf(" %08X", ReadDword(i));
                    }
                    printf("\n");
                    PrintRecent();
                }
            }

            // Padding, as in the buffers below: a write from register zero
            // is skipped whole.
            if (baseRegister == 0)
            {
                local.filler++;
                length = 1 + count;
            }
            else
            {
            t_writeSource = "a type 0 packet in the ring";
            for (uint32_t i = 0; i < count; i++)
            {
                const uint32_t value = ReadDword(cursor + 1 + i);
                const uint32_t target = oneRegister ? baseRegister : baseRegister + i;
                WriteRegister(ApertureBase + target * 4, value);
            }
            local.registerWrites += count;
            length = 1 + count;
            }
        }
        else if (type == 2)
        {
            length = 1;   // filler to the end of a ring wrap
        }
        else if (type == 3)
        {
            const uint32_t opcode = (header >> 8) & 0x7F;
            const uint32_t count = ((header >> 16) & 0x3FFF) + 1;
            length = 1 + count;

            {
                uint32_t words[5];
                const uint32_t available = count < 5 ? count : 5;
                for (uint32_t i = 0; i < available; i++)
                    words[i] = ReadDword(cursor + 1 + i);
                Remember(header, words, available);
            }

            const bool runs = BinPass(header, opcode);
            if (runs && opcode >= OpSetBinMaskLo && opcode <= OpSetBinSelectHi)
                SetBin(opcode, ReadDword(cursor + 1));
            if (runs) switch (opcode)
            {
            case OpDrawIndx:
                local.draws++;
                if (NativeState::Checking())
                    NativeState::Check(ringBase + ((cursor + count) % ringSizeDwords) * 4, ReadDword(cursor + 2), ReadDword(cursor + 3));
                if (TraceConstants()) printf("const: draw indexed vs_%016llx\n", (unsigned long long)Render::CurrentProgramHash(false));
                {
                    const bool native = NativeState::Begin(ringBase + ((cursor + count) % ringSizeDwords) * 4);
                    DecodeDraw(ReadDword(cursor + 2), ReadDword(cursor + 3),
                               ReadDword(cursor + 4));
                    if (native) NativeState::End();
                }
                break;
            case OpDrawIndx2:
                local.draws++;
                if (NativeState::Checking())
                    NativeState::Check(ringBase + ((cursor + count) % ringSizeDwords) * 4, ReadDword(cursor + 1), 0);
                if (TraceConstants()) printf("const: draw vs_%016llx\n", (unsigned long long)Render::CurrentProgramHash(false));
                {
                    const bool native = NativeState::Begin(ringBase + ((cursor + count) % ringSizeDwords) * 4);
                    DecodeDraw(ReadDword(cursor + 1));
                    if (native) NativeState::End();
                }
                break;
            case OpImLoadImmediate:
                DecodeShaderLoad(ReadDword(cursor + 1), ReadDword(cursor + 2),
                                 ringBase + ((cursor + 3) % ringSizeDwords) * 4);
                break;
            case OpImLoad:
                DecodeShaderLoadIndirect(ReadDword(cursor + 1), ReadDword(cursor + 2));
                break;
            case OpLoadAluConstant:
                LoadConstantsFromMemory(ReadDword(cursor + 1), ReadDword(cursor + 2),
                                        ReadDword(cursor + 3));
                break;
            case OpSetConstant:
            {
                // The constants in the packet itself, after the offset and
                // type word: two and a half million of these a run went by
                // uncounted, and the level's geometry was projected through
                // whatever the constants held from before.
                uint32_t index;
                if (ConstantIndex(ReadDword(cursor + 1), index))
                {
                    if (TraceConstants())
                    {
                        printf("const: set %08X -> %04X x%u:", ReadDword(cursor + 1), index, count - 1);
                        for (uint32_t i = 0; i + 1 < count && i < 8; i++) { const uint32_t v = ReadDword(cursor + 2 + i); float f; memcpy(&f, &v, 4); printf(" %g", f); }
                        printf("\n");
                    }
                    t_writeSource = "a SET_CONSTANT packet";
                    for (uint32_t i = 0; i + 1 < count; i++)
                        WriteRegister(ApertureBase + (index + i) * 4, ReadDword(cursor + 2 + i));
                }
                break;
            }
            case OpSetConstant2:
            case OpSetShaderConstants:
            {
                // The same with the register index given whole.
                const uint32_t index = ReadDword(cursor + 1) & 0xFFFF;
                if (index >= 0x2000 && index + count < 0x5000)
                {
                    t_writeSource = "a SET_CONSTANT2 packet";
                    for (uint32_t i = 0; i + 1 < count; i++)
                        WriteRegister(ApertureBase + (index + i) * 4, ReadDword(cursor + 2 + i));
                }
                else ReportOnce("SET_CONSTANT2 outside the constants, ignored", index, count);
                break;
            }
            case OpEventWriteShd:
                WriteFence(ReadDword(cursor + 1), ReadDword(cursor + 2),
                           ReadDword(cursor + 3));
                break;
            case OpWaitRegMem:
                WaitForValue(ReadDword(cursor + 1), ReadDword(cursor + 2),
                             ReadDword(cursor + 3), ReadDword(cursor + 4));
                break;
            case OpCondWrite:
                ConditionalWrite(ReadDword(cursor + 1), ReadDword(cursor + 2),
                                 ReadDword(cursor + 3), ReadDword(cursor + 4),
                                 ReadDword(cursor + 5), ReadDword(cursor + 6));
                break;
            case OpSwap:
                local.swaps++;
                SwapPacket(ReadDword(cursor + 1), ReadDword(cursor + 2), ReadDword(cursor + 3), ReadDword(cursor + 4));
                break;
            case OpInterrupt:
                // The stream asking for the CPU to be interrupted.
                //
                // These were counted and dropped, two hundred and forty six of
                // them a run. The title's driver learns that work has finished
                // in the handler this raises, and a driver that is never told
                // anything finished eventually decides the GPU has stopped and
                // says so in as many words. It is raised where it appears in
                // the stream, after everything queued before it has been done.
                RaiseInterruptNow(ReadDword(cursor + 1));
                break;
            case OpIndirectBuffer:
            {
                // The real command stream lives in these. Skipping them would
                // leave the driver waiting for work that was never done.
                const uint32_t target = ReadDword(cursor + 1);
                const uint32_t targetDwords = ReadDword(cursor + 2) & 0xFFFFF;
                Timeline::Mark("cp buffer", target, targetDwords);
                t_lastIndirectAddress = target;
                t_lastIndirectSize = ReadDword(cursor + 2);
                t_lastIndirectSizeAt = ringBase + ((cursor + 2) % ringSizeDwords) * 4;

                // COD3_TRACEPOOL=1: every buffer the ring hands over once a
                // level is loading, against the title's own record of what
                // it submitted.
                static const bool tracePool = []() {
                    const char* text = getenv("COD3_TRACEPOOL");
                    return text != nullptr && text[0] != 0 && text[0] != '0';
                }();
                if (tracePool && Kernel::Stats().filesOpened.load(std::memory_order_relaxed) >= 40)
                {
                    static std::atomic<int> announced{ 0 };
                    if (announced.fetch_add(1) < 3000)
                    {
                        printf("pool:   ring runs buffer %08X, %u dwords, at ring %u\n",
                            target, targetDwords, cursor);
                    }
                }
                ExecuteBuffer(Guest::PhysicalAlias(target), targetDwords, local, 1);
                break;
            }
            default:
                if (Type3Name(opcode) == nullptr) local.unknown++;
                break;
            }

            // Every type 3 packet is reported the first time it appears, which
            // is how the list above grows as the title exercises more of the
            // command set.
            static std::mutex opcodeMutex;
            static std::map<uint32_t, bool> seenOpcodes;
            bool first = false;
            {
                std::lock_guard<std::mutex> lock(opcodeMutex);
                if (seenOpcodes.find(opcode) == seenOpcodes.end())
                {
                    seenOpcodes[opcode] = true;
                    first = true;
                }
            }
            if (first)
            {
                const char* name = Type3Name(opcode);
                printf("gpu: first %s packet (opcode 0x%02X, %u dwords)\n",
                    name ? name : "unrecognised", opcode, count);
                fflush(stdout);
            }
        }
        else
        {
            // Type 1 is two register writes and is not used by this title. A
            // header that decodes as neither means the walk has lost sync.
            printf("gpu: lost sync at ring dword %u, header 0x%08X\n", cursor, header);
            fflush(stdout);
            return writePointer;
        }

        CountPacket(type, type == 3 ? ((header >> 8) & 0x7F) : 0);
        local.packets++;
        // A packet that runs past the write pointer.
        //
        // The walk stops only when the cursor lands on the write pointer
        // exactly; a packet that straddles it carries the cursor over, and
        // the loop then walks the rest of the ring, which is whatever the
        // title has not written yet, as commands. That is how a run of zero
        // words and then shader constants came to be executed. Either the
        // pointer was read mid packet, or the packet's length is wrong; both
        // are reported, and the walk stops at the pointer either way.
        {
            const uint32_t distance = (writePointer + ringSizeDwords - cursor) % ringSizeDwords;
            if (distance != 0 && length > distance)
            {
                static std::atomic<int> announced{ 0 };
                if (announced.fetch_add(1) < 6)
                {
                    const uint32_t now = Gpu::WritePointer();
                    printf("gpu: a %u dword packet at ring dword %u runs past the write "
                           "pointer %u (the register now says %u); header 0x%08X\n",
                        length, cursor, writePointer, now, header);
                    PrintRecent();
                }
                cursor = writePointer;
                break;
            }
        }
        cursor = (cursor + length) % ringSizeDwords;
    }

    {
        std::lock_guard<std::mutex> lock(g_statisticsMutex);
        g_statistics.packets += local.packets;
        g_statistics.registerWrites += local.registerWrites;
        g_statistics.draws += local.draws;
        g_statistics.swaps += local.swaps;
        g_statistics.unknown += local.unknown;
        g_statistics.readPointer = cursor;
    }

    return cursor;
}

namespace
{
    void ExecuteBuffer(uint32_t base, uint32_t dwords, Gpu::Statistics& local, int depth)
    {
        // Indirect buffers can chain. A few levels is normal; more than that
        // means a corrupt address, and following it would read arbitrary guest
        // memory as commands.
        if (depth > 4 || base == 0 || dwords == 0 || dwords > (16u << 20)) return;

        // COD3_NATIVE=2: a buffer a device's stream was read over runs from
        // its native list (native_state.cpp): each packet still to run, on
        // its own, the state between them left to the draws' records.
        static thread_local bool t_native = false;
        static thread_local std::vector<NativeState::Item> t_items;
        if (!t_native && NativeState::Executing())
        {
            std::vector<NativeState::Item> items;
            items.swap(t_items);
            uint32_t covered = 0;
            if (NativeState::NativeRun(base, dwords, items, covered))
            {
                t_native = true;
                for (const NativeState::Item& item : items)
                    ExecuteBuffer(Guest::PhysicalAlias(item.physical), item.dwords, local, depth);
                if (covered < dwords) ExecuteBuffer(base + covered * 4, dwords - covered, local, depth);
                t_native = false;
                items.swap(t_items);
                return;
            }
            items.swap(t_items);
        }

        static std::atomic<int> announced{ 0 };
        if (announced.fetch_add(1) < 3)
        {
            printf("gpu: indirect buffer at 0x%08X, %u dwords, starts:", base, dwords);
            for (uint32_t i = 0; i < 6 && i < dwords; i++)
                printf(" %08X", Guest::Read32(Guest::Base, base + i * 4));
            printf("\n");
            fflush(stdout);
        }

        uint32_t cursor = 0;
        uint32_t stepped = 0;
        while (cursor < dwords)
        {
            // A buffer is always run to the end. Abandoning one part way
            // through loses the fence writes that live at its end, and the
            // driver then waits for work it believes is still running: the
            // title went quiet after two hundred draws when this was tried.
            ++stepped;

            const uint32_t header = Guest::Read32(Guest::Base, base + cursor * 4);
            t_packetAt = base + cursor * 4;
            const uint32_t type = header >> 30;
            uint32_t length = 1;

            if (type == 0)
            {
                const uint32_t count = ((header >> 16) & 0x3FFF) + 1;
                const uint32_t baseRegister = header & 0x7FFF;
                const bool oneRegister = ((header >> 15) & 1) != 0;

                {
                    uint32_t words[5];
                    uint32_t available = 0;
                    for (; available < 5 && available < count &&
                           cursor + 1 + available < dwords; available++)
                        words[available] = Guest::Read32(Guest::Base,
                            base + (cursor + 1 + available) * 4);
                    Remember(header, words, available);
                }

                // A register write that lands on the command processor's own
                // control registers, 0x1C0 to 0x1FF, with what came before it.
                // The scratch block address was seen taking a packet header as
                // its value, which is a stream read out of step: the packet
                // before this one was sized wrong, and these are its tail.
                if (baseRegister != 0 && baseRegister >= 0x1C0 && baseRegister + count > 0x1DC && baseRegister < 0x200)
                {
                    static std::atomic<int> announced{ 0 };
                    if (announced.fetch_add(1) < 20)
                    {
                        printf("gpu: register write of %u from 0x%04X, header 0x%08X, "
                               "at dword %u of a %u dword buffer at 0x%08X\n",
                            count, baseRegister, header, cursor, dwords, base);
                        printf("  the words:");
                        for (uint32_t i = 0; i < count && i < 8 && cursor + 1 + i < dwords; i++)
                            printf(" %08X", Guest::Read32(Guest::Base, base + (cursor + 1 + i) * 4));
                        printf("\n");

                        // The buffer around the cursor, so the shape of what
                        // was submitted can be seen: where the packets end,
                        // what follows them, and how far the declared size
                        // runs past that.
                        printf("  reached through INDIRECT_BUFFER address 0x%08X size word 0x%08X\n",
                            t_lastIndirectAddress, t_lastIndirectSize);
                        printf("  the buffer begins:");
                        for (uint32_t i = 0; i < 12 && i < dwords; i++)
                            printf(" %08X", Guest::Read32(Guest::Base, base + i * 4));
                        printf("\n");
                        if (baseRegister < 0x100 && count > 64)
                        {
                        // Is the CPU still writing this buffer? If the words
                        // here change while the command processor waits, the
                        // buffer was submitted before it was finished, or
                        // rather this thread got to it before the title had
                        // finished with it.
                        {
                            const uint32_t at = cursor > 34 ? cursor - 34 : 0;
                            uint32_t before[6];
                            for (int i = 0; i < 6; i++)
                                before[i] = Guest::Read32(Guest::Base, base + (at + i) * 4);
                            // Up to half a second, polling: if the words fill in
                            // while this waits, the buffer was being written as
                            // it was being read, and how long it took says how
                            // far behind the command processor ought to run.
                            int waitedMs = 0;
                            for (; waitedMs < 500; waitedMs += 5)
                            {
                                bool any = false;
                                for (int i = 0; i < 6; i++)
                                    if (Guest::Read32(Guest::Base, base + (at + i) * 4) != before[i]) any = true;
                                if (any) break;
                                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                            }
                            printf("  after %d ms, dwords %u..%u read:", waitedMs, at, at + 5);
                            bool changed = false;
                            for (int i = 0; i < 6; i++)
                            {
                                const uint32_t now = Guest::Read32(Guest::Base, base + (at + i) * 4);
                                printf(" %08X", now);
                                if (now != before[i]) changed = true;
                            }
                            printf("%s\n", changed ? "  (CHANGED: the CPU was still writing)" : "  (unchanged)");

                            // And the size word of the packet that brought us
                            // here: a size that changes after the fact is a
                            // packet that was read before it was finished.
                            if (t_lastIndirectSizeAt != 0)
                                printf("  the indirect buffer's size word at 0x%08X now reads 0x%08X "
                                       "(was 0x%08X when followed)\n",
                                    t_lastIndirectSizeAt,
                                    Guest::Read32(Guest::Base, t_lastIndirectSizeAt),
                                    t_lastIndirectSize);
                        }
                        }
                        const uint32_t from = cursor > 72 ? cursor - 72 : 0;
                        printf("  the buffer from dword %u:", from);
                        for (uint32_t i = from; i < dwords && i < cursor + 12; i++)
                        {
                            if (((i - from) % 8) == 0) printf("\n    %4u:", i);
                            printf(" %08X", Guest::Read32(Guest::Base, base + i * 4));
                        }
                        printf("\n");
                        PrintRecent();
                    }
                }

                // A write starting at register zero is padding.
                //
                // The driver fills the unused tail of a command buffer with
                // zero words, and a zero word is a type 0 packet that writes
                // one value to register zero. It also submits the buffer at
                // its allocated length rather than its filled length, so past
                // the padding the walk reaches whatever the allocator put next:
                // here a table of shader constants beginning with 1.0f, which
                // as a header is a type 0 write of 16257 registers from zero.
                // Carried out, that put the constants into the command
                // processor's own registers, scratch block address included,
                // and every "GPU is hung" the driver reported followed from
                // it. Nothing legitimate writes a run of registers from zero;
                // the driver evidently relies on the hardware ignoring it, and
                // so does this. The words are consumed and nothing is written.
                if (baseRegister == 0)
                {
                    local.filler++;
                    length = 1 + count;
                }
                else
                {
                t_writeSource = "a type 0 packet in an indirect buffer";
                for (uint32_t i = 0; i < count; i++)
                {
                    if (cursor + 1 + i >= dwords) break;
                    const uint32_t value = Guest::Read32(Guest::Base, base + (cursor + 1 + i) * 4);
                    const uint32_t target = oneRegister ? baseRegister : baseRegister + i;
                    Gpu::WriteRegister(Gpu::ApertureBase + target * 4, value);
                }
                local.registerWrites += count;
                length = 1 + count;
                }
            }
            else if (type == 2)
            {
                length = 1;
                Remember(header, nullptr, 0);
            }
            else if (type == 3)
            {
                const uint32_t opcode = (header >> 8) & 0x7F;
                const uint32_t count = ((header >> 16) & 0x3FFF) + 1;
                length = 1 + count;

                {
                    uint32_t words[5];
                    uint32_t available = 0;
                    for (; available < 5 && available < count &&
                           cursor + 1 + available < dwords; available++)
                        words[available] = Guest::Read32(Guest::Base,
                            base + (cursor + 1 + available) * 4);
                    Remember(header, words, available);
                }

                if (!BinPass(header, opcode))
                {
                    // predicated out: nothing runs
                }
                else if (opcode >= OpSetBinMaskLo && opcode <= OpSetBinSelectHi && cursor + 1 < dwords)
                    SetBin(opcode, Guest::Read32(Guest::Base, base + (cursor + 1) * 4));
                else if (opcode == OpInterrupt)
                {
                    // Nearly all of the stream is inside these buffers, and so
                    // are nearly all of the interrupts; raising them only from
                    // the outer ring missed almost every one. The word after
                    // the header is the mask of CPUs to interrupt.
                    const uint32_t cpuMask = cursor + 1 < dwords
                        ? Guest::Read32(Guest::Base, base + (cursor + 1) * 4) : 0;
                    static const bool tracePool = []() {
                        const char* text = getenv("COD3_TRACEPOOL");
                        return text != nullptr && text[0] != 0 && text[0] != '0';
                    }();
                    if (tracePool && Kernel::Stats().filesOpened.load(std::memory_order_relaxed) >= 40)
                    {
                        printf("pool:   interrupt packet, cpu mask %u, raiser %s\n", cpuMask,
                            g_raiseInterrupt != nullptr ? "installed" : "missing");
                    }
                    RaiseInterruptNow(cpuMask);
                }
                else if (opcode == OpDrawIndx && cursor + 2 < dwords)
                {
                    local.draws++;
                    // COD3_CALLPS=1: the pixel program the title's draw
                    // call named, from d3d_hooks.cpp, for colour draws (the
                    // native path's first step; off, since the stream's
                    // program was found to agree with it).
                    const uint32_t lastWord = base + (cursor + count) * 4;
                    if ((g_registerFile[0x2208].load(std::memory_order_relaxed) & 7) == 4)
                    {
                        uint32_t programPhysical = 0, programDwords = 0;
                        if (D3dHooks::PixelProgram(lastWord, programPhysical, programDwords))
                            Render::ShaderLoaded(true, programPhysical, programDwords);
                    }
                    if (D3dHooks::Observing())
                        D3dHooks::CheckDraw(lastWord, Render::CurrentProgramHash(false), Render::CurrentProgramHash(true), g_registerFile[0x2208].load(std::memory_order_relaxed));
                    if (NativeState::Checking() && !NativeState::Executing())
                        NativeState::Check(lastWord, Guest::Read32(Guest::Base, base + (cursor + 2) * 4),
                            cursor + 3 < dwords ? Guest::Read32(Guest::Base, base + (cursor + 3) * 4) : 0);
                    if (TraceConstants()) printf("const: draw indexed vs_%016llx\n", (unsigned long long)Render::CurrentProgramHash(false));
                    const bool native = NativeState::Begin(lastWord);
                    DecodeDraw(
                        Guest::Read32(Guest::Base, base + (cursor + 2) * 4),
                        cursor + 3 < dwords
                            ? Guest::Read32(Guest::Base, base + (cursor + 3) * 4) : 0,
                        cursor + 4 < dwords
                            ? Guest::Read32(Guest::Base, base + (cursor + 4) * 4) : 0);
                    if (native) NativeState::End();
                }
                else if (opcode == OpDrawIndx2 && cursor + 1 < dwords)
                {
                    local.draws++;
                    if (TraceConstants()) printf("const: draw vs_%016llx\n", (unsigned long long)Render::CurrentProgramHash(false));
                    const uint32_t w1 = Guest::Read32(Guest::Base, base + (cursor + 1) * 4);
                    if (NativeState::Checking() && !NativeState::Executing()) NativeState::Check(base + (cursor + count) * 4, w1, 0);
                    DumpDraw("DRAW_INDX_2", header, w1,
                        cursor + 2 < dwords ? Guest::Read32(Guest::Base, base + (cursor + 2) * 4) : 0);
                    const bool native = NativeState::Begin(base + (cursor + count) * 4);
                    DecodeDraw(w1);
                    if (native) NativeState::End();
                }
                else if (opcode == OpImLoadImmediate && cursor + 2 < dwords)
                {
                    DecodeShaderLoad(Guest::Read32(Guest::Base, base + (cursor + 1) * 4),
                                     Guest::Read32(Guest::Base, base + (cursor + 2) * 4),
                                     base + (cursor + 3) * 4);
                }
                else if (opcode == OpImLoad && cursor + 2 < dwords)
                {
                    DecodeShaderLoadIndirect(
                        Guest::Read32(Guest::Base, base + (cursor + 1) * 4),
                        Guest::Read32(Guest::Base, base + (cursor + 2) * 4));
                }
                else if (opcode == OpLoadAluConstant && cursor + 3 < dwords)
                {
                    LoadConstantsFromMemory(
                        Guest::Read32(Guest::Base, base + (cursor + 1) * 4),
                        Guest::Read32(Guest::Base, base + (cursor + 2) * 4),
                        Guest::Read32(Guest::Base, base + (cursor + 3) * 4));
                }
                else if (opcode == OpSetConstant && cursor + 1 < dwords)
                {
                    // Constants in the packet: nearly all of the level's are
                    // set this way, and nearly all of the stream is in these
                    // buffers, so this is where the projection matrices of
                    // the level's geometry arrive.
                    const uint32_t count = ((header >> 16) & 0x3FFF) + 1;
                    uint32_t index;
                    if (ConstantIndex(Guest::Read32(Guest::Base, base + (cursor + 1) * 4), index))
                    {
                        if (TraceConstants())
                        {
                            printf("const: set %08X -> %04X x%u:", Guest::Read32(Guest::Base, base + (cursor + 1) * 4), index, count - 1);
                            for (uint32_t i = 0; i + 1 < count && i < 8; i++) { const uint32_t v = Guest::Read32(Guest::Base, base + (cursor + 2 + i) * 4); float f; memcpy(&f, &v, 4); printf(" %g", f); }
                            printf("\n");
                        }
                        t_writeSource = "a SET_CONSTANT packet";
                        for (uint32_t i = 0; i + 1 < count && cursor + 2 + i < dwords; i++)
                            Gpu::WriteRegister(Gpu::ApertureBase + (index + i) * 4, Guest::Read32(Guest::Base, base + (cursor + 2 + i) * 4));
                    }
                }
                else if ((opcode == OpSetConstant2 || opcode == OpSetShaderConstants) && cursor + 1 < dwords)
                {
                    // The index is the register itself. Only the registers
                    // and constants above 0x2000 are taken: one of these
                    // taken from a mis-sized packet wrote the command
                    // processor's own write pointer, and the ring stopped.
                    const uint32_t count = ((header >> 16) & 0x3FFF) + 1;
                    const uint32_t index = Guest::Read32(Guest::Base, base + (cursor + 1) * 4) & 0xFFFF;
                    if (index >= 0x2000 && index + count < 0x5000)
                    {
                        t_writeSource = "a SET_CONSTANT2 packet";
                        for (uint32_t i = 0; i + 1 < count && cursor + 2 + i < dwords; i++)
                            Gpu::WriteRegister(Gpu::ApertureBase + (index + i) * 4, Guest::Read32(Guest::Base, base + (cursor + 2 + i) * 4));
                    }
                    else ReportOnce("SET_CONSTANT2 outside the constants, ignored", index, count);
                }
                else if (opcode == OpWaitRegMem && cursor + 4 < dwords)
                {
                    WaitForValue(
                        Guest::Read32(Guest::Base, base + (cursor + 1) * 4),
                        Guest::Read32(Guest::Base, base + (cursor + 2) * 4),
                        Guest::Read32(Guest::Base, base + (cursor + 3) * 4),
                        Guest::Read32(Guest::Base, base + (cursor + 4) * 4));
                }
                else if (opcode == OpEventWrite && cursor + 1 < dwords)
                {
                    // Events: which ones the title raises, once each, with
                    // the sample count registers as they stand, for the
                    // occlusion queries the title may be waiting on.
                    const uint32_t initiator = Guest::Read32(Guest::Base, base + (cursor + 1) * 4);
                    static std::mutex mutex;
                    static std::map<uint32_t, uint32_t> seen;
                    std::lock_guard<std::mutex> lock(mutex);
                    if (++seen[initiator & 0x3F] <= 2)
                    {
                        printf("gpu: EVENT_WRITE event %u (initiator %08X), sample count control %08X address %08X, viz query %08X\n",
                            initiator & 0x3F, initiator, g_registerFile[0x2324].load(), g_registerFile[0x2325].load(), g_registerFile[0x2293].load());
                        fflush(stdout);
                    }
                }
                else if (opcode == OpEventWriteShd && cursor + 3 < dwords)
                {
                    WriteFence(Guest::Read32(Guest::Base, base + (cursor + 1) * 4),
                               Guest::Read32(Guest::Base, base + (cursor + 2) * 4),
                               Guest::Read32(Guest::Base, base + (cursor + 3) * 4));
                }
                else if (opcode == OpCondWrite && cursor + 6 < dwords)
                {
                    ConditionalWrite(
                        Guest::Read32(Guest::Base, base + (cursor + 1) * 4),
                        Guest::Read32(Guest::Base, base + (cursor + 2) * 4),
                        Guest::Read32(Guest::Base, base + (cursor + 3) * 4),
                        Guest::Read32(Guest::Base, base + (cursor + 4) * 4),
                        Guest::Read32(Guest::Base, base + (cursor + 5) * 4),
                        Guest::Read32(Guest::Base, base + (cursor + 6) * 4));
                }
                else if (opcode == OpSwap)
                {
                    local.swaps++;
                    if (cursor + 4 < dwords)
                        SwapPacket(
                            Guest::Read32(Guest::Base, base + (cursor + 1) * 4),
                            Guest::Read32(Guest::Base, base + (cursor + 2) * 4),
                            Guest::Read32(Guest::Base, base + (cursor + 3) * 4),
                            Guest::Read32(Guest::Base, base + (cursor + 4) * 4));
                }
                else if (opcode == OpIndirectBuffer && cursor + 2 < dwords)
                {
                    t_lastIndirectAddress = Guest::Read32(Guest::Base, base + (cursor + 1) * 4);
                    t_lastIndirectSize = Guest::Read32(Guest::Base, base + (cursor + 2) * 4);
                    t_lastIndirectSizeAt = base + (cursor + 2) * 4;
                    ExecuteBuffer(
                        Guest::PhysicalAlias(t_lastIndirectAddress),
                        t_lastIndirectSize & 0xFFFFF,
                        local, depth + 1);
                }
                else if (Type3Name(opcode) == nullptr)
                {
                    local.unknown++;
                }
            }
            else
            {
                return;   // lost sync inside the buffer
            }

            CountPacket(type, type == 3 ? ((header >> 8) & 0x7F) : 0);
            local.packets++;
            cursor += length;
        }
    }
}

void Gpu::ReportPacketMix()
{
    {
        std::lock_guard<std::mutex> lock(g_registerMutex);
        if (!g_registerWrites.empty())
        {
            printf("registers written in the level, by index:");
            for (const auto& entry : g_registerWrites)
                printf(" %04X x%llu", entry.first, (unsigned long long)entry.second);
            printf("\n");
        }
    }
    std::lock_guard<std::mutex> lock(g_histogramMutex);
    if (g_type0 + g_type2 + g_type3 == 0) return;

    printf("packets: %llu register writes, %llu filler, %llu commands\n",
        (unsigned long long)g_type0, (unsigned long long)g_type2,
        (unsigned long long)g_type3);

    {
        std::lock_guard<std::mutex> draws(g_drawMutex);
        if (!g_draws.primitives.empty())
        {
            printf("geometry:");
            for (const auto& entry : g_draws.primitives)
                printf(" %s x%llu", PrimitiveName(entry.first),
                    (unsigned long long)entry.second);
            printf(", %llu indices\n", (unsigned long long)g_draws.indices);
        }
        if (g_draws.vertexShaders + g_draws.pixelShaders > 0)
            printf("shaders: %llu vertex, %llu pixel, %llu dwords of microcode, "
                   "last at 0x%08X and 0x%08X\n",
                (unsigned long long)g_draws.vertexShaders,
                (unsigned long long)g_draws.pixelShaders,
                (unsigned long long)g_draws.shaderDwords,
                g_draws.lastVertexShader, g_draws.lastPixelShader);
    }

    if (g_waits.load() != 0)
        printf("waits: %llu, of which %llu gave up waiting, %.2f s waiting\n",
            (unsigned long long)g_waits.load(),
            (unsigned long long)g_waitTimeouts.load(), g_waitNanoseconds.load() / 1e9);

    if (g_fences.load() != 0)
        printf("fences: %llu written\n", (unsigned long long)g_fences.load());

    if (g_type3Opcodes.empty()) return;
    printf("commands:");
    for (const auto& entry : g_type3Opcodes)
    {
        const char* name = Type3Name(entry.first);
        if (name) printf(" %s x%llu", name, (unsigned long long)entry.second);
        else      printf(" 0x%02X x%llu", entry.first, (unsigned long long)entry.second);
    }
    printf("\n");
    fflush(stdout);
}

uint64_t Mmio::Timebase()
{
    // Counted from the first call so the numbers stay small enough to scale
    // without a wider multiply.
    static const int64_t frequency = []() {
        LARGE_INTEGER value;
        QueryPerformanceFrequency(&value);
        return value.QuadPart;
    }();
    static const int64_t origin = []() {
        LARGE_INTEGER value;
        QueryPerformanceCounter(&value);
        return value.QuadPart;
    }();

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const int64_t ticks = now.QuadPart - origin;
    if (frequency <= 0 || ticks < 0) return 0;

    constexpr int64_t ConsoleFrequency = 49875000;
    return uint64_t((ticks / frequency) * ConsoleFrequency
                  + (ticks % frequency) * ConsoleFrequency / frequency);
}

void Mmio::CallIndirect(PPCContext& ctx, uint8_t* base, uint32_t address)
{
    // The title's own code through its table; anything else is in a
    // level's DLL, if one is loaded, or nowhere.
    PPCFunc* routine = (address >= PPC_CODE_BASE &&
                        address < PPC_CODE_BASE + PPC_CODE_SIZE)
        ? PPC_LOOKUP_FUNC(base, address) : Guest::LookupModule(address);

    // COD3_TRACECALL names a call site by its return address, in hex, and
    // every distinct target called from it is printed once with the first
    // three arguments: which method a virtual call goes to, in other words.
    // Several sites may be named, separated by commas. The value the call
    // returns is printed too, which for a chain of initialisations that each
    // run only if the last succeeded says which one failed.
    static const std::vector<uint32_t> traced = []() {
        std::vector<uint32_t> sites;
        const char* text = getenv("COD3_TRACECALL");
        for (const char* at = text; at != nullptr && *at != 0;)
        {
            char* end = nullptr;
            const uint32_t site = uint32_t(strtoul(at, &end, 16));
            if (end == at) break;
            sites.push_back(site);
            at = *end == ',' ? end + 1 : end;
        }
        return sites;
    }();
    bool tracing = false;
    for (uint32_t site : traced) tracing |= (site == uint32_t(ctx.lr));

    // COD3_TRACETARGET names targets rather than sites: every call through
    // a pointer that lands on one of them.
    static const std::vector<uint32_t> tracedTargets = []() {
        std::vector<uint32_t> targets;
        const char* text = getenv("COD3_TRACETARGET");
        for (const char* at = text; at != nullptr && *at != 0;)
        {
            char* end = nullptr;
            const uint32_t target = uint32_t(strtoul(at, &end, 16));
            if (end == at) break;
            targets.push_back(target);
            at = *end == ',' ? end + 1 : end;
        }
        return targets;
    }();
    for (uint32_t target : tracedTargets) tracing |= (target == address);

    // COD3_TRACEMODULE=1 traces every call that crosses into or out of a
    // level's DLL through a pointer: the interface between the title and
    // the level, which is where a level that never starts stops.
    static const bool traceModule = []() {
        const char* text = getenv("COD3_TRACEMODULE");
        return text != nullptr && text[0] != 0 && text[0] != '0';
    }();
    if (traceModule && (Modules::Contains(address) != Modules::Contains(uint32_t(ctx.lr))))
    {
        static std::atomic<int> crossings{ 0 };
        if (crossings.fetch_add(1) < 400) tracing = true;
    }
    if (tracing)
    {
        static std::mutex mutex;
        static std::map<uint64_t, int> targets;
        std::lock_guard<std::mutex> lock(mutex);
        if (targets[(uint64_t(ctx.lr) << 32) | address]++ >= 3) tracing = false;
    }
    if (tracing)
    {
        printf("trace: call from 0x%08X to 0x%08X with r3 0x%08X, r4 0x%08X, r5 0x%08X",
            uint32_t(ctx.lr), address, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
        uint32_t functions[12] = {};
        const int count = Sampler::FunctionsOnStack(functions, 12);
        for (int i = 1; i < count; i++) printf("%s%08X", i == 1 ? "  <- " : " ", functions[i]);
        printf("\n");
        fflush(stdout);
    }

    if (routine != nullptr)
    {
        const uint32_t site = uint32_t(ctx.lr);
        routine(ctx, base);
        if (tracing)
        {
            printf("trace: call from 0x%08X to 0x%08X returned 0x%08X\n", site, address, ctx.r3.u32);
            fflush(stdout);
        }
        return;
    }

    // Once per distinct address: the same bad call in a loop would otherwise
    // bury everything else.
    static std::mutex mutex;
    static std::map<uint32_t, bool> seen;
    bool first = false;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (seen.find(address) == seen.end()) { seen[address] = true; first = true; }
    }

    if (first)
    {
        printf("\ncall: through a pointer to 0x%08X, which is not a function\n",
            address);
        printf("  the call was made with r3 0x%08X, r4 0x%08X, r5 0x%08X\n",
            ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
        if (ctx.r3.u32 >= 0x10000 && ctx.r3.u32 < 0xC0000000)
            printf("  the object at r3 begins %08X %08X %08X %08X\n",
                Guest::Read32(base, ctx.r3.u32),
                Guest::Read32(base, ctx.r3.u32 + 4),
                Guest::Read32(base, ctx.r3.u32 + 8),
                Guest::Read32(base, ctx.r3.u32 + 12));

        // Every register, because the one that matters is never the one the
        // call convention names: the object came out of a container held in a
        // callee saved register, and that is what says where it came from.
        printf("  registers r0 to r31:");
        for (int i = 0; i < 32; i++)
        {
            if ((i % 8) == 0) printf("\n   r%-2d", i);
            printf(" %08X", Kernel::Register(ctx, i));
        }
        printf("\n");

        // The same physical address through every window it has.
        //
        // Guest memory below 0x20000000 is visible at four more addresses, and
        // the console makes them one and the same memory. If they are not the
        // same here, a structure written through one window reads back as zero
        // through another, which is exactly what an object with no vtable looks
        // like. Printing all five says which it is.
        if (ctx.r3.u32 >= 0x80000000u)
        {
            const uint32_t offset = ctx.r3.u32 & 0x1FFFFFFFu;
            static const uint32_t windows[] = {
                0x00000000u, 0x80000000u, 0xA0000000u, 0xC0000000u, 0xE0000000u };
            for (uint32_t window : windows)
            {
                printf("  window 0x%08X ->", window | offset);
                for (int word = 0; word < 4; word++)
                    printf(" %08X", Guest::Read32(base, (window | offset) + word * 4));
                printf("\n");
            }
        }

        // What the callee saved registers point at.
        //
        // The object a virtual call is made on comes out of a container, and
        // the container is what says why it is empty. Those live in the
        // registers a function keeps across calls, so every one of them that
        // looks like a pointer into the image gets its first words printed.
        for (int i = 14; i < 32; i++)
        {
            const uint32_t value = Kernel::Register(ctx, i);
            if (value < 0x82000000u || value >= 0x82C30000u) continue;
            printf("  r%-2d 0x%08X ->", i, value);
            for (int word = 0; word < 6; word++)
                printf(" %08X", Guest::Read32(base, value + word * 4));
            printf("\n");
        }

        Kernel::DumpRequested();

        uint32_t functions[10] = {};
        const int count = Sampler::FunctionsOnStack(functions, 10);
        if (count > 0)
        {
            printf("  guest call chain, innermost first:");
            for (int i = 0; i < count; i++)
                printf("%ssub_%08X", i == 0 ? " " : " <- ", functions[i]);
            printf("\n");
        }
        fflush(stdout);
    }

    // Nothing is called and nothing is returned. The console would fault here
    // too, so the title is somewhere it did not mean to be; carrying on shows
    // what it does next instead of ending the run at the first sign of it.
    ctx.r3.u64 = 0;
}

uint32_t Gpu::TakePendingInterrupts()
{
    return g_pendingInterrupts.exchange(0, std::memory_order_relaxed);
}

void Gpu::ReportFences()
{
    std::lock_guard<std::mutex> guard(g_fenceTargetMutex);
    if (g_fenceTargets.empty()) return;

    printf("fences went to:");
    for (const auto& entry : g_fenceTargets)
        printf(" 0x%08X x%llu", entry.first, (unsigned long long)entry.second);
    printf("\n");

    {
        std::lock_guard<std::mutex> guard(g_condTargetMutex);
        if (!g_condTargets.empty())
        {
            printf("conditional writes to registers, allowed of asked:");
            int shown = 0;
            for (const auto& entry : g_condTargets)
            {
                if (shown++ >= 8) break;
                printf(" 0x%08X %llu/%llu", entry.first,
                    (unsigned long long)entry.second.second,
                    (unsigned long long)entry.second.first);
            }
            printf("\n");
        }
    }
    fflush(stdout);
}

void Gpu::SetInterruptRaiser(void (*raiser)(uint32_t cpuMask))
{
    g_raiseInterrupt = raiser;
}
