// The title's linked-in Xbox 360 D3D, taken over call by call: the layer of
// docs/native-d3d11-plan.md. Each hook replaces one function of the library
// (0x822E0000..0x82300000) through the recompiled function's weak alias, as
// title_fixes.cpp does.
//
// F1, observing: the hooks count the calls, by the host thread that makes
// them, and call the original, so nothing changes yet but what is known.
// COD3_D3DHOOKS=1 prints the counts every five seconds.

#include "kernel.h"
#include "d3d_hooks.h"
#include "render_shaders.h"
#include "native_state.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <set>

#include <Windows.h>

namespace
{
    enum Call { DrawIndexed, DrawIndexedB, DrawIndexedC, DrawImmediate, RectangleEvent, ShaderLoad, SwapCountdown, IndirectA, IndirectB, GameDraw, DrawD, DrawE, DrawF, DrawG, CallCount };
    const char* const CallNames[CallCount] = { "draw 822F3620", "draw tessellated 822F3EA0", "draw indexed tessellated 822F4338",
        "draw immediate 822F6858", "rectangle + event 822EFFF0", "shader load 822FB4B0", "swap countdown 822F4A10",
        "(ring write 822F1E68: pool_trace.cpp)", "indirect buffer 822F8590",
        "game's own draw 82154930", "draw 822F30F0", "draw 822F3A28", "draw 822F6DF8", "draw 82301888" };

    bool ObservingNow()
    {
        static const bool on = getenv("COD3_D3DHOOKS") != nullptr;
        return on;
    }

    // What the title's draw call said, by where its draw packet ends in the
    // command buffer (physical): the device's programs at the call.
    struct DrawRecord { uint32_t vertexObject, pixelObject, count; uint64_t pixelHash; uint32_t pixelPhysical, pixelDwords, lastWordValue; };

    // A pixel shader object's own program: its microcode, where the shader
    // load (sub_822FB4B0) finds it, hashed the way the program cache does.
    bool PixelMicrocode(uint8_t* base, uint32_t object, uint32_t& physical, uint32_t& dwords)
    {
        if (object == 0) return false;
        const uint32_t info = object + Guest::Read32(base, object + 64);
        physical = Guest::PhysicalAlias(Guest::Read32(base, info + 40) + Guest::Read32(base, object + 24));
        dwords = Guest::Read32(base, info + 44) >> 2;
        return dwords != 0 && dwords <= 0x4000;
    }

    // COD3_CALLPS=1: a colour draw's pixel program is the one its draw call
    // named rather than the one the stream loaded last. The parity check
    // found the two always the same when the record is the draw's own (a
    // record left at a reused address, from a draw function not hooked
    // yet, is told apart by the packet's last word), so this is off; it is
    // the first step of the native path.
    bool StreamPixelPrograms()
    {
        static const bool on = getenv("COD3_CALLPS") == nullptr;
        return on;
    }
    std::mutex g_recordMutex;
    std::unordered_map<uint32_t, DrawRecord> g_records;

    // The parity check: draws the command processor ran, how many had a
    // record, and whether each program object always stood for the same
    // loaded program.
    std::mutex g_parityMutex;
    uint64_t g_executed = 0, g_matched = 0, g_nullPixel = 0, g_colourDraws = 0, g_ownProgram = 0;
    uint64_t g_fresh[2] = {}, g_stale[2] = {};   // [0] the stream's program is the call's, [1] it is another
    std::map<uint32_t, std::set<uint64_t>> g_vertexHashes, g_pixelHashes;
    std::map<uint32_t, std::map<uint64_t, uint64_t>> g_pixelDetail;   // object: program: draws

    std::mutex g_mutex;
    std::map<uint32_t, uint64_t> g_counts[CallCount];   // by host thread id
    std::chrono::steady_clock::time_point g_lastReport = std::chrono::steady_clock::now();

    std::map<uint32_t, std::map<uint32_t, uint64_t>> g_devices;   // by thread: the devices its draws name

