#pragma once

#include <cstdint>

// The hook layer over the title's linked-in D3D (d3d_hooks.cpp,
// docs/native-d3d11-plan.md).
namespace D3dHooks
{
    // COD3_D3DHOOKS=1: the hooks observe and report.
    bool Observing();

    // The command processor has reached a draw packet whose last word is at
    // that physical address, with these programs loaded: checked against
    // what the title's draw call said, for the parity of the two ways.
    void CheckDraw(uint32_t lastWordPhysical, uint64_t vertexHash, uint64_t pixelHash, uint32_t modeControl);

    // The pixel program the title's draw call named, for the draw packet
    // whose last word is at that physical address: its microcode's
    // physical address and length. False when the packet came from no
    // recorded call, or the call had no pixel shader.
    bool PixelProgram(uint32_t lastWordPhysical, uint32_t& physical, uint32_t& dwords);
}
