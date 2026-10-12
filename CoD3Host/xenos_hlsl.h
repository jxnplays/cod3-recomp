#pragma once

// The console's shader microcode as HLSL, for the D3D11 backend.
//
// A program is a control flow block of exec instructions, each running a
// run of ALU and fetch instructions from the same words. The translation
// is straightforward and literal: every register is a float4, every ALU
// instruction becomes the vector operation on its three operands and the
// scalar operation on the third, written through their masks, exports go
// to the outputs, fetches become loads from the vertex buffers or samples
// from the textures. What the hardware did implicitly - the viewport, the
// interpolator handover - is done by the epilogue and the backend.

#include <cstdint>
#include <string>
#include <vector>

namespace XenosHlsl
{
    struct VertexFetch
    {
        uint32_t slot = 0;        // the fetch constant, 0 to 95
        uint32_t stride = 0;      // in dwords, as the program's first fetch of it says
    };

    struct TextureFetch
    {
        uint32_t slot = 0;        // the fetch constant, 0 to 31; the texture register too
        uint32_t sampler = 0;     // the sampler register, one per texture the program uses
        uint32_t dimension = 1;   // 0 one dimensional, 1 two, 2 three, 3 cube
    };

    struct Translation
    {
        bool ok = false;
        std::string hlsl;
        std::string problem;      // what could not be translated, if anything
        std::vector<VertexFetch> vertexFetches;
        std::vector<TextureFetch> textureFetches;
        uint32_t interpolators = 0;   // how many the vertex program exports
        // The float constants the program reads, packed: the HLSL's c[i] is
        // the file's c[constantMap[i]], so the backend uploads only those.
        // Empty when the program addresses constants relative to a0 and
        // reads the file as it is.
        std::vector<uint16_t> constantMap;
        uint32_t colourTargets = 1;   // a bit per colour target the pixel program writes
        bool writesDepth = false;
        bool usesKill = false;
    };

    // A vertex or pixel program from its microcode words.
    Translation Translate(const std::vector<uint32_t>& words, bool pixel);

    // A number that changes whenever the translation would: the disk cache
    // of compiled programs is keyed by it.
    uint64_t Version();

    // Whether pixel depth is cut to the console's 24 bit float precision
    // (COD3_DEPTH24, on unless 0), and the HLSL function that does it.
    bool Depth24();
    extern const char* const Depth20e4Source;
}
