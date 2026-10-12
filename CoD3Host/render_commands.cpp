// The PC render layer: registers in, commands out. See render_commands.h.
//
// A draw is prepared in the order the profile names its sections: the
// registers into a snapshot and the programs looked up, the targets and
// the scissor, the pipeline handles, the textures, the vertex buffers, the
// constants gathered into the ring, the indices turned round into theirs.
// Nothing here calls the Direct3D context except through the resource
// layer's rings and caches, and nothing allocates on a draw that has been
// seen before.

#include "render_commands.h"
#include "render_internal.h"
#include "render_pipeline.h"
#include "render_shaders.h"
#include "render_resources.h"
#include "render_stats.h"
#include "gpu.h"
#include "kernel.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <tuple>
#include <vector>

#include <d3d11_1.h>
#include <emmintrin.h>

using RenderState::Handle;
using RenderState::DrawCommand;
using RenderState::ResolveCommand;

namespace
{
    // --- what a draw was not made for ---------------------------------------------------------------

    enum SkipReason { SkipBackendOff, SkipPrimitive, SkipProgram, SkipProgramBuilding, SkipProgramOnRequest, SkipPitch, SkipTarget, SkipScissor, SkipBuffer, SkipAimBlur, SkipCount };
    const char* const SkipNames[SkipCount] = { "render backend off", "primitive type", "program", "program building", "program skipped on request", "no surface pitch", "no target", "empty scissor", "vertex buffer", "aim blur off" };

    // The title's depth of field while aiming: the frame downsampled to a
    // quarter (10e0411b), blurred (0925661f), and blended back over the
    // frame by depth (56ccd878), the weapon nearer than the plane staying
    // sharp. When the settings say, the last of the three is left out and
    // the frame stays as it was before it: the scene and the weapon,
    // sharp. The two before it still run, so the surface they make is
    // there for anything else that reads it.
    bool g_aimBlur = true;
    bool IsAimBlurProgram(uint64_t hash)
    {
        return hash == 0x56ccd878c09e55bbull;
    }
    uint64_t g_skips[SkipCount] = {};

    void Skip(SkipReason reason)
    {
        g_skips[reason]++;
        RenderStats::Frame().skipped++;
    }

    // COD3_SKIPLOG=1: each frame whose skips, reason by reason, are not the
    // frame before's, for finding a draw that goes missing for one frame.
    void SkipFrameCheck()
    {
        static const bool log = getenv("COD3_SKIPLOG") != nullptr;
        if (!log) return;
        static uint64_t lastSwap = 0, atStart[SkipCount] = {}, previous[SkipCount] = {};
        const uint64_t swap = RenderInternal::Swaps();
        if (swap == lastSwap) return;
        uint64_t frame[SkipCount];
        bool differs = false;
        for (int i = 0; i < SkipCount; i++) { frame[i] = g_skips[i] - atStart[i]; if (frame[i] != previous[i]) differs = true; }
        if (differs)
        {
            printf("render: skips in frame %llu:", (unsigned long long)lastSwap);
            for (int i = 0; i < SkipCount; i++) if (frame[i] != 0) printf(" %s x%llu;", SkipNames[i], (unsigned long long)frame[i]);
            printf("\n");
        }
        memcpy(previous, frame, sizeof(frame));
        memcpy(atStart, g_skips, sizeof(atStart));
        lastSwap = swap;
    }

    // --- the constants ----------------------------------------------------------------------------------

    // The draw's own block, kept from one draw to the next so it goes up
    // only when it changed.
    RenderState::DrawConstants g_constants{};
    RenderState::DrawConstants g_lastConstants{};

    // What has been written to the float files and not yet uploaded, in
    // float4s, per stage.
    uint32_t g_pendingFirst[2] = { 0, 0 }, g_pendingEnd[2] = { 256, 256 };

    // What is in the ring: where each stage's file went last, packed for
    // which program, and where the draw block went. Invalid after the ring
    // wrapped, since a discard threw the old contents away.
    struct Uploaded
    {
        uint32_t floatAt[2] = { 0xFFFFFFFFu, 0xFFFFFFFFu };
        uint32_t floatCount[2] = { 0, 0 };
        Handle floatProgram[2] = { 0, 0 };
        uint32_t drawAt = 0xFFFFFFFFu;
    };
    Uploaded g_uploaded;

    // Without the ring, the files go whole: the scratch the command points at.
    uint32_t g_floatFile[2][1024];

    // From a draw's record: the file as it was when it last went up, by
    // stage, and whether a program's constants differ from it now.
    uint32_t g_sentFile[2][1024];
    // COD3_CONSTCHECK: what each stage's last upload packed.
    std::vector<uint32_t> g_packedShadow[2];
    bool RecordChanged(uint32_t stage, const std::vector<uint16_t>& map, const uint32_t* file)
    {
        const uint32_t* now = file + (stage == 0 ? 0x4000 : 0x4400);
        if (map.empty()) return memcmp(g_sentFile[stage], now, 4096) != 0;
        size_t i = 0;
        while (i < map.size())
        {
            size_t run = 1;
            while (i + run < map.size() && map[i + run] == map[i] + run) run++;
            if (memcmp(g_sentFile[stage] + map[i] * 4u, now + map[i] * 4u, run * 16) != 0) return true;
            i += run;
        }
        return false;
    }
    uint32_t g_bools[40];
    bool g_boolsUploaded = false;

    // --- scratch for the indices ------------------------------------------------------------------------

    // Kept between draws: no allocation once it has grown to the largest draw.
    std::vector<uint32_t> g_indices32;

    inline uint32_t BSwap16(uint16_t v) { return uint16_t((v >> 8) | (v << 8)); }