    void Count(Call call, uint32_t device = 0)
    {
        if (!ObservingNow()) return;
        const uint32_t thread = GetCurrentThreadId();
        std::lock_guard<std::mutex> lock(g_mutex);
        g_counts[call][thread]++;
        if (device != 0) g_devices[thread][device]++;
        const auto now = std::chrono::steady_clock::now();
        if (now - g_lastReport < std::chrono::seconds(5)) return;
        g_lastReport = now;
        printf("d3d hooks, the last five seconds:\n");
        for (int i = 0; i < CallCount; i++)
        {
            if (g_counts[i].empty()) continue;
            printf("  %-28s", CallNames[i]);
            for (const auto& [id, n] : g_counts[i]) printf(" thread %u x%llu", id, (unsigned long long)n);
            printf("\n");
            g_counts[i].clear();
        }
        for (const auto& [thread, devices] : g_devices)
        {
            printf("  thread %u draws on device", thread);
            for (const auto& [device, n] : devices) printf(" %08X x%llu", device, (unsigned long long)n);
            printf("\n");
        }
        g_devices.clear();
        {
            std::lock_guard<std::mutex> parity(g_parityMutex);
            size_t vertexMany = 0, pixelMany = 0;
            for (const auto& [object, hashes] : g_vertexHashes) if (hashes.size() > 1) vertexMany++;
            for (const auto& [object, hashes] : g_pixelHashes) if (hashes.size() > 1) pixelMany++;
            printf("  parity: %llu draws ran, %llu matched to a draw call (%llu of them with no pixel shader); "
                   "%zu vertex shader objects (%zu loaded as more than one program), %zu pixel shader objects (%zu more than one)\n",
                (unsigned long long)g_executed, (unsigned long long)g_matched, (unsigned long long)g_nullPixel,
                g_vertexHashes.size(), vertexMany, g_pixelHashes.size(), pixelMany);
            printf("  colour draws matched: %llu, run with their own pixel program: %llu, with another: %llu\n",
                (unsigned long long)g_colourDraws, (unsigned long long)g_ownProgram, (unsigned long long)(g_colourDraws - g_ownProgram));
            printf("  records fresh: %llu same program, %llu another; stale: %llu same, %llu another\n",
                (unsigned long long)g_fresh[0], (unsigned long long)g_fresh[1], (unsigned long long)g_stale[0], (unsigned long long)g_stale[1]);
            g_fresh[0] = g_fresh[1] = g_stale[0] = g_stale[1] = 0;
            g_executed = g_matched = g_nullPixel = g_colourDraws = g_ownProgram = 0;
            int shown = 0;
            for (const auto& [object, programs] : g_pixelDetail)
            {
                if (programs.size() < 2 || shown++ >= 6) continue;
                printf("    pixel shader object %08X:", object);
                for (const auto& [hash, n] : programs) printf(" %016llx x%llu", (unsigned long long)hash, (unsigned long long)n);
                printf("\n");
            }
            g_pixelDetail.clear();
        }
        fflush(stdout);
    }
}

