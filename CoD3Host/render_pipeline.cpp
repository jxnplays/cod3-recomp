#include "render_pipeline.h"
#include "render_table.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <d3d11_1.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace
{
    ID3D11Device* g_device = nullptr;

    // The objects by handle, and the keys they were made for.
    template <int KeyWords, typename Object>
    struct StateTable
    {
        RenderTable::KeyTable<KeyWords> keys;
        std::vector<ComPtr<Object>> objects;   // by handle; objects[0] is null

        StateTable() { objects.emplace_back(); }

        uint32_t Find(const uint32_t* key) const { return keys.Find(key); }

        uint32_t Insert(const uint32_t* key, ComPtr<Object> object)
        {
            const uint32_t handle = uint32_t(objects.size());
            objects.push_back(std::move(object));
            keys.Insert(key, handle);
            return handle;
        }

        Object* Get(uint32_t handle) const
        {
            return handle < objects.size() ? objects[handle].Get() : nullptr;
        }

        void Clear()
        {
            keys.Clear();
            objects.clear();
            objects.emplace_back();
        }

        uint32_t Used() const { return keys.Size(); }
    };

    StateTable<5, ID3D11BlendState> g_blend;
    StateTable<2, ID3D11DepthStencilState> g_depth;
    StateTable<3, ID3D11RasterizerState> g_rasterizer;
    StateTable<2, ID3D11SamplerState> g_sampler;

    RenderPipeline::Filter g_filter = RenderPipeline::Filter::Anisotropic;
    uint32_t g_anisotropy = 16;

    // --- blend -------------------------------------------------------------------

    D3D11_BLEND BlendFactor(uint32_t factor, bool alpha)
    {
        // The console's numbering: zero, one, then the source colour pair at
        // four, source alpha at six, destination colour at eight, destination
        // alpha at ten, the constant colour at twelve and its alpha at
        // fourteen, and source alpha saturate at sixteen. In the alpha
        // channel the colour factors read as their alpha.
        switch (factor)
        {
        case 0:  return D3D11_BLEND_ZERO;
        case 1:  return D3D11_BLEND_ONE;
        case 4:  return alpha ? D3D11_BLEND_SRC_ALPHA : D3D11_BLEND_SRC_COLOR;
        case 5:  return alpha ? D3D11_BLEND_INV_SRC_ALPHA : D3D11_BLEND_INV_SRC_COLOR;
        case 6:  return D3D11_BLEND_SRC_ALPHA;
        case 7:  return D3D11_BLEND_INV_SRC_ALPHA;
        case 8:  return alpha ? D3D11_BLEND_DEST_ALPHA : D3D11_BLEND_DEST_COLOR;
        case 9:  return alpha ? D3D11_BLEND_INV_DEST_ALPHA : D3D11_BLEND_INV_DEST_COLOR;
        case 10: return D3D11_BLEND_DEST_ALPHA;
        case 11: return D3D11_BLEND_INV_DEST_ALPHA;
        case 12: return D3D11_BLEND_BLEND_FACTOR;
        case 13: return D3D11_BLEND_INV_BLEND_FACTOR;
        case 14: return D3D11_BLEND_BLEND_FACTOR;
        case 15: return D3D11_BLEND_INV_BLEND_FACTOR;
        case 16: return D3D11_BLEND_SRC_ALPHA_SAT;
        default: return D3D11_BLEND_ONE;
        }
    }

    D3D11_BLEND_OP BlendOperation(uint32_t op)
    {
        switch (op)
        {
        case 0: return D3D11_BLEND_OP_ADD;
        case 1: return D3D11_BLEND_OP_SUBTRACT;
        case 2: return D3D11_BLEND_OP_MIN;
        case 3: return D3D11_BLEND_OP_MAX;
        case 4: return D3D11_BLEND_OP_REV_SUBTRACT;
        default: return D3D11_BLEND_OP_ADD;
        }
    }

    ComPtr<ID3D11BlendState> MakeBlend(const uint32_t control[4], uint32_t colorMask)
    {
        D3D11_BLEND_DESC desc{};
        desc.IndependentBlendEnable = TRUE;
        for (int i = 0; i < 4; i++)
        {
            const uint32_t c = control[i];
            D3D11_RENDER_TARGET_BLEND_DESC& target = desc.RenderTarget[i];
            const uint32_t srcColor = c & 0x1F, colorOp = (c >> 5) & 7, dstColor = (c >> 8) & 0x1F;
            const uint32_t srcAlpha = (c >> 16) & 0x1F, alphaOp = (c >> 21) & 7, dstAlpha = (c >> 24) & 0x1F;
            const bool trivial = srcColor == 1 && dstColor == 0 && colorOp == 0 &&
                                 srcAlpha == 1 && dstAlpha == 0 && alphaOp == 0;
            target.BlendEnable = trivial ? FALSE : TRUE;
            target.SrcBlend = BlendFactor(srcColor, false);
            target.DestBlend = BlendFactor(dstColor, false);
            target.BlendOp = BlendOperation(colorOp);
            target.SrcBlendAlpha = BlendFactor(srcAlpha, true);
            target.DestBlendAlpha = BlendFactor(dstAlpha, true);
            target.BlendOpAlpha = BlendOperation(alphaOp);
            target.RenderTargetWriteMask = UINT8((colorMask >> (i * 4)) & 0xF);
        }
        ComPtr<ID3D11BlendState> state;
        g_device->CreateBlendState(&desc, &state);
        return state;
    }

    // --- depth stencil -------------------------------------------------------------

    D3D11_STENCIL_OP StencilOperation(uint32_t op)
    {
        switch (op)
        {
        case 0: return D3D11_STENCIL_OP_KEEP;
        case 1: return D3D11_STENCIL_OP_ZERO;
        case 2: return D3D11_STENCIL_OP_REPLACE;
        case 3: return D3D11_STENCIL_OP_INCR_SAT;
        case 4: return D3D11_STENCIL_OP_DECR_SAT;
        case 5: return D3D11_STENCIL_OP_INVERT;
        case 6: return D3D11_STENCIL_OP_INCR;
        case 7: return D3D11_STENCIL_OP_DECR;
        default: return D3D11_STENCIL_OP_KEEP;
        }
    }

    ComPtr<ID3D11DepthStencilState> MakeDepth(uint32_t depthControl, uint32_t stencilRefMask)
    {
        D3D11_DEPTH_STENCIL_DESC desc{};
        desc.DepthEnable = (depthControl >> 1) & 1;
        desc.DepthWriteMask = ((depthControl >> 2) & 1) ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
        desc.DepthFunc = D3D11_COMPARISON_FUNC(((depthControl >> 4) & 7) + 1);
        desc.StencilEnable = depthControl & 1;
        desc.StencilReadMask = UINT8((stencilRefMask >> 8) & 0xFF);
        desc.StencilWriteMask = UINT8((stencilRefMask >> 16) & 0xFF);
        desc.FrontFace.StencilFunc = D3D11_COMPARISON_FUNC(((depthControl >> 8) & 7) + 1);
        desc.FrontFace.StencilFailOp = StencilOperation((depthControl >> 11) & 7);
        desc.FrontFace.StencilPassOp = StencilOperation((depthControl >> 14) & 7);
        desc.FrontFace.StencilDepthFailOp = StencilOperation((depthControl >> 17) & 7);
        if ((depthControl >> 7) & 1)   // back face state of its own
        {
            desc.BackFace.StencilFunc = D3D11_COMPARISON_FUNC(((depthControl >> 20) & 7) + 1);
            desc.BackFace.StencilFailOp = StencilOperation((depthControl >> 23) & 7);
            desc.BackFace.StencilPassOp = StencilOperation((depthControl >> 26) & 7);
            desc.BackFace.StencilDepthFailOp = StencilOperation((depthControl >> 29) & 7);
        }
        else desc.BackFace = desc.FrontFace;
        ComPtr<ID3D11DepthStencilState> state;
        g_device->CreateDepthStencilState(&desc, &state);
        return state;
    }

    // --- rasteriser ------------------------------------------------------------------

    ComPtr<ID3D11RasterizerState> MakeRasterizer(uint32_t key, float scale, float offset)
    {
        D3D11_RASTERIZER_DESC desc{};
        desc.FillMode = D3D11_FILL_SOLID;
        const bool cullFront = (key & 1) != 0;
        const bool cullBack = (key & 2) != 0;
        desc.CullMode = cullFront && cullBack ? D3D11_CULL_BACK   // everything: nothing would show; the nearest sense
                      : cullFront ? D3D11_CULL_FRONT : cullBack ? D3D11_CULL_BACK : D3D11_CULL_NONE;
        if (key & 16) desc.CullMode = D3D11_CULL_NONE;
        // FACE: nought means a counter clockwise triangle is a front face.
        desc.FrontCounterClockwise = ((key >> 2) & 1) == 0 ? TRUE : FALSE;
        desc.DepthClipEnable = FALSE;
        desc.ScissorEnable = (key & 8) ? TRUE : FALSE;
        desc.MultisampleEnable = TRUE;   // for the multisampled targets; nothing to a single sampled one
        // The console's polygon offset: the slope's scale in sixteenths (as
        // Xenia takes it), the offset in the depth's own units, which this
        // title gives as a fraction of the range (0.0001 for its decals):
        // steps of a 24 bit depth, whole, and at least one when asked for.
        // Without it the shadow maps shadowed their own surfaces in
        // blotches: the soldiers' uniforms.
        const float steps = offset * 16777216.0f;
        desc.DepthBias = steps == 0.0f ? 0 : int(steps > 0.0f ? std::max(1.0f, std::round(steps)) : std::min(-1.0f, std::round(steps)));
        desc.SlopeScaledDepthBias = scale * (1.0f / 16.0f);
        desc.DepthBiasClamp = 0.0f;
        // Behind (the Z pass): farther off (towards nought, this title's
        // far) by sixteen steps of the float depth at the triangle's
        // nearest, and by two pixels of its slope.
        if (key & 32)
        {
            desc.DepthBias -= 16;
            desc.SlopeScaledDepthBias -= 2.0f;
        }
        ComPtr<ID3D11RasterizerState> state;
        g_device->CreateRasterizerState(&desc, &state);
        return state;
    }

    // --- samplers ----------------------------------------------------------------------

    D3D11_TEXTURE_ADDRESS_MODE AddressMode(uint32_t clamp)
    {
        switch (clamp)
        {
        case 0: return D3D11_TEXTURE_ADDRESS_WRAP;
        case 1: return D3D11_TEXTURE_ADDRESS_MIRROR;
        case 2: return D3D11_TEXTURE_ADDRESS_CLAMP;
        case 3: return D3D11_TEXTURE_ADDRESS_MIRROR_ONCE;
        case 4: return D3D11_TEXTURE_ADDRESS_CLAMP;
        case 5: return D3D11_TEXTURE_ADDRESS_MIRROR_ONCE;
        default: return D3D11_TEXTURE_ADDRESS_BORDER;
        }
    }

    ComPtr<ID3D11SamplerState> MakeSampler(uint32_t key)
    {
        const uint32_t clampX = key & 7, clampY = (key >> 3) & 7, clampZ = (key >> 6) & 7;
        const uint32_t mag = (key >> 9) & 3, min = (key >> 11) & 3, mip = (key >> 13) & 3;
        const uint32_t aniso = (key >> 15) & 7;
        const uint32_t border = (key >> 18) & 3;

        D3D11_SAMPLER_DESC desc{};
        bool linearMag = mag == 1, linearMin = min == 1, linearMip = mip == 1;
        // A texture the title samples with no mip level at all (mip == 2,
        // "base map") keeps that: its levels are not there to be used.
        const bool noMips = mip == 2;
        uint32_t anisotropy = (aniso >= 1 && aniso <= 4) ? (1u << aniso) : 1;
        switch (g_filter)
        {
        case RenderPipeline::Filter::Native: break;
        case RenderPipeline::Filter::Bilinear: linearMag = linearMin = true; anisotropy = 1; break;
        case RenderPipeline::Filter::Trilinear: linearMag = linearMin = true; linearMip = !noMips; anisotropy = 1; break;
        case RenderPipeline::Filter::Anisotropic: linearMag = linearMin = true; linearMip = !noMips; anisotropy = noMips ? 1 : g_anisotropy; break;
        }
        if (anisotropy > 1)
        {
            desc.Filter = D3D11_FILTER_ANISOTROPIC;
            desc.MaxAnisotropy = anisotropy;
        }
        else
        {
            desc.Filter = D3D11_FILTER((linearMin ? 0x10 : 0) | (linearMag ? 0x4 : 0) | (linearMip ? 0x1 : 0));
            desc.MaxAnisotropy = 1;
        }
        desc.AddressU = AddressMode(clampX);
        desc.AddressV = AddressMode(clampY);
        desc.AddressW = AddressMode(clampZ);
        desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
        desc.MinLOD = 0.0f;
        desc.MaxLOD = noMips ? 0.0f : D3D11_FLOAT32_MAX;
        const float white = (border == 1) ? 1.0f : 0.0f;
        for (float& f : desc.BorderColor) f = white;
        ComPtr<ID3D11SamplerState> state;
        g_device->CreateSamplerState(&desc, &state);
        return state;
    }
}

