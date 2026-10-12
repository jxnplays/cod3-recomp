#pragma once

// What the PC render layer (render_commands.cpp) needs from the executor
// (render_d3d11.cpp) besides the caches: the frame log's state.

#include <cstdint>

namespace RenderInternal
{
    bool FrameLogged();       // the frame log is on for this frame
    uint64_t Swaps();         // the title's swaps so far
    int DumpNumber();         // the frame log's dump counter
}
