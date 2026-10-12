#pragma once

#include <cstdint>

// A timeline of the title's GPU pipeline, COD3_TIMELINE=1: the moments the
// handshake between the render threads, the replaying thread and the
// command processor passes through, with microsecond times, so the gaps
// between them can be read off. Recording starts a few seconds into a
// level and stops when the buffer is full; the buffer is printed once.
//
// COD3_TIMELINE=stall keeps recording, round and round, and prints only
// when the watchdog finds the title stopped: the moments before a freeze
// that comes at any time.
namespace Timeline
{
    bool Enabled();
    bool OnStallOnly();
    void Mark(const char* what, uint32_t a = 0, uint32_t b = 0);
    void Report();
}
