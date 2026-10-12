#pragma once

#include <cstdint>

struct PPCContext;

// With COD3_PROBE=1, runs a list of guest functions once with known inputs
// and prints what they give back (probe.cpp).
void ProbeRun(PPCContext& ctx, uint8_t* base);
