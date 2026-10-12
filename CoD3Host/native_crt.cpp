// The title's C runtime routines done by the host instead of by their
// recompiled code. Each replaces one recompiled function through its weak
// alias; the executable and every level DLL link their own copy of the
// runtime, so each copy is named here.

#include "kernel.h"

#include <cstdlib>
#include <cstring>

// --- memset ----------------------------------------------------------------------------------------
//
// memset(destination r3, byte r4, count r5), returning the destination. The
// title's version stores bytes up to a word boundary, then sixteen bytes a
// turn, then words, and ends on the last one to three bytes:
//
//     stb r4,0(r6) / bdzlr / stb r4,1(r6) / bdzlr / stb r4,2(r6) / blr
//
// The analyser ended the function at the first bdzlr and made each of the
// two tails a function of its own that nothing calls, so the recompiled
// memset wrote only the first of the last two or three bytes. A length of
// 74 left its 74th byte as it was: that is how an animation tree of 25
// animations (74 bytes) came out of sub_824FD368 with its last slot holding
// an old byte, a record of the animation pool it never took. When the
// restart then gave back every record the trees held (sub_824F7F90), that
// one went on the free list a second time, the list closed into a loop,
// and from then on the pool handed the same few records to every soldier:
// one soldier's animation ending took away another's, and the soldiers
// stood in their bind pose.
//
// The host's memset does the same thing whole. COD3_TITLEMEMSET=1 runs the
// recompiled one instead, to compare.
namespace
{
    bool TitleMemset()
    {
        static const bool wanted = getenv("COD3_TITLEMEMSET") != nullptr;
        return wanted;
    }

    inline void NativeMemset(PPCContext& ctx, uint8_t* base)
    {
        const uint32_t count = ctx.r5.u32;
        if (count != 0) memset(base + ctx.r3.u32, ctx.r4.u8, count);
    }
}

#define NATIVE_MEMSET(name)                                  \
    extern "C" PPC_FUNC(__imp__##name);                      \
    PPC_FUNC(name)                                           \
    {                                                        \
        if (TitleMemset()) { __imp__##name(ctx, base); return; } \
        NativeMemset(ctx, base);                             \
    }

NATIVE_MEMSET(sub_8234EBA0)
NATIVE_MEMSET(blkbrn_sub_891FBC60)
NATIVE_MEMSET(chambois_sub_8921C5E0)
NATIVE_MEMSET(credits_sub_891CFF10)
NATIVE_MEMSET(crssrds_sub_89221900)
NATIVE_MEMSET(falaise_sub_8920F380)
NATIVE_MEMSET(forest_sub_891FBC40)
NATIVE_MEMSET(fuelplnt_sub_892015E0)
NATIVE_MEMSET(hostage_sub_89216C30)
NATIVE_MEMSET(island_sub_89227FC0)
NATIVE_MEMSET(laison_sub_89218290)
NATIVE_MEMSET(mace2_sub_89245100)
NATIVE_MEMSET(mayenne_sub_8920E820)
NATIVE_MEMSET(nightd_sub_89203380)
NATIVE_MEMSET(saint_lo_sub_89236020)
NATIVE_MEMSET(stbert_sub_89209D90)
