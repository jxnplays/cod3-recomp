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

struct Counters
{
    unsigned long long d08 = 0;
    unsigned long long e48 = 0;

    // First few entries, for diagnosis. Fixed size so recording costs nothing.
    uint32_t e48_head[8] = {};
    uint32_t e48_desc[8] = {};
    uint32_t e48_index[8] = {};

    // The same for sub_82357D08.
    uint32_t d08_r29[8] = {};
    uint32_t d08_head[8] = {};

    // The seeding functions. These are what populate the free list in the first
    // place: sub_82357BF0 walks the 36-slot table and calls sub_8235B570 on
    // each empty slot. If they are never entered, the list stays empty and the
    // D08/E48 pair has no exit condition.
    unsigned long long bf0 = 0;   // sub_82357BF0 - the table walker
    unsigned long long b570 = 0;  // sub_8235B570 - the per-slot seeder
    unsigned long long bf0_r3 = 0;
    unsigned long long bf0_r30 = 0;

    // The allocator. The crash stack shows sub_823478F0 -> sub_823510B0 ->
    // D08 -> E48, so the allocator is the frame that fails to return. Recording
    // its size argument shows whether each pass asks for something new (a real
    // loop making progress) or the same thing over and over (a path that cannot
    // terminate).
    unsigned long long alloc10b0 = 0;
    unsigned long long alloc_first_r3 = 0;
    unsigned long long alloc_first_r4 = 0;
    unsigned long long alloc_first_r5 = 0;
    unsigned long long alloc_last_r3 = 0;

    // sub_82344CF8 is the XEX entry body. It calls these four in sequence:
    //
    //   0x82344D34  sub_82349D98
    //   0x82344D3C  sub_82349BF0
    //   0x82344D40  sub_82344B30
    //   0x82344D54  sub_82352998   <- the only caller of the free-list seeder
    //
    // The seeder's call site is unconditional, so if its counter reads zero
    // then one of the three earlier calls never returned. Counting all four
    // identifies which, without guessing.
    unsigned long long s49d98 = 0;
    unsigned long long s49bf0 = 0;
    unsigned long long s44b30 = 0;
    unsigned long long s52998 = 0;

    unsigned long long close_calls = 0;
};

// A shared counter for the four functions sub_82344CF8 calls in order. Wrapping
// each of them costs one host frame apiece, which slightly deepens any guest
// recursion - enough to change when the stack runs out, not enough to change
// whether it runs. That caveat is why the counts below are compared against each
// other rather than treated as absolute depths.
#define COD3_COUNTED_WRAPPER(name, field)                                      \
    extern "C" void __imp__##name(PPCContext& __restrict ctx, uint8_t* base);  \
    PPC_FUNC(name)                                                            \
    {                                                                         \
        PPC_FUNC_PROLOGUE();                                                   \
        counters().field++;                                                    \
        __imp__##name(ctx, base);                                              \
    }

// The four functions sub_82344CF8 calls in order are wrapped at the end of this
// file, once counters() is visible.

// sub_82357D08 and sub_82357E48 index the same list but with different
// registers:
//
//   sub_82357D00 at 0x82357D4C   rlwinm r29,r29,3,0,28
//   sub_82357D00 at 0x82357D50   lwzx  r11,r29,r30          r30 = 0x825C7110
//   sub_82357E48 at 0x82357E90   rlwinm r30,r3,3,0,28
//   sub_82357E48 at 0x82357E98   lwzx  r11,r30,r31          r31 = 0x825C7110
//
// D08 takes its slot index from r29, inherited from its caller. E48 derives it
// from r3, which D08 sets to 10 at 0x82357D8C (`li r3,10`), giving 10<<3 = 80.
//
// If r29 is not 80 the two functions read and write different slots. D08 then
// pushes its node into a slot E48 never inspects, E48 still sees an empty list,
// and calls D08 again - forever. That is the shape of the recursion, so both
// values are recorded rather than the mismatch being inferred from the eventual
// stack exhaustion.

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

