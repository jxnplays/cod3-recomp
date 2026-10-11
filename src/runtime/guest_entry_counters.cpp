// =============================================================================
//  COD3 Recomp - guest function entry counters
//
//  Boot ends with sub_82357D08 and sub_82357E48 alternating until the host
//  stack is exhausted. The call stack showed that - two host addresses
//  repeating sixteen times - but every midasm hook placed inside those
//  functions stayed silent, which is irreconcilable with it.
//
//  XenonRecomp emits each guest function as a weak alias to its body, so a
//  strong definition here replaces it while the original stays reachable as
//  __imp__sub_XXXX. Counting entries in that replacement measures the thing
//  directly instead of inferring it from a stack walk.
//
//  IMPORTANT: adding a wrapper changes codegen - one more host stack frame per
//  guest call, and the extra __restrict. That is a confound, not a detail.
//  COD3_NO_GUEST_OVERRIDES=1 recompiles these wrappers as plain pass-throughs
//  so the difference can be measured. Run both ways before concluding that the
//  override fixed anything.
//
//  Set COD3_TRACE_GUEST_ENTRIES=1 to print entries.
// =============================================================================

#include "ppc_recomp_shared.h"

#include "cod3_runtime.h"

#include <cstdio>
#include <cstdlib>

namespace {

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

} // namespace

extern "C" void __imp__sub_82357D08(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_82357E48(PPCContext& __restrict ctx, uint8_t* base);

#ifdef COD3_NO_GUEST_OVERRIDES

// Pass-through, for the control run.
PPC_FUNC(sub_82357D08)
{
    __imp__sub_82357D08(ctx, base);
}

PPC_FUNC(sub_82357E48)
{
    __imp__sub_82357E48(ctx, base);
}

#else

// -----------------------------------------------------------------------------
// Why the wrapper matters
// -----------------------------------------------------------------------------
//
// The override is not instrumentation with no side effects. Replacing a weak
// alias with a real function body changes codegen for the caller in two ways
// that both matter here:
//
//   1. It adds a host stack frame per guest call. Guest recursion depth is
//      bounded by the host stack, so a guest loop that exhausted the host stack
//      now overflows later - or not at all.
//   2. PPC_FUNC carries __restrict on the context reference, so the compiler is
//      told the callee cannot alias through it.
//
// Measured both ways on the same source:
//
//   without these wrappers   stack overflow; two host frames repeat 15-16 times
//                            each (reported as sub_82357D00 / sub_82357E44)
//   with these wrappers      sub_82357E48 entered once, sub_82357D08 never
//                            entered, no recursion, and a plain access
//                            violation at 0x4B677B8 inside sub_82357E48
//
// So the "recursion" the call stack reported was an artefact of how the
// generated code laid a guest loop out on the host stack, not a property of the
// guest's control flow. The guest terminates that walk on the first entry.
//
// This is not a bug being masked. It is the same total work spread over more
// host frames, so the loop gets more room before the stack runs out. What is
// left is a real fault and is now the only thing in the way.
//
// Keep the wrappers in mind when comparing runs: they are load-bearing.

// -----------------------------------------------------------------------------
// sub_82357D08
// -----------------------------------------------------------------------------
//
// Allocates a 28-byte node through sub_823510B0 and then calls
// sub_82357E48, which calls it back.
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

// -----------------------------------------------------------------------------
// sub_82357E48
// -----------------------------------------------------------------------------
//
//   0x82357E60  r3   = r30 + r3          ; r3 indexes a table in r30
//   0x82357E64  addi  r31,r11,28944      ; r31 = 0x825C7110
//   0x82357E68  lwzx  r11,r30,r31        ; list head at [r30 + 0x825C7110]
//   0x82357E6C  cmpli r11,0
//   0x82357E70  bc    <recurse>          ; branches when NOT zero
//
// It recurses whenever the head is non-zero, handing sub_82357D08 the descriptor
// so a node can be allocated and pushed. The head has to advance or the walk
// never ends.
PPC_FUNC(sub_82357E48)
{
    PPC_FUNC_PROLOGUE();

    Counters& c = counters();
    ++c.e48;
    if (tracing() && c.e48 <= 16)
    {
        const uint32_t headAddr = ctx.r30.u32 + 0x825C7110u;
        std::fprintf(stderr,
            "[entries] sub_82357E48 #%llu  r1=0x%08X r30=0x%08X r3=0x%08X"
            "  head=[0x%08X]=0x%08X\n",
            c.e48, ctx.r1.u32, ctx.r30.u32, ctx.r3.u32,
            headAddr, cod3_load_u32(headAddr));
    }

    __imp__sub_82357E48(ctx, base);
}

#endif

// Print the totals, so a run that dies mid-recursion still reports its depth.
void cod3_report_guest_entries()
{
    const Counters& c = counters();
    if (c.d08 != 0 || c.e48 != 0)
    {
        std::fprintf(stderr, "[entries] total sub_82357D08 = %llu\n", c.d08);
        std::fprintf(stderr, "[entries] total sub_82357E48 = %llu\n", c.e48);
    }
}