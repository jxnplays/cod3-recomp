#pragma once

// The title's own GPU pipeline, watched from the host: see pool_trace.cpp.
namespace PoolTrace
{
    // Once a second: what the pipeline did in the last second.
    void Report();

    // A worker's wait for the GPU, once it is over.
    void Waited(uint64_t nanoseconds);
}