#define COD3_OBSERVE(address, call)                     \
    extern "C" PPC_FUNC(__imp__sub_##address);          \
    PPC_FUNC(sub_##address)                             \
    {                                                   \
        Count(call);                                    \
        __imp__sub_##address(ctx, base);                \
    }

// With the decompiled D3D library (COD3_HAVE_DECOMP) its draws are not
// observed here: the decompiled ones take the place these hooks would.
// Neither is 822FB4B0 (the shader load), decompiled too, nor the swap
// countdown (822F4A10) or the draw of nothing (822F6858).
#ifndef COD3_HAVE_DECOMP
COD3_OBSERVE(822EFFF0, RectangleEvent)
COD3_OBSERVE(822F8590, IndirectB)
COD3_OBSERVE(822F4A10, SwapCountdown)
COD3_OBSERVE(822F6858, DrawImmediate)
#endif
COD3_OBSERVE(82154930, GameDraw)

// The draws, observed in both builds: each one's packets are recorded for
// the native state's check (native_state.cpp, COD3_NATIVECHECK=1). The body
// is the decompiled function's with the decompiled library
// (COD3_HAVE_DECOMP), the recompiled one's without.
#ifdef COD3_HAVE_DECOMP
#define COD3_DRAW_BODY(address) decomp_sub_##address
#define COD3_DRAW_DECLARE(address) PPC_FUNC(decomp_sub_##address)
#else
#define COD3_DRAW_BODY(address) __imp__sub_##address
#define COD3_DRAW_DECLARE(address) extern "C" PPC_FUNC(__imp__sub_##address)
#endif

#define COD3_DRAW(address, call)                                                       \
    COD3_DRAW_DECLARE(address);                                                        \
    PPC_FUNC(sub_##address)                                                            \
    {                                                                                  \
        const uint32_t device = ctx.r3.u32;                                            \
        const uint32_t before = Guest::Read32(base, device + 40);                      \
        Count(call, device);                                                           \
        COD3_DRAW_BODY(address)(ctx, base);                                            \
        NativeState::Recorded(base, device, before, Guest::Read32(base, device + 40)); \
    }

COD3_DRAW(822F3620, DrawIndexed)
COD3_DRAW(822F3EA0, DrawIndexedB)
COD3_DRAW(822F4338, DrawIndexedC)

// BeginVertices: its draw is written but the write pointer it leaves at
// +13076, for EndVertices to make the device's; none when there was no
// room for the vertices.
COD3_DRAW_DECLARE(822F30F0);
PPC_FUNC(sub_822F30F0)
{
    const uint32_t device = ctx.r3.u32;
    const uint32_t before = Guest::Read32(base, device + 40);
    Count(DrawD, device);
    COD3_DRAW_BODY(822F30F0)(ctx, base);
    if (ctx.r3.u32 != 0) NativeState::Recorded(base, device, before, Guest::Read32(base, device + 13076));
}

// A new segment of the command buffer (sub_822F2678(device, words), after
// a kick when the room asked for is more than a segment): the native
// state goes on from there.
COD3_DRAW_DECLARE(822F2678);
PPC_FUNC(sub_822F2678)
{
    const uint32_t device = ctx.r3.u32;
    COD3_DRAW_BODY(822F2678)(ctx, base);
    NativeState::Kicked(base, device);
}

// DrawIndexedPrimitive, the most of the draws: with COD3_CALLPS=1 or
// COD3_D3DHOOKS=1 its programs are kept too, for the first parity check.
COD3_DRAW_DECLARE(822F3A28);
PPC_FUNC(sub_822F3A28)
{
    const uint32_t device = ctx.r3.u32, count = ctx.r7.u32;
    const uint32_t before = Guest::Read32(base, device + 40);
    Count(DrawE, device);
    COD3_DRAW_BODY(822F3A28)(ctx, base);
    NativeState::Recorded(base, device, before, Guest::Read32(base, device + 40));
    if (!ObservingNow() && StreamPixelPrograms()) return;
    // The device's write pointer stands on the last word written: the
    // draw packet's last.
    const uint32_t last = Guest::Read32(base, device + 40) & 0x1FFFFFFFu;
    const uint32_t pixelObject = Guest::Read32(base, device + 12416);
    // The packet's last word as written, so a record left from an earlier
    // frame at the same address, for another draw, is not taken for this one.
    DrawRecord record{ Guest::Read32(base, device + 12420), pixelObject, count, 0, 0, 0, Guest::Read32(base, Guest::Read32(base, device + 40)) };
    if (PixelMicrocode(base, pixelObject, record.pixelPhysical, record.pixelDwords) && ObservingNow())
        record.pixelHash = RenderShaders::HashMicrocode(Guest::Base + record.pixelPhysical, record.pixelDwords);
    std::lock_guard<std::mutex> lock(g_recordMutex);
    if (g_records.size() > 200000) g_records.clear();
    g_records[last] = record;
}

bool D3dHooks::Observing() { return ObservingNow(); }

bool D3dHooks::PixelProgram(uint32_t lastWordPhysical, uint32_t& physical, uint32_t& dwords)
{
    if (StreamPixelPrograms()) return false;
    std::lock_guard<std::mutex> lock(g_recordMutex);
    auto it = g_records.find(lastWordPhysical & 0x1FFFFFFFu);
    if (it == g_records.end() || it->second.pixelObject == 0 || it->second.pixelDwords == 0) return false;
    if (Guest::Read32(Guest::Base, Guest::PhysicalAlias(lastWordPhysical)) != it->second.lastWordValue) return false;
    physical = it->second.pixelPhysical;
    dwords = it->second.pixelDwords;
    return true;
}

void D3dHooks::CheckDraw(uint32_t lastWordPhysical, uint64_t vertexHash, uint64_t pixelHash, uint32_t modeControl)
{
    DrawRecord record{};
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(g_recordMutex);
        auto it = g_records.find(lastWordPhysical & 0x1FFFFFFFu);
        if (it != g_records.end()) { record = it->second; found = true; }
    }
    std::lock_guard<std::mutex> lock(g_parityMutex);
    g_executed++;
    if (!found) return;
    g_matched++;
    g_vertexHashes[record.vertexObject].insert(vertexHash);
    if (record.pixelObject == 0) g_nullPixel++;
    // A depth only draw (mode 5) runs no pixel program, and the one loaded
    // may be an earlier draw's: only the colour draws say anything.
    else if ((modeControl & 7) == 4)
    {
        g_pixelHashes[record.pixelObject].insert(pixelHash);
        g_pixelDetail[record.pixelObject][pixelHash]++;
        g_colourDraws++;
        if (pixelHash == record.pixelHash) g_ownProgram++;
        const bool fresh = Guest::Read32(Guest::Base, Guest::PhysicalAlias(lastWordPhysical)) == record.lastWordValue;
        (fresh ? g_fresh : g_stale)[pixelHash == record.pixelHash ? 0 : 1]++;
    }
}
// With the decompiled D3D library (COD3_HAVE_DECOMP) the clear and the
// movie capture's frame are the decompiled ones.
#ifndef COD3_HAVE_DECOMP
COD3_OBSERVE(822F6DF8, DrawF)
COD3_OBSERVE(82301888, DrawG)
#endif