void RenderPipeline::Initialize(ID3D11Device* device)
{
    g_device = device;
    // COD3_TEXTURE_FILTER and COD3_ANISO from the environment; the settings
    // menu sets them afterwards when it has an opinion.
    if (const char* text = getenv("COD3_TEXTURE_FILTER"))
    {
        if (strcmp(text, "native") == 0) g_filter = Filter::Native;
        else if (strcmp(text, "bilinear") == 0) g_filter = Filter::Bilinear;
        else if (strcmp(text, "trilinear") == 0) g_filter = Filter::Trilinear;
        else if (strcmp(text, "anisotropic") == 0) g_filter = Filter::Anisotropic;
    }
    if (const char* text = getenv("COD3_ANISO"))
    {
        const long value = strtol(text, nullptr, 10);
        g_anisotropy = uint32_t(value < 1 ? 1 : value > 16 ? 16 : value);
    }
}

void RenderPipeline::SetFiltering(Filter filter, uint32_t anisotropy)
{
    anisotropy = anisotropy < 1 ? 1 : anisotropy > 16 ? 16 : anisotropy;
    if (filter == g_filter && anisotropy == g_anisotropy) return;
    g_filter = filter;
    g_anisotropy = anisotropy;
    g_sampler.Clear();
}

RenderPipeline::Filter RenderPipeline::Filtering() { return g_filter; }
uint32_t RenderPipeline::Anisotropy() { return g_anisotropy; }

