#include "render_stats.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

#include <d3d11_1.h>
#include <intrin.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace
{
    ID3D11Device* g_device = nullptr;
    ID3D11DeviceContext* g_context = nullptr;

    RenderStats::Counters g_frame;
    RenderStats::Counters g_second;      // summed over the second so far
    uint32_t g_secondFrames = 0;
    double g_secondGpuMilliseconds = 0;
    uint32_t g_secondGpuFrames = 0;
    std::chrono::steady_clock::time_point g_secondStarted = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point g_frameStarted = std::chrono::steady_clock::now();
    double g_secondFrameMilliseconds = 0;
    uint64_t g_frames = 0;

    std::mutex g_snapshotMutex;
    RenderStats::Snapshot g_snapshot;

    // The GPU timestamps: a disjoint query and two timestamps a frame, in
    // a ring of four so the read back is of a frame the GPU has finished.
    struct GpuFrame
    {
        ComPtr<ID3D11Query> disjoint, begin, end;
        bool issued = false;
        bool ended = false;
    };
    GpuFrame g_gpu[4];
    uint32_t g_gpuFrame = 0;
    bool g_gpuProfile = false;

    void ReadGpu(GpuFrame& frame)
    {
        if (!frame.issued) return;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
        UINT64 begin = 0, end = 0;
        // Flagged not to flush: a frame not done yet is simply not counted.
        if (g_context->GetData(frame.disjoint.Get(), &disjoint, sizeof(disjoint), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) return;
        if (g_context->GetData(frame.begin.Get(), &begin, sizeof(begin), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) return;
        if (g_context->GetData(frame.end.Get(), &end, sizeof(end), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) return;
        frame.issued = false;
        if (disjoint.Disjoint || disjoint.Frequency == 0 || end < begin) return;
        g_secondGpuMilliseconds += double(end - begin) * 1000.0 / double(disjoint.Frequency);
        g_secondGpuFrames++;
    }
}

namespace
{
    // The draws' timestamps, a ring of four frames read back four frames on.
    constexpr uint32_t DrawStamps = 8192;
    struct DrawFrame
    {
        ComPtr<ID3D11Query> disjoint;
        std::vector<ComPtr<ID3D11Query>> stamps;
        std::vector<uint64_t> keys;
        uint32_t count = 0;
        bool issued = false;
    };
    DrawFrame g_drawFrames[4];
    uint32_t g_drawFrame = 0;
    bool g_drawsTimed = false;
    struct DrawCost { double milliseconds = 0; uint64_t draws = 0; };
    std::map<uint64_t, DrawCost> g_drawCosts;
    uint32_t g_drawCostFrames = 0;
    std::chrono::steady_clock::time_point g_drawCostsSince = std::chrono::steady_clock::now();

    void ReadDraws(DrawFrame& frame)
    {
        if (!frame.issued) return;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
        if (g_context->GetData(frame.disjoint.Get(), &disjoint, sizeof(disjoint), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) return;
        std::vector<UINT64> times(frame.count);
        for (uint32_t i = 0; i < frame.count; i++)
            if (g_context->GetData(frame.stamps[i].Get(), &times[i], sizeof(UINT64), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) return;
        frame.issued = false;
        if (disjoint.Disjoint || disjoint.Frequency == 0) return;
        for (uint32_t i = 1; i < frame.count; i++)
        {
            if (times[i] < times[i - 1]) continue;
            DrawCost& cost = g_drawCosts[frame.keys[i]];
            cost.milliseconds += double(times[i] - times[i - 1]) * 1000.0 / double(disjoint.Frequency);
            cost.draws++;
        }
        g_drawCostFrames++;
        const auto now = std::chrono::steady_clock::now();
        if (now - g_drawCostsSince < std::chrono::seconds(5) || g_drawCostFrames == 0) return;
        std::vector<std::pair<uint64_t, DrawCost>> list(g_drawCosts.begin(), g_drawCosts.end());
        std::sort(list.begin(), list.end(), [](const auto& a, const auto& b) { return a.second.milliseconds > b.second.milliseconds; });
        double total = 0;
        for (const auto& [key, cost] : list) total += cost.milliseconds;
        const double frames = double(g_drawCostFrames);
        printf("gpu draws: %.2f ms a frame over %u frames; the most:\n", total / frames, g_drawCostFrames);
        int shown = 0;
        for (const auto& [key, cost] : list)
        {
            if (shown++ >= 30) break;
            char name[32];
            if (key == 1) snprintf(name, sizeof(name), "resolves");
            else if (key == 2) snprintf(name, sizeof(name), "present and after");
            else if ((key & 0x0000FFFFFFFFFFFFull) == 3) snprintf(name, sizeof(name), "no ps, %u wide", unsigned(key >> 48));
            else snprintf(name, sizeof(name), "ps %012llx, %u wide", (unsigned long long)(key & 0x0000FFFFFFFFFFFFull), unsigned(key >> 48));
            printf("  %-30s %6.3f ms (%4.1f%%), %6.1f draws a frame\n", name, cost.milliseconds / frames,
                total > 0 ? 100.0 * cost.milliseconds / total : 0.0, double(cost.draws) / frames);
        }
        fflush(stdout);
        g_drawCosts.clear();
        g_drawCostFrames = 0;
        g_drawCostsSince = now;
    }
}

void RenderStats::Initialize(ID3D11Device* device, ID3D11DeviceContext* context)
{
    g_device = device;
    g_context = context;
    g_gpuProfile = getenv("COD3_GPU_PROFILE") != nullptr;
    g_drawsTimed = getenv("COD3_GPU_DRAWS") != nullptr;
    for (DrawFrame& frame : g_drawFrames)
    {
        if (!g_drawsTimed) break;
        D3D11_QUERY_DESC desc{};
        desc.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
        device->CreateQuery(&desc, &frame.disjoint);
        desc.Query = D3D11_QUERY_TIMESTAMP;
        frame.stamps.resize(DrawStamps);
        frame.keys.resize(DrawStamps);
        for (auto& stamp : frame.stamps) device->CreateQuery(&desc, &stamp);
        if (!frame.disjoint) g_drawsTimed = false;
    }
    if (!g_gpuProfile) return;
    for (GpuFrame& frame : g_gpu)
    {
        D3D11_QUERY_DESC desc{};
        desc.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
        device->CreateQuery(&desc, &frame.disjoint);
        desc.Query = D3D11_QUERY_TIMESTAMP;
        device->CreateQuery(&desc, &frame.begin);
        device->CreateQuery(&desc, &frame.end);
        if (!frame.disjoint || !frame.begin || !frame.end) { g_gpuProfile = false; break; }
    }
}

RenderStats::Counters& RenderStats::Frame() { return g_frame; }

bool RenderStats::DrawsTimed() { return g_drawsTimed; }

void RenderStats::DrawTimed(uint64_t key)
{
    if (!g_drawsTimed) return;
    DrawFrame& frame = g_drawFrames[g_drawFrame % 4];
    if (!frame.issued || frame.count >= DrawStamps) return;
    g_context->End(frame.stamps[frame.count].Get());
    frame.keys[frame.count] = key;
    frame.count++;
}

const char* RenderStats::SectionName(Section section)
{
    static const char* const names[SectionCount] = { "state", "targets", "pipeline", "textures", "buffers", "constants", "bind", "indices", "draw", "log" };
    return section < SectionCount ? names[section] : "?";
}

bool RenderStats::Profiled()
{
    static const bool wanted = getenv("COD3_RENDER_PROFILE") != nullptr;
    return wanted;
}

namespace
{
    uint64_t g_sectionStarted = 0;
    RenderStats::Section g_section = RenderStats::SectionState;
}

void RenderStats::Enter(RenderStats::Section section)
{
    if (!Profiled()) return;
    const uint64_t now = __rdtsc();
    if (g_sectionStarted != 0) g_frame.sectionCycles[g_section] += now - g_sectionStarted;
    g_sectionStarted = now;
    g_section = section;
}

void RenderStats::Leave()
{
    if (g_sectionStarted == 0) return;
    g_frame.sectionCycles[g_section] += __rdtsc() - g_sectionStarted;
    g_sectionStarted = 0;
}

void RenderStats::BeginFrame()
{
    g_frameStarted = std::chrono::steady_clock::now();
    if (g_drawsTimed)
    {
        DrawFrame& frame = g_drawFrames[g_drawFrame % 4];
        ReadDraws(frame);
        if (!frame.issued)
        {
            g_context->Begin(frame.disjoint.Get());
            frame.issued = true;
            frame.count = 0;
            DrawTimed(0);   // the frame's start
        }
    }
    if (!g_gpuProfile) return;
    GpuFrame& frame = g_gpu[g_gpuFrame % 4];
    ReadGpu(frame);   // the one from four frames ago, done by now
    if (frame.issued) return;   // still not: skip this frame's query rather than wait
    g_context->Begin(frame.disjoint.Get());
    g_context->End(frame.begin.Get());
    frame.issued = true;
    frame.ended = false;
}

void RenderStats::EndGpuFrame()
{
    if (g_drawsTimed)
    {
        DrawFrame& frame = g_drawFrames[g_drawFrame % 4];
        if (frame.issued && frame.count > 0 && frame.keys[frame.count - 1] != 2)
        {
            DrawTimed(2);
            g_context->End(frame.disjoint.Get());
        }
    }
    if (!g_gpuProfile) return;
    GpuFrame& frame = g_gpu[g_gpuFrame % 4];
    if (!frame.issued || frame.ended) return;
    g_context->End(frame.end.Get());
    g_context->End(frame.disjoint.Get());
    frame.ended = true;
}

void RenderStats::EndFrame()
{
    const auto now = std::chrono::steady_clock::now();
    if (g_gpuProfile || g_drawsTimed) EndGpuFrame();
    if (g_gpuProfile) g_gpuFrame++;
    if (g_drawsTimed) g_drawFrame++;
    g_frames++;
    g_secondFrames++;
    g_secondFrameMilliseconds += std::chrono::duration<double, std::milli>(now - g_frameStarted).count();
    g_second.draws += g_frame.draws; g_second.skipped += g_frame.skipped; g_second.triangles += g_frame.triangles; g_second.resolves += g_frame.resolves;
    g_second.shaderSwitches += g_frame.shaderSwitches; g_second.textureSwitches += g_frame.textureSwitches;
    g_second.pipelineSwitches += g_frame.pipelineSwitches; g_second.targetSwitches += g_frame.targetSwitches;
    g_second.textureUploads += g_frame.textureUploads; g_second.bufferUploads += g_frame.bufferUploads;
    g_second.constantUploads += g_frame.constantUploads; g_second.mergeable += g_frame.mergeable;
    g_second.fingerprints += g_frame.fingerprints; g_second.fingerprintBytes += g_frame.fingerprintBytes;
    g_second.watchedLooks += g_frame.watchedLooks;
    g_second.uploadBytes += g_frame.uploadBytes; g_second.streamBytes += g_frame.streamBytes;
    for (uint32_t i = 0; i < SectionCount; i++) g_second.sectionCycles[i] += g_frame.sectionCycles[i];
    g_second.drawNanoseconds += g_frame.drawNanoseconds; g_second.resolveNanoseconds += g_frame.resolveNanoseconds; g_second.presentNanoseconds += g_frame.presentNanoseconds;
    g_frame = Counters();

    const double elapsed = std::chrono::duration<double>(now - g_secondStarted).count();
    if (elapsed < 1.0) return;
    Snapshot s;
    const float frames = float(g_secondFrames);
    s.fps = float(g_secondFrames / elapsed);
    s.frameMilliseconds = float(g_secondFrameMilliseconds / frames);
    s.cpuMilliseconds = float((g_second.drawNanoseconds + g_second.resolveNanoseconds + g_second.presentNanoseconds) / 1e6 / frames);
    s.gpuMilliseconds = g_secondGpuFrames != 0 ? float(g_secondGpuMilliseconds / g_secondGpuFrames) : 0.0f;
    s.gpuProfiled = g_gpuProfile;
    s.draws = g_second.draws / frames; s.triangles = g_second.triangles / frames; s.skipped = g_second.skipped / frames; s.resolves = g_second.resolves / frames;
    s.shaderSwitches = g_second.shaderSwitches / frames; s.textureSwitches = g_second.textureSwitches / frames;
    s.pipelineSwitches = g_second.pipelineSwitches / frames; s.targetSwitches = g_second.targetSwitches / frames;
    s.textureUploads = g_second.textureUploads / frames; s.bufferUploads = g_second.bufferUploads / frames;
    s.constantUploads = g_second.constantUploads / frames; s.mergeable = g_second.mergeable / frames;
    s.fingerprints = g_second.fingerprints / frames; s.fingerprintKilobytes = float(g_second.fingerprintBytes / 1024.0 / frames);
    s.watchedLooks = g_second.watchedLooks / frames;
    s.uploadKilobytes = float(g_second.uploadBytes / 1024.0 / frames); s.streamKilobytes = float(g_second.streamBytes / 1024.0 / frames);
    s.frames = g_frames;
    if (Profiled())
    {
        // Cycles into time by the draw path's own clock: the sections
        // together are what drawNanoseconds measured.
        uint64_t cycles = 0;
        for (uint32_t i = 0; i < SectionCount; i++) cycles += g_second.sectionCycles[i];
        const double draws = std::max(1.0, double(g_second.draws + g_second.skipped));
        const double perCycle = cycles != 0 ? double(g_second.drawNanoseconds) / double(cycles) : 0.0;
        for (uint32_t i = 0; i < SectionCount; i++) s.sectionMicroseconds[i] = float(g_second.sectionCycles[i] * perCycle / 1000.0 / draws);
    }
    {
        std::lock_guard<std::mutex> lock(g_snapshotMutex);
        g_snapshot = s;
    }
    g_second = Counters();
    g_secondFrames = 0;
    g_secondFrameMilliseconds = 0;
    g_secondGpuMilliseconds = 0;
    g_secondGpuFrames = 0;
    g_secondStarted = now;
    Tick();
}

RenderStats::Snapshot RenderStats::Current()
{
    std::lock_guard<std::mutex> lock(g_snapshotMutex);
    return g_snapshot;
}

namespace { std::atomic<uint32_t> g_compiled{ 0 }, g_fromDisk{ 0 }; }

void RenderStats::NoteProgram(bool fromDisk) { (fromDisk ? g_fromDisk : g_compiled).fetch_add(1, std::memory_order_relaxed); }

bool RenderStats::Wanted()
{
    static const bool wanted = getenv("COD3_RENDER_STATS") != nullptr;
    return wanted;
}

bool RenderStats::GpuProfiled() { return g_gpuProfile; }

bool RenderStats::Debug()
{
    static const bool wanted = getenv("COD3_RENDER_DEBUG") != nullptr;
    return wanted;
}

void RenderStats::Tick()
{
    if (!Wanted()) return;
    const Snapshot s = Current();
    printf("stats: %.1f fps, frame %.2f ms, cpu %.2f ms%s%.2f ms, %.0f draws (%.0f skipped), %.0f k triangles, %.0f resolves, "
           "switches: %.0f shader %.0f texture %.0f pipeline %.0f target; uploads %.0f tex %.0f buf (%.0f KB), streamed %.0f KB; programs %u compiled %u from disk\n",
        s.fps, s.frameMilliseconds, s.cpuMilliseconds, s.gpuProfiled ? ", gpu " : ", gpu n/a ", s.gpuMilliseconds,
        s.draws, s.skipped, s.triangles / 1000.0f, s.resolves,
        s.shaderSwitches, s.textureSwitches, s.pipelineSwitches, s.targetSwitches,
        s.textureUploads, s.bufferUploads, s.uploadKilobytes, s.streamKilobytes,
        g_compiled.load(std::memory_order_relaxed), g_fromDisk.load(std::memory_order_relaxed));
    if (Profiled())
    {
        float total = 0;
        for (uint32_t i = 0; i < SectionCount; i++) total += s.sectionMicroseconds[i];
        printf("profile: %.2f us a draw:", total);
        for (uint32_t i = 0; i < SectionCount; i++) printf(" %s %.2f", SectionName(Section(i)), s.sectionMicroseconds[i]);
        printf("; %.0f constant blocks, %.0f mergeable draws, %.0f fingerprints over %.0f KB a frame, %.0f found the same by their pages\n", s.constantUploads, s.mergeable, s.fingerprints, s.fingerprintKilobytes, s.watchedLooks);
    }
    fflush(stdout);
}
