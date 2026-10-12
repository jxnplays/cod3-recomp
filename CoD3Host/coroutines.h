#pragma once

// The title's script threads, as host fibers.
//
// A level's script threads are coroutines. The engine gives each one a
// stack region below its own frame; to run one it copies the thread's saved
// stack back into that region, loads the thread's registers from the frame
// at the top of it and branches to where the thread left off. The thread
// yields by saving its registers to that frame, copying its stack out to a
// chain of chunks and longjmp'ing to the engine's setjmp. The engine's
// half of this is fine on the host: setjmp and longjmp are the host's own.
// The other half is not: "branch to where the thread left off" is a jump
// into the middle of a function, which the recompiled code cannot do.
//
// So each script thread gets a host fiber. The engine's resume point hands
// the fiber the registers the engine loaded and switches to it; a fresh
// thread's fiber calls the thread's entry function, a suspended one is
// already inside its yield and simply continues. The thread's yield, seen
// at the longjmp, switches back to the engine's fiber instead, which then
// performs the longjmp the thread asked for. The guest's own copying of
// the stack in and out stays as it is: the fiber's frames refer to the
// same guest stack addresses, and the engine restores the same bytes there.

#include <cstdint>
#include <csetjmp>

#include <ppc_context.h>

namespace Coroutines
{
    // A thread's first run: the engine's call of the thread's start
    // function, made on the thread's own fiber. Returns when the thread
    // has yielded or finished; a yield's longjmp is performed on the way.
    void Begin(PPCContext& ctx, uint8_t* base, uint32_t key, PPCFunc* entry);

    // The engine's resume point, in place of its branch: the registers of
    // the thread keyed by `key` are in ctx. Continues the thread's fiber
    // from its yield and returns as Begin does.
    void Resume(PPCContext& ctx, uint8_t* base, uint32_t key);

    // From the title's longjmp: on a thread's fiber the longjmp is a yield,
    // done here, and this returns when the thread is resumed. On the
    // engine's fiber it returns false and the longjmp is the caller's.
    bool YieldIfCoroutine(jmp_buf& buffer, int value);

    // Whether the thread keyed by `key` ran to the end of its start
    // function the last time it was switched to.
    bool Finished(uint32_t key);

    // The thread keyed by `key` is gone; its fiber goes with it.
    void Destroy(uint32_t key);

    // Whether this host thread is currently running a script thread.
    bool Inside();

    // The game module is shut down (a level's end, a restart after a
    // death): the threads begun from here on are of a new generation, and
    // a resume of a thread whose fiber is of an older one is reported.
    void NewGeneration();
}
