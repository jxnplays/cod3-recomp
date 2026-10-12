#pragma once

#include <cstdint>
#include <utility>
#include <vector>

// Periodically reports which guest function each guest thread is executing.
// Useful when the title stops making progress without asking the runtime for
// anything, which from the outside looks identical to a hang.

namespace Sampler
{
    // The guest function a host address belongs to, or zero. Used by the fault
    // handler to name which translated function went wrong.
    uint32_t FunctionAt(unsigned long long hostAddress);

    // Names the guest functions on a stack that is already stopped, which is
    // what a fault handler has. Returns how many were written.
    int WalkGuestStack(void* winContext, uint32_t* functions, int limit);

    // The same, for code that is running now rather than stopped in a handler.
    int FunctionsOnStack(uint32_t* functions, int limit);

    // One report of every guest thread, right now.
    void SampleNow();

    // COD3_THREADTIME=1: how busy each of the process's threads has been
    // since the last report, busiest first: the guest's by their number and
    // start address, the host's own by the names given (os id, name).
    bool ThreadTimesWanted();
    void ReportThreadTimes(const std::vector<std::pair<uint32_t, const char*>>& named);

    void Start(int intervalSeconds);
    void Stop();
}