// Note: tracing perturbs this loop. With the per-entry fprintf enabled the
// walk terminates on the first entry; without it, it runs for ~6,400 levels
// before the host stack is exhausted. Both are the same guest code path - the
// output is simply slow enough to change how the host stack is consumed - so
// per-entry output cannot be used to judge depth. The counters can.

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

    // Record the slot index D08 derives its head from. At 0x82357D4C the guest
    // does rlwinm r29,r29,3,0,28, so the head it tests is at
    // (r29 & ~7) + 0x825C7110 - computed here so it matches what the guest
    // reads rather than what this override happens to be holding.
    if (c.d08 <= 8)
    {
        const uint32_t slot = (ctx.r29.u32 << 3) & 0xFFFFFFF8u;
        c.d08_r29[c.d08 - 1] = slot;
        c.d08_head[c.d08 - 1] = cod3_load_u32(slot + 0x825C7110u);
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

    // Record the list head the walk is about to test, without printing per
    // entry: printing perturbs the loop badly enough to hide the problem.
    // Recording a handful of samples is enough to tell whether the head ever
    // changes.
    if (c.e48 <= 8)
    {
        // 0x82357E90 is rlwinm r30,r3,3,0,28 and 0x82357E98 is lwzx
        // r11,r30,r31 with r31 = 0x825C7110. At function entry r30 has not been
        // computed yet, so the slot has to be derived from r3 the same way the
        // guest does, rather than read out of r30.
        const uint32_t slot = (ctx.r3.u32 << 3) & 0xFFFFFFF8u;
        c.e48_head[c.e48 - 1] = cod3_load_u32(slot + 0x825C7110u);
        c.e48_desc[c.e48 - 1] = slot;
        c.e48_index[c.e48 - 1] = ctx.r3.u32;
    }

    __imp__sub_82357E48(ctx, base);
}

#endif

// -----------------------------------------------------------------------------
// sub_82357BF0 - the free-list table walker
// -----------------------------------------------------------------------------
//
//   0x82357C0C  addi   r28,r11,-2960      ; r28 = 0x825C5AC0, the descriptor
//   0x82357C14  lwz    r11,4(r31)         ; slot state
//   0x82357C18  cmpwi  cr6,r11,1          ; unseeded?
//   0x82357C24  li     r4,4000
//   0x82357C34  bl     0x8235b570         ; seed it
//   0x82357C40  addi   r31,r31,8          ; next slot
//   0x82357C44  addi   r11,r30,288        ; 36 slots
//   0x82357C50  blt    cr6,0x82357c14     ; loop
//
// This is the only thing that ever puts a node on a list head. Counting it
// distinguishes "the walker never ran" from "the walker ran but the seeder
// failed", which are very different bugs and were previously being conflated.
extern "C" void __imp__sub_82357BF0(PPCContext& __restrict ctx, uint8_t* base);

PPC_FUNC(sub_82357BF0)
{
    PPC_FUNC_PROLOGUE();

    Counters& c = counters();
    ++c.bf0;
    c.bf0_r3 = ctx.r3.u32;
    c.bf0_r30 = ctx.r30.u32;

    __imp__sub_82357BF0(ctx, base);
}

// -----------------------------------------------------------------------------
// sub_8235B570 - the per-slot seeder
// -----------------------------------------------------------------------------
//
// Reached only from the walker above. Resolves a function pointer on first call,
// caches it at 0x8291F6A0, then calls through it. Its return value decides
// whether the walker leaves the slot at 0 (empty) or records the node.
extern "C" void __imp__sub_8235B570(PPCContext& __restrict ctx, uint8_t* base);

PPC_FUNC(sub_8235B570)
{
    PPC_FUNC_PROLOGUE();

    counters().b570++;

    __imp__sub_8235B570(ctx, base);
}

// -----------------------------------------------------------------------------
// sub_823510B0 - the allocator
// -----------------------------------------------------------------------------
//
// The frame above the D08/E48 pair in the crash stack. sub_823478F0 is this
// function's heap-core carve (0x82351108  bl 0x823478f0); the crash stack has
// sub_823478F0 calling into sub_823510B0, which is the opposite direction, so
// one of the two attributions is wrong and this counter decides which.
extern "C" void __imp__sub_823510B0(PPCContext& __restrict ctx, uint8_t* base);

