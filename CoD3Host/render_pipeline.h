#pragma once

// The pipeline state objects, cached: blend, depth stencil, rasteriser and
// sampler states made once for each distinct combination the title asks
// for, and found again by an integer handle.
//
// A state is looked up by the register words that define it. The words
// are hashed into a small open addressing table; a hit is a compare of the
// words and an index, a miss creates the object. A frame asks for a few
// thousand states and gets the same twenty or so, so the table is never
// large and the lookup is a handful of loads with no allocation.

#include "render_state.h"

#include <cstdint>

struct ID3D11Device;
struct ID3D11BlendState;
struct ID3D11DepthStencilState;
struct ID3D11RasterizerState;
struct ID3D11SamplerState;

namespace RenderPipeline
{
    using RenderState::Handle;

    void Initialize(ID3D11Device* device);

    // The texture filtering the settings ask for, laid over what the title's
    // fetch constants say: COD3_TEXTURE_FILTER=native|bilinear|trilinear|anisotropic
    // and COD3_ANISO=1..16, or the settings menu's. Changing it drops the
    // sampler cache, so the next draws take the new samplers.
    enum class Filter { Native, Bilinear, Trilinear, Anisotropic };
    void SetFiltering(Filter filter, uint32_t anisotropy);
    Filter Filtering();
    uint32_t Anisotropy();

    // Blend state from the four blend control words and the colour mask.
    Handle Blend(const uint32_t control[4], uint32_t colorMask);
    // Depth stencil state from the depth control and the stencil masks.
    Handle Depth(uint32_t depthControl, uint32_t stencilRefMask);
    // Rasteriser state from the cull and facing bits, with the scissor.
    // The polygon offset is the front one when bit 11 of the mode control
    // enables it, else the back one when bit 12 does (Direct3D has one for
    // both faces): its scale in sixteenths of a unit of slope, its offset
    // in units of the smallest depth step. `behind` puts the draw's depth
    // a little farther off than it is (the Z pass's, render_commands.cpp).
    Handle Rasterizer(uint32_t suScModeControl, bool scissor, bool cullNone, const float polyOffset[4], bool behind = false);
    // Sampler from the fetch constant's clamp, filter and anisotropy fields.
    Handle Sampler(const uint32_t fetch[6]);

    ID3D11BlendState* BlendObject(Handle handle);

    // Whether a blend state was built from a factor that is the constant
    // colour's alpha rather than its colour. Direct3D takes one factor
    // vector and uses its rgb for the colour channels, so the alpha has to
    // be put there instead; the caller does that with the factor it binds.
    bool BlendTakesConstantAlpha(const uint32_t control[4]);
    ID3D11DepthStencilState* DepthObject(Handle handle);
    ID3D11RasterizerState* RasterizerObject(Handle handle);
    ID3D11SamplerState* SamplerObject(Handle handle);

    struct Statistics
    {
        uint32_t blendStates = 0, depthStates = 0, rasterizerStates = 0, samplers = 0;
    };
    Statistics Stats();
}
