// =============================================================================
//  COD3 Recomp - guest function entry counters
//
//  Boot ends with sub_82357D08 and sub_82357E48 alternating on the host stack
//  until it is exhausted. The call stack says so - two host addresses repeat
//  sixteen times each - but every midasm hook placed inside those functions
//  stays silent, which is contradictory: either the functions are not
//  executing, or the reporter is attributing the frames to the wrong symbols.
//
//  This settles it without relying on the stack walk at all. XenonRecomp emits
//  each guest function as a weak alias to its body, so providing a strong
//  definition here replaces it while leaving the original reachable as
//  __imp__sub_XXXX. Counting entries in that replacement is direct
//  measurement.
//
//  Set COD3_TRACE_GUEST_ENTRIES=1 to print; the totals print either way.
//
//  Disable with COD3_NO_GUEST_OVERRIDES=1 so the effect of the override on
//  codegen can be ruled out.
// =============================================================================

#include "ppc_recomp_shared.h"

#include "cod3_runtime.h"

#include <cstdio>
#include <cstdlib>

namespace {

// One counter per instrumented function, so a count of N means the guest
// entered that function N times.
struct Counters
{
    unsigned long long d08 = 0;
    unsigned long long e48 = 0;
};

Counters& counters()
{
    static Counters c;
    return c;
}

bool tracing()
{
    static const bool on = std::getenv("COD3_TRACE_GUEST_ENTRIES") != nullptr;
    return on;
}

void report(const char* name, unsigned long long n)
{
    std::fprintf(stderr, "[entries] %s = %llu\n", name, n);
}

} // namespace

// -----------------------------------------------------------------------------
// sub_82357E48
// -----------------------------------------------------------------------------
//
//   0x82357E5C  addis r11,-32164
//   0x82357E60  r3   = r30 + r3          ; r3 indexes a table
//   0x82357E64  addi  r31,r11,28944      ; r31 = 0x825C7110
//   0x82357E68  lwzx  r11,r30,r31        ; list head at [r30 + 0x825C7110]
//   0x82357E6C  cmpli r11,0
//   0x82357E70  bc    <recurse>          ; branches when NOT zero
//
// So this recurses whenever the head is non-zero, and each visit hands
// sub_82357D08 the descriptor so it can allocate and push a node. The head has
// to advance or the walk is endless.
extern "C" void __imp__sub_82357E48(PPCContext& __restrict ctx, uint8_t* base);

PPC_FUNC(sub_82357E48)
{
    PPC_FUNC_PROLOGUE();

    Counters& c = counters();
    ++c.e48;

    if (tracing() && c.e48 <= 16)
    {
        // r30 is the descriptor and r3 the index into it, so the head the
        // walk tests lives at descriptor + 0x825C7110.
        const uint32_t headAddr = ctx.r30.u32 + 0x825C7110u;
        std::fprintf(stderr,
            "[entries] sub_82357E48 #%llu  r1=0x%08X r30=0x%08X r3=0x%08X"
            "  head=[0x%08X]=0x%08X\n",
            c.e48, ctx.r1.u32, ctx.r30.u32, ctx.r3.u32,
            headAddr, cod3_load_u32(headAddr));
    }

    __imp__sub_82357E48(ctx, base);
}

// -----------------------------------------------------------------------------
// sub_82357D08
// -----------------------------------------------------------------------------
//
// The guest function whose body begins at 0x82357D08. It is the one that
// allocates a 28-byte node through sub_823510B0 and then calls
// sub_82357E48, which calls it back.
extern "C" void __imp__sub_82357D08(PPCContext& __restrict ctx, uint8_t* base);

PPC_FUNC(sub_82357D08)
{
    PPC_FUNC_PROLOGUE();

    Counters& c = counters();
    ++c.d08;
    if (tracing() && c.d08 <= 16)
    {
        std::fprintf(stderr,
            "[entries] sub_82357D08 #%llu  r1=0x%08X r3=0x%08X\n",
            c.d08, ctx.r1.u32, ctx.r3.u32);
    }

    __imp__sub_82357D08(ctx, base);
}

// Print the totals. Called from the runtime once the guest has stopped, so a
// run that dies mid-recursion still reports how far it got.
extern "C" void cod3_report_guest_entries()
{
    const Counters& c = counters();
    if (c.d08 != 0 || c.e48 != 0)
    {
        report("sub_82357D08", c.d08);
        report("sub_82357E48", c.e48);
    }
}