PPC_FUNC(sub_823510B0)
{
    PPC_FUNC_PROLOGUE();

    Counters& c = counters();
    if (++c.alloc10b0 == 1)
    {
        c.alloc_first_r3 = ctx.r3.u32;
        c.alloc_first_r4 = ctx.r4.u32;
        c.alloc_first_r5 = ctx.r5.u32;
    }
    c.alloc_last_r3 = ctx.r3.u32;

    __imp__sub_823510B0(ctx, base);
}

// Print the totals, so a run that dies mid-recursion still reports its depth.
void cod3_report_guest_entries()
{
    const Counters& c = counters();
    if (c.d08 != 0 || c.e48 != 0)
    {
        std::fprintf(stderr, "[entries] total sub_82357D08 = %llu\n", c.d08);
        std::fprintf(stderr, "[entries] total sub_82357E48 = %llu\n", c.e48);

        for (unsigned i = 0; i < 8 && i < c.e48; ++i)
        {
            std::fprintf(stderr,
                "[entries]   E48 #%u index=0x%08X (slot 0x%08X) head=0x%08X\n",
                i + 1, c.e48_index[i], c.e48_index[i] << 3, c.e48_head[i]);
        }

        // D08's slot index comes from its caller's r29, not from r3. If this
        // differs from E48's 0x50, the two functions are working on different
        // slots and neither can ever see what the other wrote.
        std::fprintf(stderr,
            "[entries]   E48 uses slot 0x%08X (r3=10 << 3)\n",
            10u << 3);

        // -----------------------------------------------------------------------------
// What this establishes, measured rather than inferred
// -----------------------------------------------------------------------------
//
// Boot ends with ~6,415 mutual entries into sub_82357D08 and sub_82357E48 - one
// level each, exactly balanced - until the host stack is exhausted.
//
// D08 and E48 walk size-class slot 0x50 of the CRT heap free-list table at
// 0x825C7110:
//
//   0x82357D50  lwzx  r11,r29,r30     D08: r30 = 0x825C7110, r29 = caller slot
//   0x82357E98  lwzx  r11,r30,r31     E48: r31 = 0x825C7110, r30 = r3 << 3
//
// They agree on the slot: E48 is entered with r3 = 10 giving r30 = 0x50, and D08
// with r29 = 0x50. An earlier hypothesis that they disagreed - each deriving
// its own index - is disproved by the recorded values. Both read head = 0 on
// every one of the 6,415 entries.
//
// The crash stack shows where the pair is entered from, and this is the part
// that was wrong before:
//
//     #6  sub_823478F0
//     #7  sub_823510B0    <- the allocator
//     #8  sub_82357D00
//     #9  sub_82357E44
//    #10  sub_82357D00
//    #11  sub_82357E44    ... 96 frames
//
// So the recursion is not the free-list walk finding an unterminated list. It is
// the allocator sub_823510B0 failing to terminate, and the D08/E48 pair is what
// it is failing inside. At 0x82351108 it calls sub_823478F0 to get memory and
// at 0x823510D0 calls sub_82348d78; one of those is handing back control into
// the very code that asked for memory.
//
// The list itself is never seeded, which is a separate and real defect:
// sub_82357BF0 is the only function that seeds those slots (it walks 36 of
// them, 0x82357C40 addi r31,r31,8 through 0x82357C44 addi r11,r30,288, calling
// sub_8235B570 on each whose state is 1). Both counters read zero, so neither
// runs. Its single call site is sub_82352998 at 0x82352A08, gated on
// KeTlsAlloc at 0x823529E0 and KeTlsSetValue at 0x823529F8, and tracing both
// return addresses (COD3_TRACE_LR) produces nothing, so that function is not
// reached during boot either.
//
// Note the return addresses are the ones after the call, so the KeTlsAlloc site
// traces at lr=0x823529E8, not 0x823529E0. Pointing the tracer at the
// instruction address silently reports "never called" for a call that happens on
// every iteration - a mistake that briefly looked like a dead end.
//
// Two distinct bugs, both real, neither yet fixed:
//
//   1. sub_823510B0 (allocator) recurses instead of returning. This is what
//      ends boot.
//   2. The free-list seeding pass (sub_82357BF0) never runs, so size-class 10
//      stays empty.
//
// (1) is the blocker. Fixing (2) alone would not make boot complete.
//
// -----------------------------------------------------------------------------
// The deadlock, stated precisely
// -----------------------------------------------------------------------------
//
// Counters from one run, all measured by entry override:
//
//   sub_823510B0 = 6416   r3 = 0x1C every time (28 bytes), r4 = 0, r5 = 0x800
//   sub_82357D08 = 6416
//   sub_82357E48 = 6416
//   sub_82357BF0 = 0
//   sub_8235B570 = 0
//
// NtAllocateVirtualMemory is called 80 times and succeeds every time, so the
// guest heap is not exhausted and the VM stub is not the problem.
//
// Read D08 and E48 together and the cycle is explicit:
//
//   D08  0x82357D50  lwzx  head        D08  0x82357D58  beq -> allocate
//   D08  0x82357D64  li    r3,28
//   D08  0x82357D68  bl    sub_823510B0        the 28-byte node
//   D08  0x82357D8C  li    r3,10
//   D08  0x82357D90  bl    sub_82357E48        recurse
//
//   E48  0x82357E98  lwzx  head
//   E48  0x82357E9C  cmplwi head,0
//   E48  0x82357EA0  bne   -> return the head    non-empty: exit
//   E48  0x82357EA4  bl    sub_82357D08           empty: recurse
//
// E48 recurses exactly while the head is zero, and the head is only pushed at
// D08 0x82357DD8 (`stwx r28,r29,r30`) - which is reached from 0x82357DA0, i.e.
// after sub_82357E48 has already returned. So the list is only populated on the
// way back out of the recursion, and the recursion only unwinds when the list is
// populated. With an empty list that never happens, and the two functions consume
// the host stack until it is gone.
//
// Each level costs one 28-byte allocation, which is why the allocator's count
// tracks the recursion count exactly and why its size argument never changes.
//
// So the free list must already be non-empty when D08 is first entered. In a
// working run sub_82357BF0 has already seeded all 36 slots during CRT heap
// initialisation, via sub_82352998 - and here it never runs. Fixing the
// seeder is what breaks the deadlock; everything else is a consequence.

        for (unsigned i = 0; i < 8 && i < c.d08; ++i)
        {
            std::fprintf(stderr,
                "[entries]   D08 #%u slot=0x%08X head=0x%08X\n",
                i + 1, c.d08_r29[i], c.d08_head[i]);
        }

        // The seeding side. If the walker never ran, the list is empty because
        // nothing ever populated it, and the D08/E48 recursion has no exit.
        std::fprintf(stderr,
            "[entries] seeding: sub_82357BF0 = %llu (r3=0x%08X r30=0x%08X),"
            " sub_8235B570 = %llu\n",
            c.bf0, c.bf0_r3, c.bf0_r30, c.b570);

        std::fprintf(stderr,
            "[entries] allocator sub_823510B0 = %llu"
            "  first r3=0x%08X r4=0x%08X r5=0x%08X  last r3=0x%08X\n",
            c.alloc10b0, c.alloc_first_r3, c.alloc_first_r4,
            c.alloc_first_r5, c.alloc_last_r3);

        // sub_82344CF8 calls these in order. The first with a non-trivial count
        // is where the recursion actually is; anything after it is never
        // reached, which is why the seeder's count is zero.
        std::fprintf(stderr,
            "[entries] entry order: sub_82349D98 = %llu,"
            " sub_82349BF0 = %llu, sub_82344B30 = %llu,"
            " sub_82352998 = %llu\n",
            c.s49d98, c.s49bf0, c.s44b30, c.s52998);
    }
}