    // Sixteen bit words turned round, eight at a time.
    void Swap16Into(uint16_t* to, const uint16_t* from, uint32_t count)
    {
        uint32_t i = 0;
        for (; i + 8 <= count; i += 8)
        {
            __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(from + i));
            v = _mm_or_si128(_mm_slli_epi16(v, 8), _mm_srli_epi16(v, 8));
            _mm_storeu_si128(reinterpret_cast<__m128i*>(to + i), v);
        }
        for (; i < count; i++) to[i] = uint16_t(BSwap16(from[i]));
    }

    // Thirty two bit words turned round, four at a time.
    void Swap32Into(uint32_t* to, const uint32_t* from, uint32_t count)
    {
        uint32_t i = 0;
        for (; i + 4 <= count; i += 4)
        {
            __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(from + i));
            v = _mm_or_si128(_mm_slli_epi16(v, 8), _mm_srli_epi16(v, 8));
            v = _mm_shufflelo_epi16(v, _MM_SHUFFLE(2, 3, 0, 1));
            v = _mm_shufflehi_epi16(v, _MM_SHUFFLE(2, 3, 0, 1));
            _mm_storeu_si128(reinterpret_cast<__m128i*>(to + i), v);
        }
        for (; i < count; i++) to[i] = _byteswap_ulong(from[i]);
    }

    // --- the pieces of a draw -----------------------------------------------------------------------------

    // The programs: the stage's current ones, ready or not. False when the
    // draw cannot be made with them, the skip counted.
    bool Programs(DrawCommand& out, const RenderShaders::Program*& vs, const RenderShaders::Program*& ps)
    {
        out.vertexShader = RenderShaders::Current(false);
        out.pixelShader = RenderShaders::Current(true);
        vs = RenderShaders::Get(out.vertexShader);
        ps = RenderShaders::Get(out.pixelShader);
        if (vs == nullptr || ps == nullptr) { Skip(SkipProgram); return false; }
        const RenderShaders::State vsState = vs->state.load(std::memory_order_acquire);
        const RenderShaders::State psState = ps->state.load(std::memory_order_acquire);
        if (vsState != RenderShaders::State::Ready || psState != RenderShaders::State::Ready)
        {
            // A program that failed is said the first time a draw wants it:
            // that is what a missing thing on screen comes from.
            for (const RenderShaders::Program* program : { vs, ps })
            {
                if (program->state.load(std::memory_order_acquire) != RenderShaders::State::Failed || program->announced) continue;
                const_cast<RenderShaders::Program*>(program)->announced = true;
                printf("render: a draw wants %s program %016llx, which could not be built: %s\n",
                    program->pixel ? "pixel" : "vertex", (unsigned long long)program->hash, program->problem.empty() ? "the compile failed" : program->problem.c_str());
                fflush(stdout);
            }
            Skip(vsState == RenderShaders::State::Failed || psState == RenderShaders::State::Failed ? SkipProgram : SkipProgramBuilding);
            return false;
        }
        if (vs->skipped) { Skip(SkipProgramOnRequest); return false; }
        if (!g_aimBlur && IsAimBlurProgram(ps->hash)) { Skip(SkipAimBlur); return false; }
        out.vs = static_cast<ID3D11VertexShader*>(vs->shader);
        out.ps = static_cast<ID3D11PixelShader*>(ps->shader);
        return true;
    }

    // The targets the pixel program writes and the surface's scissor, in
    // host pixels. False when there is nowhere to draw.
    // The EDRAM's depth only mode, RB_MODECONTROL 5: the hardware writes
    // depth and runs no pixel program at all, so the title does not load
    // one for such a draw and leaves the last one in place. The shadow maps
    // are drawn so: the soldiers' casters followed a clear in one cascade
    // and inherited its program (nothing discarded, a whole shadow), and
    // followed the alpha tested casters in another and inherited theirs,
    // which discards by a texture the soldiers' draw never meant for it,
    // and which draws came before them changed from frame to frame with
    // what each cascade held: the soldiers' shadows came and went. The
    // depth pre-pass (select 0x80000001) uses the mode too, with the colour
    // mask left open. Here such a draw gets no pixel program and no colour
    // target. COD3_DEPTHMODEPS=1 runs the program as before.
    bool DepthOnlyMode(const RenderState::Snapshot& state)
    {
        static const bool runProgram = getenv("COD3_DEPTHMODEPS") != nullptr;
        return state.modeControl == 5 && !runProgram;
    }
    constexpr RenderState::Handle NoPixelProgram = 0xFFFFFFFFu;

    bool Targets(const RenderState::Snapshot& state, const RenderShaders::Program* vs, const RenderShaders::Program* ps, DrawCommand& out, float& scaleY)
    {
        // The rows the scissor reaches, so the surfaces are tall enough
        // before they are looked up.
        {
            const uint32_t tl = state.scissorTopLeft, br = state.scissorBottomRight;
            const int32_t offsetY = (tl & 0x80000000u) ? 0 : (int32_t(state.windowOffset << 1) >> 17);
            const int32_t bottom = int32_t((br >> 16) & 0x7FFF) + offsetY;
            if (bottom > 0 && RenderResources::EnsureRows(state.pitch, uint32_t(bottom))) out.bindingsLost = true;
        }

        // Only the targets the pixel program writes, each once: the title
        // leaves the other target registers pointing at the same tiles, and
        // the host refuses one surface bound twice.
        const bool depthOnly = DepthOnlyMode(state);
        for (uint32_t i = 0; i < 4 && !depthOnly; i++)
        {
            if (((ps->colourTargets >> i) & 1) == 0) continue;
            if (((state.colorMask >> (i * 4)) & 0xF) == 0) continue;
            const RenderResources::ColorTarget* target = RenderResources::ColorTargetOf(RenderResources::ColorTargetFor(state.colorInfo[i], state.pitch));
            if (target == nullptr) continue;
            bool duplicate = false;
            for (uint32_t j = 0; j < i; j++) if (out.targets[j] == target->view) duplicate = true;
            if (duplicate) continue;
            out.targets[i] = target->view;
            out.targetResources[i] = target->resource;
            if (i == 0) out.colorTexture = target->texture;
            out.targetCount = i + 1;
        }
        if ((state.depthControl & 3) != 0)
        {
            if (const RenderResources::DepthTarget* depth = RenderResources::DepthTargetOf(RenderResources::DepthTargetFor(state.depthInfo, state.pitch)))
            {
                out.depthView = depth->view;
                out.depthResource = depth->resource;
                out.depthTexture = depth->texture;
            }
        }
        if (out.targetCount == 0 && out.depthView == nullptr)
        {
            // The first few, with their state: what a draw with nowhere to
            // draw is asking for.
            static int announced = 0;
            if (announced++ < 6)
            {
                printf("render: draw with no target: primitive %u, %u indices, mode %u, colour mask %08X, depth control %08X, targets written %X, colour info %08X, depth info %08X, vs %016llx ps %016llx\n",
                    state.primitive, state.indexCount, state.modeControl, state.colorMask, state.depthControl, ps->colourTargets, state.colorInfo[0], state.depthInfo,
                    (unsigned long long)vs->hash, (unsigned long long)ps->hash);
                fflush(stdout);
            }
            Skip(SkipTarget);
            return false;
        }

        // The target's host size, and the scissor in it. Bit 31 of the
        // scissor says the window offset does not apply.
        float scaleX;
        RenderResources::TargetSize(state.pitch, 4, out.targetWidth, out.targetHeight, scaleX, scaleY);
        {
            const uint32_t tl = state.scissorTopLeft, br = state.scissorBottomRight;
            const int32_t offsetX = (tl & 0x80000000u) ? 0 : (int32_t(state.windowOffset << 17) >> 17);
            const int32_t offsetY = (tl & 0x80000000u) ? 0 : (int32_t(state.windowOffset << 1) >> 17);
            out.scissor[0] = std::max(0, int(std::lround((int(tl & 0x7FFF) + offsetX) * scaleX)));
            out.scissor[1] = std::max(0, int(std::lround((int((tl >> 16) & 0x7FFF) + offsetY) * scaleY)));
            out.scissor[2] = std::min(int(out.targetWidth), int(std::lround((int(br & 0x7FFF) + offsetX) * scaleX)));
            out.scissor[3] = std::min(int(out.targetHeight), int(std::lround((int((br >> 16) & 0x7FFF) + offsetY) * scaleY)));
            if (out.scissor[2] <= out.scissor[0] || out.scissor[3] <= out.scissor[1]) { Skip(SkipScissor); return false; }
        }
        return true;
    }

    // A view that is not of the fetch's kind, said once each: a compressed
    // or 8 bit texture that reads as a surface a resolve made (a depth copy,
    // a shadow map, a copy of the frame) draws as a flat red or black
    // shape, which is how foliage was once seen to go.
    void CheckView(ID3D11ShaderResourceView* view, const uint32_t words[6], bool resolved, Handle texture, uint32_t slot, int stage)
    {
        if (view == nullptr || (!resolved && texture == RenderResources::WhiteTexture())) return;
        const uint32_t format = words[1] & 0x3F;
        DXGI_FORMAT expected;
        switch (format)
        {
        case 2: expected = DXGI_FORMAT_R8_UNORM; break;
        case 6: if (resolved) return; expected = DXGI_FORMAT_R8G8B8A8_UNORM; break;
        case 18: expected = DXGI_FORMAT_BC1_UNORM; break;
        case 19: expected = DXGI_FORMAT_BC2_UNORM; break;
        case 20: expected = DXGI_FORMAT_BC3_UNORM; break;
        default: return;
        }
        D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
        view->GetDesc(&desc);
        if (desc.Format == expected && !resolved) return;
        static std::atomic<int> said{ 0 };
        static uint64_t last = 0;
        const uint64_t what = (uint64_t(words[1]) << 32) ^ (uint64_t(slot) << 8) ^ uint64_t(desc.Format);
        if (what == last || said.fetch_add(1) >= 100) return;
        last = what;
        printf("render: frame %llu, %s texture %u, format %u at %08X (%ux%u), is bound to a view of DXGI format %u%s\n",
            (unsigned long long)RenderInternal::Swaps(), stage == 0 ? "pixel" : "vertex", slot, format, words[1] & 0xFFFFF000u,
            (words[2] & 0x1FFF) + 1, ((words[2] >> 13) & 0x1FFF) + 1, unsigned(desc.Format), resolved ? ", a resolved surface" : "");
        fflush(stdout);
    }

    // The textures and samplers the programs sample: the pixel program's,
    // and the vertex program's (the terrain's height map).
    void Textures(const RenderShaders::Program* vs, const RenderShaders::Program* ps, DrawCommand& out, RenderState::DrawConstants& constants)
    {
        for (int stage = 0; stage < 2; stage++)
        {
            const RenderShaders::Program* program = stage == 0 ? ps : vs;
            for (const XenosHlsl::TextureFetch& fetch : program->textureFetches)
            {
                if (out.textureCount == DrawCommand::MaxTextures) return;
                uint32_t words[6];
                RenderState::ReadTextureFetch(fetch.slot, words);
                uint32_t width = 1, height = 1;
                bool resolved = false;
                const Handle texture = RenderResources::TextureFor(words, width, height, resolved);
                DrawCommand::Texture& binding = out.textures[out.textureCount++];
                binding.stage = uint8_t(stage);
                binding.slot = uint8_t(fetch.slot);
                binding.samplerSlot = uint8_t(fetch.sampler);
                if (resolved) binding.view = RenderResources::ResolvedAt(words[1] & 0xFFFFF000u)->resource;
                else binding.view = RenderResources::TextureView(texture, fetch.dimension);
                CheckView(binding.view, words, resolved, texture, fetch.slot, stage);
                binding.sampler = RenderPipeline::SamplerObject(RenderPipeline::Sampler(words));
                constants.textureSize[fetch.slot][0] = float(width);
                constants.textureSize[fetch.slot][1] = float(height);
                constants.textureSize[fetch.slot][2] = 1.0f / float(width);
                constants.textureSize[fetch.slot][3] = 1.0f / float(height);
                // A surface the resolve made is sampled by the title with red
                // and blue swapped: the resolve wrote the console's memory in
                // one byte order and the fetch reads it in another, and the
                // swizzle in the fetch constant undoes that. The surfaces
                // here never go through memory, so the swap is left out.
                uint32_t swizzle = (words[3] >> 1) & 0xFFF;
                if (resolved && swizzle == 0x60A && (words[1] & 0x3F) == 6) swizzle = 0x688;
                constants.textureAdjustment[fetch.slot][0] = swizzle;
                constants.textureAdjustment[fetch.slot][1] = (words[0] >> 2) & 0xFF;
            }
        }
    }

    // The vertex buffers the vertex program fetches. False when one could
    // not be had.
    bool Buffers(const RenderShaders::Program* vs, DrawCommand& out)
    {
        for (const XenosHlsl::VertexFetch& fetch : vs->vertexFetches)
        {
            uint32_t word0, word1;
            RenderState::ReadVertexFetch(fetch.slot, word0, word1);
            // The programs turn every word round on the way in, which is the
            // console's usual byte order for a buffer; another would need a
            // conversion on upload, and is noted if it ever comes.
            if ((word1 & 3) != 2)
            {
                static int announced = 0;
                if (announced++ < 4) printf("render: a vertex buffer in byte order %u, drawn as order 2\n", word1 & 3);
            }
            const Handle buffer = RenderResources::VertexBufferFor(word0 & ~3u, ((word1 >> 2) & 0xFFFFFF) * 4, 2);
            ID3D11ShaderResourceView* view = RenderResources::VertexBufferView(buffer);
            if (view == nullptr || out.bufferCount == DrawCommand::MaxBuffers) { Skip(SkipBuffer); return false; }
            DrawCommand::Buffer& binding = out.buffers[out.bufferCount++];
            binding.slot = uint8_t(fetch.slot);
            binding.view = view;
        }
        return true;
    }

    // The float files, packed to what the programs read, and the draw's own
    // block, into the ring; the booleans when they changed. A file goes up
    // when something the program reads was written since it last went up,
    // and only as much of it as the program reads: the title writes a few
    // constants before most draws, and the whole four kilobytes a draw was
    // most of what a draw cost. The blocks go in one map, bound at offsets.
    // False when the ring could not be mapped.
    bool Constants(const RenderShaders::Program* vs, const RenderShaders::Program* ps, DrawCommand& out, const RenderState::DrawConstants& constants)
    {
        const uint32_t* file = RenderState::Registers();
        // From the register file, what was written since is what the stream
        // says; from a draw's record, what differs from what went up.
        const bool record = RenderState::UsingRecord();
        for (uint32_t range = 0; range < 2; range++)
        {
            uint32_t spanFirst, spanEnd;
            if (!Gpu::TakeConstantSpan(range, spanFirst, spanEnd)) continue;
            g_pendingFirst[range] = std::min(g_pendingFirst[range], spanFirst / 4);
            g_pendingEnd[range] = std::max(g_pendingEnd[range], (spanEnd + 3) / 4);
        }
        {
            uint32_t spanFirst, spanEnd;
            const bool written = Gpu::TakeConstantSpan(2, spanFirst, spanEnd);
            if (record ? memcmp(g_bools, file + 0x4900, sizeof(g_bools)) != 0 : written)
                g_boolsUploaded = false;
            if (!g_boolsUploaded)
            {
                memcpy(g_bools, file + 0x4900, sizeof(g_bools));
                out.bools = g_bools;
                out.boolsChanged = true;
                g_boolsUploaded = true;
            }
        }
        const bool drawChanged = memcmp(&g_lastConstants, &constants, sizeof(constants)) != 0;
        const RenderShaders::Program* programs[2] = { vs, ps };
        const Handle handles[2] = { out.vertexShader, out.pixelShader };
        out.ring = RenderResources::ConstantOffsetsAvailable();
        if (!out.ring)
        {
            // No offsets on this device: the whole files, in place.
            for (uint32_t stage = 0; stage < 2; stage++)
            {
                if (!record && g_pendingFirst[stage] >= 256 && g_uploaded.floatAt[stage] != 0xFFFFFFFFu) continue;
                const uint32_t first = stage == 0 ? 0x4000 : 0x4400;
                if (record && g_uploaded.floatAt[stage] != 0xFFFFFFFFu && memcmp(g_floatFile[stage], file + first, 4096) == 0) continue;
                memcpy(g_floatFile[stage], file + first, 4096);
                out.floatFile[stage] = g_floatFile[stage];
                out.floatChanged[stage] = true;
                g_uploaded.floatAt[stage] = 0;
                g_pendingFirst[stage] = 256; g_pendingEnd[stage] = 0;
                RenderStats::Frame().streamBytes += 4096;
            }
            if (drawChanged || g_uploaded.drawAt == 0xFFFFFFFFu)
            {
                g_lastConstants = constants;
                g_uploaded.drawAt = 0;
                out.drawConstants = &g_lastConstants;
                out.drawChanged = true;
            }
            return true;
        }

        constexpr uint32_t DrawBytes = (sizeof(constants) + 255) & ~255u;
        uint32_t need[2], bytes = 0;
        bool upload[2];
        for (uint32_t stage = 0; stage < 2; stage++)
        {
            const std::vector<uint16_t>& map = programs[stage]->constantMap;
            const uint32_t count = map.empty() ? 256 : uint32_t(map.size());
            need[stage] = std::min(256u, std::max(16u, (count + 15) & ~15u));
            // The file's span the program reads, for the dirty check.
            const uint32_t readFirst = map.empty() ? 0 : map.front(), readEnd = map.empty() ? 256 : map.back() + 1u;
            const bool dirty = record ? RecordChanged(stage, map, file) : g_pendingFirst[stage] < readEnd && g_pendingEnd[stage] > readFirst;
            upload[stage] = g_uploaded.floatAt[stage] == 0xFFFFFFFFu || g_uploaded.floatProgram[stage] != handles[stage] || dirty;
            // COD3_CONSTCHECK=1: a stage whose upload is skipped held against
            // what the upload before it packed: a constant that differs is
            // one the skip got wrong.
            static const bool constCheck = getenv("COD3_CONSTCHECK") != nullptr;
            if (constCheck && !upload[stage] && g_packedShadow[stage].size() == size_t(need[stage]) * 4)
            {
                const uint32_t* from = file + (stage == 0 ? 0x4000 : 0x4400);
                const size_t count = map.empty() ? 256 : map.size();
                for (size_t i = 0; i < count; i++)
                {
                    const uint32_t constant = map.empty() ? uint32_t(i) : map[i];
                    if (memcmp(&g_packedShadow[stage][i * 4], from + constant * 4, 16) == 0) continue;
                    static int told = 0;
                    if (told++ < 30)
                    {
                        printf("render: constant check: %s %016llx c%u was uploaded as %08X %08X %08X %08X, is %08X %08X %08X %08X now, the sent file says %08X; record %d\n",
                            stage == 0 ? "vs" : "ps", (unsigned long long)programs[stage]->hash, constant,
                            g_packedShadow[stage][i * 4], g_packedShadow[stage][i * 4 + 1], g_packedShadow[stage][i * 4 + 2], g_packedShadow[stage][i * 4 + 3],
                            from[constant * 4], from[constant * 4 + 1], from[constant * 4 + 2], from[constant * 4 + 3],
                            g_sentFile[stage][constant * 4], record ? 1 : 0);
                        fflush(stdout);
                    }
                    break;
                }
            }
            if (upload[stage]) bytes += need[stage] * 16;
        }
        bool drawUpload = drawChanged || g_uploaded.drawAt == 0xFFFFFFFFu;
        if (drawUpload) bytes += DrawBytes;
        if (bytes != 0)
        {
            if (RenderResources::ConstantReserve(need[0] * 16 + need[1] * 16 + DrawBytes))
            {
                // A wrap: everything remembered is in the old buffer.
                g_uploaded = Uploaded();
                upload[0] = upload[1] = drawUpload = true;
                bytes = need[0] * 16 + need[1] * 16 + DrawBytes;
            }
            uint32_t base;
            uint8_t* to = RenderResources::ConstantMap(bytes, base);
            if (to == nullptr) { Skip(SkipBuffer); return false; }
            uint32_t at = 0;
            for (uint32_t stage = 0; stage < 2; stage++)
            {
                if (!upload[stage]) continue;
                const uint32_t first = stage == 0 ? 0x4000 : 0x4400;
                uint32_t* words = reinterpret_cast<uint32_t*>(to + at);
                const std::vector<uint16_t>& map = programs[stage]->constantMap;
                // The file's words are written by this thread: plain copies.
                const uint32_t* from = reinterpret_cast<const uint32_t*>(file + first);
                // What was sent is kept for RecordChanged, which compares
                // only the constants this program reads: a change of program
                // uploads anyway, so the rest of the file need not be kept
                // (a whole 4 KB a draw was most of the command processor's
                // copying).
                if (map.empty())
                {
                    memcpy(words, from, 4096);
                    memcpy(g_sentFile[stage], from, 4096);
                }
                else
                {
                    // Runs of consecutive constants go as one copy.
                    size_t i = 0;
                    while (i < map.size())
                    {
                        size_t run = 1;
                        while (i + run < map.size() && map[i + run] == map[i] + run) run++;
                        memcpy(words + i * 4, from + map[i] * 4u, run * 16);
                        memcpy(g_sentFile[stage] + map[i] * 4u, from + map[i] * 4u, run * 16);
                        i += run;
                    }
                }
                // COD3_DUMPCONST=hash: what a program's constants hold,
                // once a second, to see whether the file that reaches it is
                // the file the title wrote.
                static const char* const dumpConst = getenv("COD3_DUMPCONST");
                char hashText[24] = {};
                if (dumpConst != nullptr) snprintf(hashText, sizeof(hashText), "%016llx", (unsigned long long)programs[stage]->hash);
                // Several hashes may be named, with commas; in a logged
                // frame every upload is printed, not one a second.
                if (dumpConst != nullptr && strstr(dumpConst, hashText) != nullptr)
                {
                    static uint64_t last = 0;
                    const uint64_t now = GetTickCount64() / 1000;
                    if (now != last || RenderInternal::FrameLogged())
                    {
                        last = now;
                        const float* values = reinterpret_cast<const float*>(words);
                        printf("const %016llx %u read: bools", (unsigned long long)programs[stage]->hash, unsigned(map.size()));
                        for (uint32_t i = 0; i < 8; i++) printf(" %08X", file[0x4900 + i]);
                        printf(" loops");
                        for (uint32_t i = 0; i < 32; i++) printf(" %08X", file[0x4908 + i]);
                        printf(" ::");
                        for (size_t i = 0; i < map.size(); i++)
                            printf(" c%u=(%.4g %.4g %.4g %.4g)", unsigned(map[i]),
                                values[i * 4], values[i * 4 + 1], values[i * 4 + 2], values[i * 4 + 3]);
                        printf("\n");
                        fflush(stdout);
                    }
                }
                static const bool constCheck = getenv("COD3_CONSTCHECK") != nullptr;
                if (constCheck) g_packedShadow[stage].assign(words, words + need[stage] * 4);
                g_uploaded.floatAt[stage] = base + at;
                g_uploaded.floatCount[stage] = need[stage];
                g_uploaded.floatProgram[stage] = handles[stage];
                at += need[stage] * 16;
                // Nothing is pending after an upload: a change of program
                // uploads anyway, and the same program only needs what is
                // written from here on.
                g_pendingFirst[stage] = 256; g_pendingEnd[stage] = 0;
            }
            if (drawUpload)
            {
                memcpy(to + at, &constants, sizeof(constants));
                g_lastConstants = constants;
                g_uploaded.drawAt = base + at;
                at += DrawBytes;
            }
            RenderResources::ConstantUnmap();
            RenderStats::Frame().streamBytes += at;
            RenderStats::Frame().constantUploads += (upload[0] ? 1 : 0) + (upload[1] ? 1 : 0) + (drawUpload ? 1 : 0);
        }
        out.floatAt[0] = g_uploaded.floatAt[0]; out.floatAt[1] = g_uploaded.floatAt[1];
        out.floatCount[0] = g_uploaded.floatCount[0]; out.floatCount[1] = g_uploaded.floatCount[1];
        out.drawAt = g_uploaded.drawAt;
        out.drawBytes = DrawBytes;
        return true;
    }

    // COD3_INDEXCHECK=1: an indexed draw whose indices reach outside the
    // hardware's clamp (VGT_MIN/MAX_VTX_INDX) or past the end of a vertex
    // buffer its program reads, said for each vertex program.
    void IndexCheck(const RenderState::Snapshot& state, const RenderShaders::Program* vs, const DrawCommand& out)
    {
        static const bool on = getenv("COD3_INDEXCHECK") != nullptr;
        if (!on || !out.indexed || state.sourceSelect != 0 || state.indexCount == 0) return;
        const uint8_t* data = Guest::Base + Guest::PhysicalAlias(state.indexBase);
        const bool strips = out.topology == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP || out.topology == D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP;
        const bool resetEnabled = strips && (state.suScModeControl & (1u << 21)) != 0;
        const uint32_t reset = state.resetIndex & (state.wideIndices ? 0xFFFFFFu : 0xFFFFu);
        uint32_t lowest = 0xFFFFFFFFu, highest = 0, resets = 0;
        for (uint32_t i = 0; i < state.indexCount; i++)
        {
            uint32_t index;
            if (state.wideIndices) { uint32_t w; memcpy(&w, data + i * 4, 4); index = _byteswap_ulong(w) & 0xFFFFFFu; }
            else { uint16_t h; memcpy(&h, data + i * 2, 2); index = BSwap16(h); }
            if (resetEnabled && index == reset) { resets++; continue; }
            lowest = std::min(lowest, index);
            highest = std::max(highest, index);
        }
        if (lowest > highest) return;
        uint32_t vertices = 0xFFFFFFFFu;
        for (const XenosHlsl::VertexFetch& fetch : vs->vertexFetches)
        {
            uint32_t word0, word1;
            RenderState::ReadVertexFetch(fetch.slot, word0, word1);
            if (fetch.stride != 0) vertices = std::min(vertices, ((word1 >> 2) & 0xFFFFFFu) / fetch.stride);
        }
        const int64_t first = int64_t(lowest) + state.indexOffset, last = int64_t(highest) + state.indexOffset;
        const uint32_t clampLow = state.minVertexIndex & 0xFFFFFFu, clampHigh = state.maxVertexIndex & 0xFFFFFFu;
        const bool past = last >= int64_t(vertices) || first < 0 || highest > clampHigh || lowest < clampLow;
        if (!past) return;
        static std::atomic<int> said{ 0 };
        static uint64_t lastHash = 0;
        if (vs->hash == lastHash || said.fetch_add(1) >= 60) return;
        lastHash = vs->hash;
        printf("render: frame %llu, vs %016llx: %u indices (%u resets) from %u to %u, offset %d, clamp %u..%u, the buffer has %u vertices\n",
            (unsigned long long)RenderInternal::Swaps(), (unsigned long long)vs->hash, state.indexCount, resets, lowest, highest,
            state.indexOffset, clampLow, clampHigh, vertices);
        fflush(stdout);
    }

    // The indices: the title's, byte swapped into the index ring, or none.
    // Quads and fans become triangle lists. The index that ends a strip and
    // starts the next, when the title has turned it on (PA_SU_SC_MODE_CNTL
    // bit 21), is the host's own cut index when it is all ones, and is
    // rewritten to it when it is not. False when the ring is full.
    bool Indices(const RenderState::Snapshot& state, bool convertQuads, bool convertFan, DrawCommand& out)
    {
        uint32_t indexCount = state.indexCount;
        const bool strips = out.topology == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP || out.topology == D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP;
        const bool resetEnabled = strips && (state.suScModeControl & (1u << 21)) != 0;
        const bool convert = convertQuads || convertFan;
        if (state.sourceSelect == 0 && !convert)
        {
            // Kept converted, while their pages say they are the same
            // (COD3_WRITEWATCH): nothing to turn round or stream.
            {
                const uint32_t reset = resetEnabled ? state.resetIndex & (state.wideIndices ? 0xFFFFFFu : 0xFFFFu) : ~0u;
                bool indices32 = false;
                if (ID3D11Buffer* kept = RenderResources::IndexBufferFor(state.indexBase, indexCount, state.wideIndices, reset, indices32))
                {
                    out.indexBuffer = kept;
                    out.indexRingOffset = 0;
                    out.indices32 = indices32;
                    out.indexed = true;
                    out.indexCount = indexCount;
                    return true;
                }
            }
            // The common case: turned round straight into the ring.
            const uint8_t* data = Guest::Base + Guest::PhysicalAlias(state.indexBase);
            if (state.wideIndices)
            {
                out.indices32 = true;
                uint32_t* to = reinterpret_cast<uint32_t*>(RenderResources::IndexMap(indexCount * 4, out.indexRingOffset));
                if (to == nullptr) { Skip(SkipBuffer); return false; }
                Swap32Into(to, reinterpret_cast<const uint32_t*>(data), indexCount);
                if (resetEnabled)
                {
                    const uint32_t reset = state.resetIndex & 0xFFFFFFu;
                    for (uint32_t i = 0; i < indexCount; i++) if ((to[i] & 0xFFFFFFu) == reset) to[i] = 0xFFFFFFFFu;
                }
                RenderResources::IndexUnmap();
                RenderStats::Frame().streamBytes += indexCount * 4;
            }
            else
            {
                const uint16_t* from = reinterpret_cast<const uint16_t*>(data);
                const uint32_t reset = state.resetIndex & 0xFFFFu;
                if (resetEnabled && reset != 0xFFFFu)
                {
                    // Widened, so the cut index can be the host's.
                    out.indices32 = true;
                    uint32_t* to = reinterpret_cast<uint32_t*>(RenderResources::IndexMap(indexCount * 4, out.indexRingOffset));
                    if (to == nullptr) { Skip(SkipBuffer); return false; }
                    for (uint32_t i = 0; i < indexCount; i++)
                    {
                        const uint32_t index = BSwap16(from[i]);
                        to[i] = index == reset ? 0xFFFFFFFFu : index;
                    }
                    RenderResources::IndexUnmap();
                    RenderStats::Frame().streamBytes += indexCount * 4;
                }
                else
                {
                    uint16_t* to = reinterpret_cast<uint16_t*>(RenderResources::IndexMap(indexCount * 2, out.indexRingOffset));
                    if (to == nullptr) { Skip(SkipBuffer); return false; }
                    Swap16Into(to, from, indexCount);
                    RenderResources::IndexUnmap();
                    RenderStats::Frame().streamBytes += indexCount * 2;
                }
            }
            out.indexed = true;
            out.indexCount = indexCount;
            return true;
        }
        if (!convert)
        {
            // A plain draw: the vertex ids are the indices.
            out.indexed = false;
            out.indexCount = indexCount;
            return true;
        }

        // Quads or a fan: the indices, or the vertex ids, as a triangle list.
        g_indices32.resize(indexCount);
        if (state.sourceSelect == 0)
        {
            const uint8_t* data = Guest::Base + Guest::PhysicalAlias(state.indexBase);
            if (state.wideIndices) Swap32Into(g_indices32.data(), reinterpret_cast<const uint32_t*>(data), indexCount);
            else
            {
                const uint16_t* from = reinterpret_cast<const uint16_t*>(data);
                for (uint32_t i = 0; i < indexCount; i++) g_indices32[i] = BSwap16(from[i]);
            }
        }
        else
            for (uint32_t i = 0; i < indexCount; i++) g_indices32[i] = i;
        static std::vector<uint32_t> list;
        list.clear();
        if (convertQuads)
            for (uint32_t i = 0; i + 3 < indexCount; i += 4)
            {
                const uint32_t q[6] = { g_indices32[i], g_indices32[i + 1], g_indices32[i + 2], g_indices32[i], g_indices32[i + 2], g_indices32[i + 3] };
                list.insert(list.end(), q, q + 6);
            }
        else
            for (uint32_t i = 1; i + 1 < indexCount; i++)
            {
                const uint32_t t[3] = { g_indices32[0], g_indices32[i], g_indices32[i + 1] };
                list.insert(list.end(), t, t + 3);
            }
        if (list.empty()) return false;
        const uint32_t bytes = uint32_t(list.size()) * 4;
        out.indexRingOffset = RenderResources::IndexAppend(list.data(), bytes);
        if (out.indexRingOffset == 0xFFFFFFFFu) { Skip(SkipBuffer); return false; }
        RenderStats::Frame().streamBytes += bytes;
        out.indexed = true;
        out.indices32 = true;
        out.indexCount = uint32_t(list.size());
        return true;
    }

    // The frame log's line for the draw, with its textures and buffers as
    // the registers name them.
    void Log(const RenderState::Snapshot& state, const RenderShaders::Program* vs, const RenderShaders::Program* ps, const DrawCommand& out)
    {
        printf("frame %llu (dump %d): draw %u of %u indices (%s), vte %03X viewport %g %g %g %g z %g %g, target %u %ux%u, scissor %d,%d-%d,%d, "
               "depth %08X blend %08X factor %g %g %g %g mask %X, mode %u, cull %08X, alpha %08X, vs %016llx ps %016llx\n",
            (unsigned long long)RenderInternal::Swaps(), RenderInternal::DumpNumber(), state.primitive, state.indexCount, out.indexed ? "indexed" : "plain",
            state.vteControl, state.viewport[0], state.viewport[1], state.viewport[2], state.viewport[3], state.viewport[4], state.viewport[5],
            out.targetCount, out.targetWidth, out.targetHeight, out.scissor[0], out.scissor[1], out.scissor[2], out.scissor[3],
            state.depthControl, state.blendControl[0], state.blendFactor[0], state.blendFactor[1], state.blendFactor[2], state.blendFactor[3], state.colorMask, state.modeControl, state.suScModeControl, state.colorControl,
            (unsigned long long)vs->hash, (unsigned long long)ps->hash);
        for (int stage = 0; stage < 2; stage++)
        {
            const RenderShaders::Program* program = stage == 0 ? ps : vs;
            for (const XenosHlsl::TextureFetch& fetch : program->textureFetches)
            {
                uint32_t w[6];
                RenderState::ReadTextureFetch(fetch.slot, w);
                uint32_t tw, th; bool tr;
                const bool white = RenderResources::TextureFor(w, tw, th, tr) == 1 && !tr;
                printf("   %stexture %u (read as %uD)%s%s: format %u %ux%u at %08X%s swizzle %03X signs %02X, words %08X %08X %08X %08X %08X %08X\n", stage == 0 ? "" : "vs ", fetch.slot, fetch.dimension == 3 ? 6 : fetch.dimension + 1, white ? " WHITE: " : "", white ? RenderResources::WhiteReason() : "", w[1] & 0x3F,
                    (w[2] & 0x1FFF) + 1, ((w[2] >> 13) & 0x1FFF) + 1, w[1] & 0xFFFFF000u, RenderResources::ResolvedAt(w[1] & 0xFFFFF000u) ? " (resolved)" : "",
                    (w[3] >> 1) & 0xFFF, (w[0] >> 2) & 0xFF, w[0], w[1], w[2], w[3], w[4], w[5]);
            }
        }
        for (const XenosHlsl::VertexFetch& fetch : vs->vertexFetches)
        {
            uint32_t word0, word1;
            RenderState::ReadVertexFetch(fetch.slot, word0, word1);
            printf("   vertex slot %u at %08X, %u dwords, endian %u, stride %u\n", fetch.slot, word0 & ~3u, (word1 >> 2) & 0xFFFFFF, word1 & 3, fetch.stride);
        }
        fflush(stdout);
    }
}