RenderState::Handle RenderPipeline::Blend(const uint32_t control[4], uint32_t colorMask)
{
    const uint32_t key[5] = { control[0], control[1], control[2], control[3], colorMask };
    if (const uint32_t handle = g_blend.Find(key)) return handle;
    return g_blend.Insert(key, MakeBlend(control, colorMask));
}

RenderState::Handle RenderPipeline::Depth(uint32_t depthControl, uint32_t stencilRefMask)
{
    const uint32_t key[2] = { depthControl, stencilRefMask & 0x00FFFF00u };
    if (const uint32_t handle = g_depth.Find(key)) return handle;
    return g_depth.Insert(key, MakeDepth(depthControl, stencilRefMask));
}

RenderState::Handle RenderPipeline::Rasterizer(uint32_t suScModeControl, bool scissor, bool cullNone, const float polyOffset[4], bool behind)
{
    float scale = 0.0f, offset = 0.0f;
    static const bool noOffset = getenv("COD3_NOPOLYOFFSET") != nullptr;   // for a comparison
    if (!noOffset && ((suScModeControl >> 11) & 1)) { scale = polyOffset[0]; offset = polyOffset[1]; }
    else if (!noOffset && ((suScModeControl >> 12) & 1)) { scale = polyOffset[2]; offset = polyOffset[3]; }
    uint32_t scaleBits, offsetBits;
    memcpy(&scaleBits, &scale, 4);
    memcpy(&offsetBits, &offset, 4);
    const uint32_t key[3] = { (suScModeControl & 7) | (scissor ? 8u : 0u) | (cullNone ? 16u : 0u) | (behind ? 32u : 0u), scaleBits, offsetBits };
    if (const uint32_t handle = g_rasterizer.Find(key)) return handle;
    // Each new offset once, to see what the title asks for.
    static int announced = 0;
    if ((scale != 0.0f || offset != 0.0f) && announced++ < 12)
    {
        printf("render: polygon offset scale %g offset %g (mode control %08X)\n", scale, offset, suScModeControl);
        fflush(stdout);
    }
    return g_rasterizer.Insert(key, MakeRasterizer(key[0], scale, offset));
}