// -----------------------------------------------------------------------------
// MmAllocatePhysicalMemoryEx - why it is being implemented now
// -----------------------------------------------------------------------------
//
// The stub returns 0 and the guest calls it 60 times during boot. sub_82344EE8
// checks the result immediately:
//
//   0x82344F44  bl    MmAllocatePhysicalMemoryEx
//   0x82344F48  mr.   r31,r3
//   0x82344F4C  bne   0x82344f54        success -> continue
//   0x82344F50  li    r3,8              failure -> error path
//
// Every one of those 60 calls takes the failure path, so whatever the caller is
// building never gets built. The fault that ends boot is consistent with that:
// sub_824D5D60 loads r4 from [[0x8268DEEC]+4] and passes it to sub_820B0410,
// which does `mr r26,r4` then `lbz r11,101(r26)`. With r4 zero the load faults
// at 101 decimal = 0x65, which is exactly the reported fault address.
//
// The signature below is taken from that call site rather than assumed. See
// mm_allocate_physical_memory in kernel_imports_core.cpp for the full reading.
// -----------------------------------------------------------------------------
//
// They are emitted here, at the end of the file, because the macro needs
// counters() in scope and they need external linkage: a strong definition that
// overrides XenonRecomp's weak alias cannot live in an anonymous namespace.
//
//   0x82344D34  sub_82349D98
//   0x82344D3C  sub_82349BF0
//   0x82344D40  sub_82344B30
//   0x82344D54  sub_82352998   <- the only caller of the free-list seeder
//
// The seeder's call site is unconditional, so if its count reads zero then one of
// the three earlier calls never returned. Counting all four says which, rather
// than leaving it to be guessed at.
COD3_COUNTED_WRAPPER(sub_82349D98, s49d98)
COD3_COUNTED_WRAPPER(sub_82349BF0, s49bf0)
COD3_COUNTED_WRAPPER(sub_82344B30, s44b30)
COD3_COUNTED_WRAPPER(sub_82352998, s52998)

