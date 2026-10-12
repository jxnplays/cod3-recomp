#pragma once

#include <cstdint>

namespace AnimHeapTrace
{
    // Prints what the animation heap still holds from earlier levels, with
    // COD3_TRACEANIMHEAP; called as each level's game module starts.
    void LevelStart(uint8_t* base);
}