namespace
{
    // COD3_DRAWLOG=path: one line a draw, every frame, to that file: the
    // swap, the draw's number in it, the pass (bin select), the programs,
    // the primitive and count, the first vertex buffer with a sampled look
    // at its memory, a look at the indices and at the vertex program's
    // 256 constants, and the depth and blend state. Frames
    // are then compared draw by draw, to find what one frame did that its
    // neighbours did not.
    uint64_t SampleHash(uint32_t physical, uint32_t bytes)
    {
        if (physical == 0 || bytes == 0 || bytes > (64u << 20)) return 0;
        const uint8_t* data = Guest::Base + Guest::PhysicalAlias(physical);
        uint64_t hash = 1469598103934665603ull ^ bytes;
        const uint32_t step = std::max<uint32_t>(bytes / 64, 4) & ~3u;
        for (uint32_t at = 0; at + 4 <= bytes; at += step)
        {
            uint32_t word;
            memcpy(&word, data + at, 4);
            hash = (hash ^ word) * 1099511628211ull;
        }
        return hash;
    }

    void DrawLog(const RenderState::Snapshot& state, const RenderShaders::Program* vs, const RenderShaders::Program* ps, const DrawCommand& out)
    {
        static const char* const path = getenv("COD3_DRAWLOG");
        if (path == nullptr) return;
        // COD3_DRAWLOG_FROM / _UNTIL: only the swaps in that range. The lines
        // are kept in memory and written when the range is over, so the log
        // slows the frames it covers as little as it can.
        static const uint64_t from = []() { const char* t = getenv("COD3_DRAWLOG_FROM"); return t ? uint64_t(strtoull(t, nullptr, 10)) : 0ull; }();
        static const uint64_t until = []() { const char* t = getenv("COD3_DRAWLOG_UNTIL"); return t ? uint64_t(strtoull(t, nullptr, 10)) : ~0ull; }();
        static std::string lines;
        static bool written = false;
        const uint64_t swap = RenderInternal::Swaps();
        if (written) return;
        if (swap >= until || lines.size() > (512u << 20))
        {
            if (FILE* f = fopen(path, "wb")) { fwrite(lines.data(), 1, lines.size(), f); fclose(f); }
            printf("render: the draw log went to %s, %zu bytes\n", path, lines.size());
            fflush(stdout);
            lines.clear(); lines.shrink_to_fit();
            written = true;
            return;
        }
        if (swap < from) return;
        if (lines.empty()) lines.reserve(64u << 20);
        static uint64_t lastSwap = ~0ull;
        static uint32_t number = 0;
        if (swap != lastSwap) { lastSwap = swap; number = 0; }
        uint32_t vb = 0, vbBytes = 0;
        if (!vs->vertexFetches.empty())
        {
            uint32_t word0, word1;
            RenderState::ReadVertexFetch(vs->vertexFetches[0].slot, word0, word1);
            vb = word0 & ~3u;
            vbBytes = ((word1 >> 2) & 0xFFFFFF) * 4;
        }
        const std::atomic<uint32_t>* registers = Gpu::RegisterFile();
        uint64_t vsConstants = 1469598103934665603ull, psConstants = 1469598103934665603ull, textures = 1469598103934665603ull;
        for (uint32_t i = 0; i < 1024; i++) vsConstants = (vsConstants ^ registers[0x4000 + i].load(std::memory_order_relaxed)) * 1099511628211ull;
        for (uint32_t i = 0; i < 1024; i++) psConstants = (psConstants ^ registers[0x4400 + i].load(std::memory_order_relaxed)) * 1099511628211ull;
        for (const XenosHlsl::TextureFetch& fetch : ps->textureFetches)
        {
            uint32_t w[6];
            RenderState::ReadTextureFetch(fetch.slot, w);
            const uint32_t base = w[1] & 0xFFFFF000u;
            textures = (textures ^ (base | (RenderResources::ResolvedAt(base) ? 1u : 0u))) * 1099511628211ull;
        }
        // Every vertex stream the program reads, each hashed whole when it
        // is small: a second stream (a model's lighting, say) is as likely
        // to be what changes as the first.
        uint64_t streams = 1469598103934665603ull;
        for (const XenosHlsl::VertexFetch& fetch : vs->vertexFetches)
        {
            uint32_t word0, word1;
            RenderState::ReadVertexFetch(fetch.slot, word0, word1);
            const uint32_t at = word0 & ~3u, bytes = ((word1 >> 2) & 0xFFFFFF) * 4;
            uint64_t h = (at ^ 0x9E3779B97F4A7C15ull) * 1099511628211ull;
            if (at != 0 && bytes != 0 && bytes <= (256u << 10))
            {
                const uint8_t* data = Guest::Base + Guest::PhysicalAlias(at);
                for (uint32_t i = 0; i + 8 <= bytes; i += 8) { uint64_t w; memcpy(&w, data + i, 8); h = (h ^ w) * 1099511628211ull; }
            }
            else h ^= SampleHash(at, bytes);
            streams = (streams ^ h) * 1099511628211ull;
        }
        const uint32_t indexBytes = out.indexed ? state.indexCount * 4 : 0;
        char line[512];
        const int n = snprintf(line, sizeof(line), "%llu %u %llx %016llx %016llx p%u n%u %c vb %08X %u %016llx ib %016llx c %016llx d %08X b %08X m %X t %u pc %016llx tx %016llx vs %016llx %u\n",
            (unsigned long long)swap, number++, (unsigned long long)Gpu::BinSelect(), (unsigned long long)vs->hash, (unsigned long long)ps->hash,
            state.primitive, state.indexCount, out.indexed ? 'i' : 'p', vb, vbBytes, (unsigned long long)SampleHash(vb, vbBytes),
            (unsigned long long)(out.indexed ? SampleHash(state.indexBase, indexBytes) : 0), (unsigned long long)vsConstants,
            state.depthControl, state.blendControl[0], state.colorMask, out.targetCount, (unsigned long long)psConstants, (unsigned long long)textures,
            (unsigned long long)streams, unsigned(vs->vertexFetches.size()));
        if (n > 0) lines.append(line, size_t(n));
        // COD3_DRAWLOG_PSDUMP=hash: the first draw of a frame with that pixel
        // program adds its 256 pixel constants, every word, to the log.
        static const uint64_t psDump = []() { const char* t = getenv("COD3_DRAWLOG_PSDUMP"); return t ? uint64_t(strtoull(t, nullptr, 16)) : 0ull; }();
        static uint64_t dumpedSwap = ~0ull;
        if (psDump != 0 && ps->hash == psDump && dumpedSwap != swap)
        {
            dumpedSwap = swap;
            char word[16];
            lines += "PSDUMP " + std::to_string(swap);
            for (uint32_t i = 0; i < 1024; i++) { snprintf(word, sizeof(word), " %08X", registers[0x4400 + i].load(std::memory_order_relaxed)); lines += word; }
            lines += "\n";
        }
    }
}