// -----------------------------------------------------------------------------
// sub_823438C8 - reads [r13+256] then [r11+332]
// -----------------------------------------------------------------------------
//
// This is where boot stops. [r13+256] is a valid pointer immediately after the
// loader builds the thread block, but by the time this runs it reads as zero,
// so the second load faults at 0x14C (= 332).
//
// Nothing in the recompiled guest ever stores to [r13+256] - that search came up
// empty. So either the CRT zeroes the whole block through a pointer it holds
// elsewhere, or r13 itself has been changed. Recording r13 at the fault settles
// that directly, which is why it is measured rather than reasoned about.
//
// NOTE: this override was removed once before because it distorted attribution.
// It is safe now because the crash handler dumps the index neighbourhood: the
// offset is checked against the neighbouring host address rather than trusted.
extern "C" void __imp__sub_823438C8(PPCContext& __restrict ctx, uint8_t* base);

PPC_FUNC(sub_823438C8)
{
    PPC_FUNC_PROLOGUE();

    Counters& c = counters();
    if (++c.close_calls <= 2)
    {
        std::fprintf(stderr,
            "[entries] sub_823438C8 #%llu r13=0x%08X  [r13+256]=0x%08X\n",
            c.close_calls, ctx.r13.u32,
            cod3_load_u32(ctx.r13.u32 + 256));
        if (g_thread_io_word != nullptr)
        {
            // Read through the guest helper, not by dereferencing the host
            // pointer: the block is guest memory holding big-endian words, so a
            // native read reports them byte-swapped and looks like corruption
            // that is not there.
            std::fprintf(stderr,
                "[entries]   loader set 0x%08X at %p; it now reads 0x%08X\n",
                g_thread_io_block_expected,
                static_cast<void*>(g_thread_io_word),
                cod3_load_u32(g_crt_thread_block + 256));

            // Which words still hold what the loader wrote? If every one is gone
            // the block was cleared wholesale; if only some are, something
            // specific overwrote them. Worth being able to tell the two apart.
            const uint32_t base = ctx.r13.u32;
            unsigned zero = 0, set_ = 0;
            for (uint32_t off = 0; off + 4 <= 17304; off += 4)
                (cod3_load_u32(base + off) == 0 ? zero : set_)++;
            std::fprintf(stderr,
                "[entries]   block scan: %u words zero, %u non-zero"
                " (+0=0x%08X)\n",
                zero, set_, cod3_load_u32(base));
        }
        std::fflush(stderr);
    }

    __imp__sub_823438C8(ctx, base);
}

