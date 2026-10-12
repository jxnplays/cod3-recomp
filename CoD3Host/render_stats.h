#pragma once

// What the renderer did, counted per frame and averaged over the last
// second: for the COD3_RENDER_STATS=1 line, the overlay's panel and the
// comparison of one renderer against another.
//
// Counting is a relaxed add on the command thread. The GPU's time comes
// from timestamp queries around the frame, with COD3_GPU_PROFILE=1, read
// back a few frames later so nothing waits on the GPU.

#include <cstdint>

struct ID3D11Device;
struct ID3D11DeviceContext;

namespace RenderStats
{
    void Initialize(ID3D11Device* device, ID3D11DeviceContext* context);

    // The parts of the draw path, for the profile: where a draw's CPU
    // time goes, in cycles, with COD3_RENDER_PROFILE=1.
    enum Section : uint32_t
    {
        SectionState,        // the registers into the snapshot, the programs
        SectionTargets,      // the render targets and the scissor
        SectionPipeline,     // the blend, depth and rasteriser handles
        SectionTextures,     // the fetch constants into texture and sampler handles, the binds
        SectionBuffers,      // the vertex buffers
        SectionConstants,    // the constant files gathered and mapped into the ring
        SectionBind,         // the programs and the states set on the context
        SectionIndices,      // the indices turned round and appended
        SectionDraw,         // the draw call itself
        SectionLog,          // the frame log, when on
        SectionCount
    };
    const char* SectionName(Section section);

    // Per frame counters, added to as the frame is drawn.
    struct Counters
    {
        uint32_t draws = 0, skipped = 0, triangles = 0, resolves = 0;
        uint32_t shaderSwitches = 0, textureSwitches = 0, pipelineSwitches = 0, targetSwitches = 0;
        uint32_t textureUploads = 0, bufferUploads = 0;
        uint32_t constantUploads = 0;      // constant blocks through the ring
        uint32_t mergeable = 0;            // draws that could have joined the one before
        uint32_t fingerprints = 0;         // resources sampled for a change
        uint64_t fingerprintBytes = 0;     // the bytes those resources span
        uint32_t watchedLooks = 0;         // resources found the same by their pages (COD3_WRITEWATCH)
        uint64_t uploadBytes = 0;          // textures and vertex buffers
        uint64_t streamBytes = 0;          // indices and constants through the rings
        uint64_t drawNanoseconds = 0;      // CPU time inside the draw path
        uint64_t resolveNanoseconds = 0;
        uint64_t presentNanoseconds = 0;
        uint64_t sectionCycles[SectionCount] = {};
    };
    Counters& Frame();

    // COD3_RENDER_PROFILE=1: the sections are timed. The clock is the
    // cycle counter, read at each section's edge as the draw path passes
    // it: Enter says which section is running from here, Leave that none
    // is. Both do nothing when the profile is off.
    bool Profiled();
    void Enter(Section section);
    void Leave();

    // The frame's edges: the swap ends one and starts the next. Begin
    // issues the GPU timestamp, End the closing one and folds the frame's
    // counters into the running averages.
    void BeginFrame();
    void EndGpuFrame();   // before the present: the GPU's work for the frame ends here
    void EndFrame();

    // COD3_GPU_DRAWS=1: a GPU timestamp after every draw, the time since
    // the one before put down to the draw's key (its pixel program's hash;
    // 1 a resolve, 3 a draw with no pixel program; what follows the last
    // draw, the present's own work, to 2), and the keys that took the most
    // printed every five seconds, in milliseconds a frame.
    bool DrawsTimed();
    void DrawTimed(uint64_t key);

    // The averages of the last second, for showing.
    struct Snapshot
    {
        float fps = 0;
        float frameMilliseconds = 0;       // between swaps
        float cpuMilliseconds = 0;         // on the command thread, drawing
        float gpuMilliseconds = 0;         // between the frame's timestamps, when profiled
        float draws = 0, triangles = 0, skipped = 0, resolves = 0;
        float shaderSwitches = 0, textureSwitches = 0, pipelineSwitches = 0, targetSwitches = 0;
        float textureUploads = 0, bufferUploads = 0;
        float constantUploads = 0, mergeable = 0;
        float fingerprints = 0, fingerprintKilobytes = 0, watchedLooks = 0;
        float uploadKilobytes = 0, streamKilobytes = 0;
        float sectionMicroseconds[SectionCount] = {};   // a draw's, on average
        uint64_t frames = 0;
        bool gpuProfiled = false;
    };
    Snapshot Current();

    // A program compiled or read from the disk cache, from any thread:
    // the stats line carries the totals.
    void NoteProgram(bool fromDisk);

    bool Wanted();        // COD3_RENDER_STATS=1: a line a second on stdout
    bool GpuProfiled();   // COD3_GPU_PROFILE=1
    bool Debug();         // COD3_RENDER_DEBUG=1: the commands of a frame, once

    // The one second line, when wanted; called at the swap.
    void Tick();
}
