// Calls guest functions with known inputs and prints what they give back:
// the quickest way to name a pure function (the C runtime's math, the
// scripts' vector built-ins) is to run it. COD3_PROBE=1, once, at the first
// start of a level's game module (table_trace.cpp calls Probe::Run there,
// on the game's own thread, between two of its calls).
//
// Each function is called on a copy of the thread's context with its stack
// pointer moved down, and three vectors in the gap: A (3, 4, 0) in r3 or
// r4, B (1, 2, 2), and C (zeroed) for a result; f1 = 0.5, f2 = 2. What is
// printed is f1, r3 and the three vectors after the call.

#include "probe.h"
#include "kernel.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
    void PutVector(uint8_t* base, uint32_t at, float x, float y, float z)
    {
        const float v[3] = { x, y, z };
        for (int i = 0; i < 3; i++)
        {
            uint32_t bits;
            memcpy(&bits, &v[i], 4);
            Guest::Write32(base, at + 4 * i, bits);
        }
    }

    float GetFloat(uint8_t* base, uint32_t at)
    {
        const uint32_t bits = Guest::Read32(base, at);
        float value;
        memcpy(&value, &bits, 4);
        return value;
    }

    struct Probe
    {
        uint32_t address;
        const char* args;   // which registers carry what: "f" floats, "ab" vectors in r3 r4, "cab" out in r3
    };

    // The functions to run: the table's math and vector entries and the
    // C runtime routines they reach.
    const Probe probes[] = {
        { 0x8234B538, "f" }, { 0x8234B610, "f" }, { 0x8234BBD8, "f" }, { 0x8234D010, "f" },
        { 0x8234D0D8, "f" }, { 0x8234EC40, "f" }, { 0x8234E980, "f" },
        { 0x820A00C8, "f" }, { 0x820A00A0, "f" }, { 0x820A00F0, "f" }, { 0x820B2048, "f" },
        { 0x820A4950, "f" }, { 0x820B2070, "f" }, { 0x820B26F8, "f" },
        { 0x820B2228, "ab" }, { 0x820B2228, "cab" }, { 0x820B2290, "ab" }, { 0x820B2290, "cab" },
        { 0x820B2300, "cab" }, { 0x820B2310, "cab" }, { 0x820B23C8, "cab" }, { 0x820B23D8, "cab" },
        { 0x820B23E8, "cab" }, { 0x820B2680, "cab" },
    };
}

void ProbeRun(PPCContext& ctx, uint8_t* base)
{
    static bool done = false;
    if (done || getenv("COD3_PROBE") == nullptr) return;
    done = true;

    const uint32_t top = ctx.r1.u32;
    const uint32_t a = top - 0x1000, b = top - 0x0F00, c = top - 0x0E00;
    for (const Probe& probe : probes)
    {
        PutVector(base, a, 3, 4, 0);
        PutVector(base, b, 1, 2, 2);
        PutVector(base, c, 0, 0, 0);
        PPCContext call = ctx;
        call.r1.u32 = top - 0x4000;
        call.f1.f64 = 0.5;
        call.f2.f64 = 2.0;
        if (strcmp(probe.args, "ab") == 0) { call.r3.u32 = a; call.r4.u32 = b; call.r5.u32 = c; }
        else if (strcmp(probe.args, "cab") == 0) { call.r3.u32 = c; call.r4.u32 = a; call.r5.u32 = b; }
        call.lr = 0;
        PPCFunc* function = PPC_LOOKUP_FUNC(base, probe.address);
        function(call, base);
        printf("probe: sub_%08X(%s): f1 %g r3 %08X; A %g %g %g; B %g %g %g; C %g %g %g\n", probe.address, probe.args,
            call.f1.f64, call.r3.u32, GetFloat(base, a), GetFloat(base, a + 4), GetFloat(base, a + 8),
            GetFloat(base, b), GetFloat(base, b + 4), GetFloat(base, b + 8),
            GetFloat(base, c), GetFloat(base, c + 4), GetFloat(base, c + 8));
    }
    fflush(stdout);
}