bool RenderCommands::PrepareDraw(uint32_t initiator, uint32_t indexBase, DrawCommand& out)
{
    SkipFrameCheck();
    RenderStats::Enter(RenderStats::SectionState);
    RenderState::Snapshot state;
    RenderState::Read(initiator, indexBase, state);
    if (state.indexCount == 0) return false;
    if (state.modeControl == 0) { Skip(SkipBackendOff); return false; }
    // EDRAM mode 5 is drawn as colour and depth like mode 4: Xenia calls
    // it "depth only", but this title draws its ground, tents and jeeps
    // in it with programs that fetch the shadow maps.

    bool convertQuads = false, convertFan = false;
    switch (state.primitive)
    {
    case RenderState::Points:
    {
        // The host draws a point as one pixel; a wider one is noted.
        out.topology = D3D11_PRIMITIVE_TOPOLOGY_POINTLIST;
        static int announced = 0;
        if (((state.pointSize & 0xFFFF) > 8 || (state.pointSize >> 16) > 8) && announced++ < 4)
            printf("render: a point list of %u with a point size of %u by %u eighths of a pixel, drawn as pixels\n", state.indexCount, state.pointSize & 0xFFFF, state.pointSize >> 16);
        break;
    }
    case RenderState::Lines: out.topology = D3D11_PRIMITIVE_TOPOLOGY_LINELIST; break;
    case RenderState::LineStrip: out.topology = D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP; break;
    case RenderState::Triangles: out.topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST; break;
    case RenderState::TriangleFan: out.topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST; convertFan = true; break;
    case RenderState::TriangleStrip: out.topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP; break;
    case RenderState::Rectangles: out.topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST; out.rectangles = true; break;
    case RenderState::Quads: out.topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST; convertQuads = true; break;
    default: Skip(SkipPrimitive); return false;
    }

    const RenderShaders::Program* vs;
    const RenderShaders::Program* ps;
    if (!Programs(out, vs, ps)) return false;
    if (DepthOnlyMode(state)) { out.ps = nullptr; out.pixelShader = NoPixelProgram; }
    if (state.pitch == 0) { Skip(SkipPitch); return false; }

    RenderStats::Enter(RenderStats::SectionTargets);
    float scaleY = 1.0f;
    if (!Targets(state, vs, ps, out, scaleY)) return false;

    RenderStats::Enter(RenderStats::SectionPipeline);
    // The polygon offset. Its constant part (PA_SU_POLY_OFFSET_*_OFFSET) is
    // in depth units: the title writes 0.0001 for the layers it lays over a
    // surface (the mud, the ivy, the decals, drawn with a greater or equal
    // test and no depth write) and nought for the surfaces themselves, and
    // the console adds it to the depth as it is. The host's bias counts in
    // steps of the depth's own precision, which on a float depth buffer
    // shrinks with the depth: with this title's depth running from one near
    // to nought far, a layer fifty units away was lifted an eighth of what
    // it should, lost to its surface in patches, and drew as ragged holes.
    // So the constant part goes into the vertex program's depth (the w of
    // the viewport offset constant) and the rasterizer keeps the slope's.
    // COD3_OLDPOLYOFFSET=1 gives the host the constant part again.
    static const bool oldOffset = getenv("COD3_OLDPOLYOFFSET") != nullptr;
    static const bool noOffset = getenv("COD3_NOPOLYOFFSET") != nullptr;
    float depthOffset = 0.0f;
    float slopeOnly[4] = { state.polyOffset[0], 0.0f, state.polyOffset[2], 0.0f };
    if (!oldOffset && !noOffset)
    {
        if ((state.suScModeControl >> 11) & 1) depthOffset = state.polyOffset[1];
        else if ((state.suScModeControl >> 12) & 1) depthOffset = state.polyOffset[3];
    }
    // The Z pass (the depth only mode with its colour mask left open; the
    // shadow maps close theirs) is put a little behind. The colour pass
    // tests against it (greater or equal) and finds its own surfaces there
    // again, but with MSAA a translated pixel program gives one depth, cut
    // to the console's steps at the covered samples' centroid, to all of a
    // pixel's samples, where the Z pass has each sample's own: on steep
    // ground half a pixel's samples failed and kept an old frame's colour
    // (dotted lines along the triangles' edges; white speckles everywhere
    // without MSAA, where the cut alone loses to the Z pass's last bits),
    // and cutting the Z pass's depth the same way instead left whole
    // patches of a stale colour where the two passes' triangles cover a
    // pixel's samples differently. Behind by two pixels of slope and a few
    // steps, the Z pass still hides what is truly behind and never its own
    // surface. COD3_ZPASSBEHIND=0 leaves it where it is.
    static const bool behindOff = []() { const char* t = getenv("COD3_ZPASSBEHIND"); return t != nullptr && t[0] == '0'; }();
    const bool behind = !behindOff && DepthOnlyMode(state) && state.colorMask != 0;
    out.rasterizer = RenderPipeline::Rasterizer(state.suScModeControl, true, out.rectangles, oldOffset ? state.polyOffset : slopeOnly, behind);
    out.blend = RenderPipeline::Blend(state.blendControl, state.colorMask);
    out.depth = RenderPipeline::Depth(out.depthView ? state.depthControl : 0, state.stencilRefMask);
    memcpy(out.blendFactor, state.blendFactor, sizeof(out.blendFactor));
    // A factor that is the constant colour's alpha: Direct3D multiplies the
    // colour channels by the factor's rgb, so the alpha goes there. (The
    // title's bloom and its exposure pass blend by the constant alpha, and
    // with the rgb of a factor that is (0,0,0,1) they came out as nothing
    // at all.)
    if (RenderPipeline::BlendTakesConstantAlpha(state.blendControl))
        out.blendFactor[0] = out.blendFactor[1] = out.blendFactor[2] = state.blendFactor[3];
    out.stencilReference = state.stencilRefMask & 0xFF;
    // Only the opaque draws may go flat: a blended one in a flat colour
    // with an alpha of one would cover the frame.
    out.opaque = state.blendControl[0] == 0x00010001;

    // The draw's constants: the viewport the vertex program folds in, the
    // alpha test, and what the textures below fill in.
    RenderState::DrawConstants& constants = g_constants;
    constants.viewportScale[0] = state.viewport[0]; constants.viewportOffset[0] = state.viewport[1];
    constants.viewportScale[1] = state.viewport[2]; constants.viewportOffset[1] = state.viewport[3];
    constants.viewportScale[2] = state.viewport[4]; constants.viewportOffset[2] = state.viewport[5];
    constants.viewportOffset[3] = depthOffset;
    constants.targetSize[0] = float(state.pitch);
    constants.targetSize[1] = float(out.targetHeight) / scaleY;
    constants.targetSize[2] = 1.0f / float(state.pitch);
    constants.targetSize[3] = scaleY / float(out.targetHeight);
    // The viewport control, and in bit 16 where the console's pixel centres
    // are: at whole coordinates (PA_SU_VTX_CNTL bit 0 clear, as this title
    // has it), half a pixel up and left of the host's, so the vertex program
    // moves the geometry half a pixel the other way. COD3_NOHALFPIXEL=1 for
    // the host's centres, to compare.
    static const bool noHalfPixel = getenv("COD3_NOHALFPIXEL") != nullptr;
    constants.flags[0] = state.vteControl | (((state.vertexControl & 1) != 0 || noHalfPixel) ? 0x10000u : 0u);
    constants.flags[1] = ((state.colorControl >> 3) & 1) ? (state.colorControl & 7) : 7;   // the alpha test, or always
    constants.flags[2] = state.alphaReference;
    constants.flags[3] = uint32_t(ps->hash);
    // The pixel's own position, which the console writes into a register
    // of the pixel program when the program control asks (bit 18), the
    // register named by the context's bits 8 to 15. The soft particles
    // read the depth under them there; without it they read the corner
    // of the depth, and the smoke came and went with the tree's leaves.
    {
        uint32_t hostWidth, hostHeight;
        float scaleX, scaleYAgain;
        RenderResources::TargetSize(state.pitch, 4, hostWidth, hostHeight, scaleX, scaleYAgain);
        constants.pixelGen[0] = scaleX > 0.0f ? 1.0f / scaleX : 1.0f;
        constants.pixelGen[1] = scaleYAgain > 0.0f ? 1.0f / scaleYAgain : 1.0f;
        constants.pixelGen[2] = float((state.contextMisc >> 8) & 0xFF);
        constants.pixelGen[3] = ((state.programControl >> 18) & 1) ? 1.0f : 0.0f;
    }

    // COD3_SKIPPS=hash: the draws of that pixel program are left out, to
    // see what a pass (the fog, say) puts over the picture.
    static const uint64_t skipPixel = []() { const char* t = getenv("COD3_SKIPPS"); return t ? uint64_t(strtoull(t, nullptr, 16)) : 0ull; }();
    if (skipPixel != 0 && ps != nullptr && ps->hash == skipPixel) return false;

    RenderStats::Enter(RenderStats::SectionTextures);
    Textures(vs, ps, out, constants);

    RenderStats::Enter(RenderStats::SectionBuffers);
    if (!Buffers(vs, out)) return false;

    RenderStats::Enter(RenderStats::SectionConstants);
    if (!Constants(vs, ps, out, constants)) return false;

    RenderStats::Enter(RenderStats::SectionIndices);
    if (!Indices(state, convertQuads, convertFan, out)) return false;
    out.baseVertex = state.indexOffset;
    IndexCheck(state, vs, out);
    switch (out.topology)
    {
    case D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST: out.triangles = out.rectangles ? out.indexCount / 3 * 2 : out.indexCount / 3; break;
    case D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP: out.triangles = out.indexCount >= 2 ? out.indexCount - 2 : 0; break;
    default: break;
    }

    if (RenderInternal::FrameLogged())
    {
        RenderStats::Enter(RenderStats::SectionLog);
        out.logged = true;
        Log(state, vs, ps, out);
    }
    DrawLog(state, vs, ps, out);
    return true;
}