RenderState::Handle RenderPipeline::Sampler(const uint32_t fetch[6])
{
    const uint32_t clampX = (fetch[0] >> 10) & 7, clampY = (fetch[0] >> 13) & 7, clampZ = (fetch[0] >> 16) & 7;
    uint32_t mag = (fetch[3] >> 19) & 3, min = (fetch[3] >> 21) & 3, mip = (fetch[3] >> 23) & 3;
    const uint32_t aniso = (fetch[3] >> 25) & 7;
    const uint32_t border = fetch[5] & 3;
    if (mag == 3) mag = 1;
    if (min == 3) min = 1;
    if (mip == 3) mip = 1;
    const uint32_t key[2] = { clampX | (clampY << 3) | (clampZ << 6) | (mag << 9) | (min << 11) | (mip << 13) | (aniso << 15) | (border << 18), 0 };
    if (const uint32_t handle = g_sampler.Find(key)) return handle;
    return g_sampler.Insert(key, MakeSampler(key[0]));
}

bool RenderPipeline::BlendTakesConstantAlpha(const uint32_t control[4])
{
    for (int i = 0; i < 4; i++)
    {
        const uint32_t word = control[i];
        const uint32_t factors[4] = { word & 0x1F, (word >> 8) & 0x1F, (word >> 16) & 0x1F, (word >> 24) & 0x1F };
        for (uint32_t factor : factors)
            if (factor == 14 || factor == 15) return true;
    }
    return false;
}

ID3D11BlendState* RenderPipeline::BlendObject(Handle handle) { return g_blend.Get(handle); }
ID3D11DepthStencilState* RenderPipeline::DepthObject(Handle handle) { return g_depth.Get(handle); }
ID3D11RasterizerState* RenderPipeline::RasterizerObject(Handle handle) { return g_rasterizer.Get(handle); }
ID3D11SamplerState* RenderPipeline::SamplerObject(Handle handle) { return g_sampler.Get(handle); }

RenderPipeline::Statistics RenderPipeline::Stats()
{
    Statistics s;
    s.blendStates = g_blend.Used();
    s.depthStates = g_depth.Used();
    s.rasterizerStates = g_rasterizer.Used();
    s.samplers = g_sampler.Used();
    return s;
}