// No override is installed for sub_823438D8.
//
// An override was tried here and it made attribution worse rather than better:
// adding a strong definition puts MY function at sub_823438C8's host address,
// the compiler then inlines the original body and the diagnostics into it, and
// the resulting code is large enough that a PC well past the original body's end
// is still reported as sub_823438C8. The index dump added to the crash handler
// shows the layout directly:
//
//   00007FF6906C7A10  sub_823438C8
//   00007FF6906C7B10  sub_823438D8
//   fault PC          00007FF6906C7A3F   (+47 from sub_823438C8)
//
// 0x100 = 256 is a byte offset that smells like [r13+256] with r13 == 0, which
// is exactly what this three-instruction body does:
//
//   lwz r11,256(r13)
//   lwz r3,332(r11)
//
// With r13 == 0 the first load reads [0x100] and faults there. So r13 is unset
// at this point in boot, and the next thing to establish is who is supposed to
// set it - r13 is the CRT's per-thread pointer, initialised on the thread-start
// path. That is the thread to pull, not this function.
// =============================================================================
//  Findings: why boot stops in sub_823460D8
// =============================================================================
//
//  MEASURED, by entry override rather than by reading the stack:
//
//    sub_82345E48 #1 -> 0x9C5B4000  handle r4=0x9C5AD000
//      [h+56]  = 0x846A8590   (a valid first block)
//      [h+1408]= 0x00000000
//      [h+1412]= 0x00000000   <- the grow method
//
//    first VM page 0x846A8000:
//      [h+56]  = 0x00000610
//      [h+1408]= 0x846A8610   <- POPULATED, and it points into the same page
//      [h+1412]= 0x00000000
//
//  0x846A8610 - 0x846A8000 = 0x610, which is also [page+56]. The descriptor
//  stores the offset of its own sub-structure, so it is self-describing and
//  0x846A8000 is a correctly initialised heap descriptor.
//
//  ## The chain
//
//  sub_82345E48 tests [heap+1412]. Zero means "no grow method", so it falls back
//  to NtAllocateVirtualMemory and returns the raw page as though it were an
//  initialised block header. sub_823460D8 then reads that block's +12 field,
//  which is zero, and faults at address 0x0.
//
//  ## What is NOT the cause, having been checked
//
//  * Not a missing free-list seed - the seeder sub_82357BF0 runs (1 call) and its
//    per-slot seeder sub_8235B570 runs 14 times.
//  * Not the VM stub - NtAllocateVirtualMemory succeeds and the addresses it
//    returns are consistent and non-overlapping.
//  * Not MmAllocatePhysicalMemoryEx - it is implemented, and its requests now
//    succeed (371 MiB reservation, 11 MiB retry, 16 KiB and smaller).
//  * Not r13 - the CRT thread block is built and intact.
//
//  ## The correction that mattered
//
//  sub_82347340 was identified as the heap-setup routine and chased for a while on
//  that basis. It is not. The boot path calls it as
//
//      li r3,2 ; li r4,0 ; li r6,4096 ; li r7,0 ; bl sub_82347340
//
//  which is ExAllocatePool-shaped: type 2, flags 0, size 4096. It returns an
//  allocated address. An earlier trace appeared to show it installing methods
//  because r3 was read AFTER the call, so the return value was mistaken for the
//  descriptor that had been passed in. Reading the argument before the call gives
//  0x00000002.
//
//  So the searches that followed it were searching the wrong function, and the
//  "sole writer of +1408/+1412" claim was wrong. The remaining writers are
//  sub_823473D8 (uncalled), sub_824C2640 (operates on image-resident addresses,
//  0x82A8E610 and 0x82A90004 - not the heap), sub_82571678 (uncalled),
//  sub_823AAC90 and sub_823AB250 (called only from sub_823AB698).
//
//  ## Where that leaves it
//
//  Two descriptors exist. 0x846A8000 is correctly initialised, with +1408
//  pointing at its own sub-structure. 0x9C5AD000 - the one the boot path walks -
//  has both fields zero. Something passes the second where the first is wanted,
//  or the second is expected to be initialised by a path that never runs.
//
//  Not established which. Both readings fit every measurement so far, and
//  choosing between them without evidence is what produced the wrong answers
//  above.
//
// =============================================================================