bool RenderCommands::PrepareResolve(ResolveCommand& out)
{
    RenderState::ResolveSnapshot state;
    RenderState::ReadResolve(state);
    if (state.pitch == 0) return false;

    const uint32_t x0 = state.scissorTopLeft & 0x7FFF, y0 = (state.scissorTopLeft >> 16) & 0x7FFF;
    const uint32_t x1 = state.scissorBottomRight & 0x7FFF, y1 = (state.scissorBottomRight >> 16) & 0x7FFF;
    if (x1 <= x0 || y1 <= y0 || x1 - x0 > 4096 || y1 - y0 > 4096) return false;
    const uint32_t width = x1 - x0, height = y1 - y0;

    const uint32_t sourceSelect = state.control & 7;
    const uint32_t command = (state.control >> 20) & 3;
    const bool colorClear = ((state.control >> 8) & 1) != 0;
    const bool depthClear = ((state.control >> 9) & 1) != 0;
    RenderStats::Frame().resolves++;
    if (RenderResources::EnsureRows(state.pitch, y1)) out.bindingsLost = true;

    uint32_t targetWidth, targetHeight;
    float scaleX, scaleY;
    RenderResources::TargetSize(state.pitch, 4, targetWidth, targetHeight, scaleX, scaleY);
    const float hostX0 = x0 * scaleX, hostY0 = y0 * scaleY;
    const uint32_t hostWidth = std::max(1u, uint32_t(std::lround(width * scaleX)));
    const uint32_t hostHeight = std::max(1u, uint32_t(std::lround(height * scaleY)));

    if (RenderInternal::FrameLogged())
        printf("frame: resolve control %08X to %08X, %u,%u-%u,%u, pitch %u, clears %d%d, colour %08X depth %08X\n",
            state.control, state.destBase, x0, y0, x1, y1, state.pitch, colorClear ? 1 : 0, depthClear ? 1 : 0, state.colorInfo[0], state.depthInfo);
    // COD3_RESOLVELOG=1: every five seconds, how many resolves went where.
    static const bool resolveLog = getenv("COD3_RESOLVELOG") != nullptr;
    if (resolveLog)
    {
        static std::map<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t>, uint64_t> where;
        static auto told = std::chrono::steady_clock::now();
        where[{ state.control, state.destBase, width, height }]++;
        if (std::chrono::steady_clock::now() - told > std::chrono::seconds(5))
        {
            told = std::chrono::steady_clock::now();
            printf("resolves, five seconds:");
            for (const auto& [key, count] : where)
                printf(" %08X>%08X %ux%u x%llu;", std::get<0>(key), std::get<1>(key), std::get<2>(key), std::get<3>(key), (unsigned long long)count);
            printf("\n");
            fflush(stdout);
            where.clear();
        }
    }

    if (command != 3 && state.destBase != 0)
    {
        const bool fromDepth = sourceSelect == 4;
        DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
        if (fromDepth)
        {
            const RenderResources::DepthTarget* depth = RenderResources::DepthTargetOf(RenderResources::DepthTargetFor(state.depthInfo, state.pitch));
            if (depth == nullptr) return false;
            out.source = depth->resource;
            out.sourceWidth = depth->width;
            out.sourceHeight = depth->height;
            out.sourceSamples = depth->samples;
            format = DXGI_FORMAT_R32_FLOAT;
        }
        else
        {
            const uint32_t info = state.colorInfo[sourceSelect < 4 ? sourceSelect : 0];
            const RenderResources::ColorTarget* color = RenderResources::ColorTargetOf(RenderResources::ColorTargetFor(info, state.pitch));
            if (color == nullptr) return false;
            out.source = color->resource;
            out.sourceWidth = color->width;
            out.sourceHeight = color->height;
            out.sourceSamples = color->samples;
            format = color->format;
        }
        RenderResources::Resolved* surface = RenderResources::ResolvedFor(state.destBase, hostWidth, hostHeight, format, fromDepth);
        if (surface == nullptr) return false;
        surface->guestWidth = width;
        surface->guestHeight = height;
        out.copy = true;
        out.fromDepth = fromDepth;
        out.destination = surface->view;
        out.destinationResource = surface->resource;
        out.x0 = hostX0;
        out.y0 = hostY0;
        out.width = hostWidth;
        out.height = hostHeight;
    }

    if (colorClear)
    {
        if (const RenderResources::ColorTarget* color = RenderResources::ColorTargetOf(RenderResources::ColorTargetFor(state.colorInfo[0], state.pitch)))
        {
            const uint32_t value = state.colorClearValue;
            out.colorClear = true;
            out.clearTarget = color->view;
            out.clearColor[0] = ((value >> 16) & 0xFF) / 255.0f;
            out.clearColor[1] = ((value >> 8) & 0xFF) / 255.0f;
            out.clearColor[2] = (value & 0xFF) / 255.0f;
            out.clearColor[3] = (value >> 24) / 255.0f;
            out.clearRect[0] = int(std::lround(hostX0));
            out.clearRect[1] = int(std::lround(hostY0));
            out.clearRect[2] = int(std::lround(x1 * scaleX));
            out.clearRect[3] = int(std::lround(y1 * scaleY));
        }
    }
    if (depthClear)
    {
        if (const RenderResources::DepthTarget* depth = RenderResources::DepthTargetOf(RenderResources::DepthTargetFor(state.depthInfo, state.pitch)))
        {
            const uint32_t value = state.depthClearValue;
            out.depthClear = true;
            out.clearDepth = depth->view;
            out.clearDepthValue = float(value >> 8) / 16777215.0f;
            out.clearStencilValue = uint8_t(value & 0xFF);
        }
    }
    return out.copy || out.colorClear || out.depthClear;
}

void RenderCommands::SetAimBlur(bool drawn) { g_aimBlur = drawn; }

void RenderCommands::ReportSkips()
{
    bool any = false;
    for (int i = 0; i < SkipCount; i++) if (g_skips[i] != 0) any = true;
    if (!any) return;
    printf("        skipped:");
    for (int i = 0; i < SkipCount; i++) if (g_skips[i] != 0) printf(" %s x%llu;", SkipNames[i], (unsigned long long)g_skips[i]);
    printf("\n");
}
