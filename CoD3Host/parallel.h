#pragma once

// Work split across the machine's cores.
//
// The rasteriser is software, and a frame of it is millions of pixels each
// run through an interpreted shader. On one thread that is a few frames a
// second. The rows of a triangle are independent of each other: the tiled
// address function is a permutation, so two rows never touch the same word of
// EDRAM, and blending only ever reads the pixel it is about to write. So the
// rows are handed out in bands, and every core takes bands until none are
// left.
//
// This is deliberately small. One batch at a time, dispatched by the thread
// that has the work and finished before it returns, so nothing here outlives
// the call that started it and no work is ever queued behind other work.

#include <functional>

namespace Parallel
{
    // Runs work(first, last) over [begin, end) in pieces of about grain, on
    // every core including the caller's. Returns when all of it is done. With
    // fewer rows than one piece it simply runs the work inline.
    void For(int begin, int end, int grain,
             const std::function<void(int, int)>& work);

    // How many threads take part, counting the caller.
    int Width();
}
