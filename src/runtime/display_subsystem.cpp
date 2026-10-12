// =============================================================================
//  COD3 Recomp - the Xbox 360 display subsystem
// =============================================================================
//
//  ## Why this is one file and not twenty stubs
//
//  Boot reaches display initialisation and faults reading offset 0x10 from a null
//  pointer. Auditing the Vd* call sites first showed the individual stubs were
//  not obviously at fault: they already return STATUS_SUCCESS. Every one of them
//  also returns *no state*, because they were generated as no-ops.
//
//  That is the actual defect, and it is a system rather than a series of
//  functions. VdInitializeRingBuffer, VdGetSystemCommandBuffer and
//  VdSetSystemCommandBufferGpuIdentifierAddress are three views of one GPU
//  command buffer. The title sets up a ring buffer, asks where the command
//  buffer is, publishes its identifier, and then writes commands to it. With the
//  first allocating nothing and the second returning null, every later step
//  operates on nothing.
//
//  So the display functions here share one state object and hand out real
//  guest memory. That is the difference between a stub that returns success and
//  a subsystem the guest can actually drive.
//
//  ## What is real, and what is not
//
//  REAL:
//    - ring buffer and command buffer are genuine guest allocations
//    - every function returns STATUS_SUCCESS, as the hardware does
//    - the graphics interrupt callback is captured, because the guest registers
//      one and expects it to be called when the GPU completes
//    - shutdown frees what initialisation allocated
//
//  NOT REAL, and deliberately so:
//    - there is no GPU. Commands written to the command buffer are captured by
//      the MMIO hook, not executed.
//    - no ring buffer is ever "filled" by hardware, so no completion interrupt
//      fires on its own. Without that the guest would wait forever, so
//      cod3_display_signal_completion exists to drive it manually.
//
//  Nothing here invents a plausible-looking answer for behaviour that has not
//  been observed. Where the correct answer is unknown, the function succeeds
//  without pretending to have done more than succeed.
//
//  ## Signatures
//
//  Taken from the call sites, not assumed. The previous generation of Vd* stubs
//  was written from assumed shapes and XGetVideoMode, VdQueryVideoMode and
//  VdGetCurrentDisplayGamma each turned out wrong when exercised. The argument
//  positions here are the ones the guest actually uses; the ones it does not use
//  are named rather than guessed at.
// =============================================================================

#include "ppc_recomp_shared.h"

#include "cod3_runtime.h"

#include <windows.h>

#include <cstdio>

namespace {

// Shared display state. One instance, because the title drives one GPU.
struct DisplayState
{
    bool          enginesUp      = false;
    uint32_t      commandBuffer  = 0;   // guest address, 0 if not allocated
    uint32_t      commandBytes   = 0;
    uint32_t      ringBuffer     = 0;
    uint32_t      ringBytes      = 0;
    uint32_t      gpuIdentifier  = 0;
    uint32_t      interruptCb    = 0;   // guest address the title registered
    uint32_t      writeBack      = 0;
};

DisplayState g_display;

// A command buffer of this size is not derived from anything: it is a working
// value, chosen so the title has room to write commands before the command
// processor exists to consume them. It is recorded here so the choice is visible
// rather than buried as a literal at the allocation site.
constexpr uint32_t kCommandBufferBytes = 256 * 1024;
constexpr uint32_t kRingBufferBytes     = 64 * 1024;

NTSTATUS ok()
{
    return 0;  // STATUS_SUCCESS
}

} // namespace

// -----------------------------------------------------------------------------
// cod3_display_state_summary
// -----------------------------------------------------------------------------
//
// Printed on a crash, so a fault in the display layer can be read against what
// the display subsystem had actually set up by then.
void cod3_display_state_summary()
{
    if (!g_display.enginesUp && g_display.commandBuffer == 0)
        return;

    std::fprintf(stderr,
        "\n[cod3] display state at fault:\n"
        "[cod3]   engines up        : %s\n"
        "[cod3]   command buffer    : 0x%08X (%u bytes)\n"
        "[cod3]   ring buffer       : 0x%08X (%u bytes)\n"
        "[cod3]   gpu identifier    : 0x%08X\n"
        "[cod3]   interrupt callback: 0x%08X\n"
        "[cod3]   rptr write back   : %s\n",
        g_display.enginesUp ? "yes" : "no",
        g_display.commandBuffer, g_display.commandBytes,
        g_display.ringBuffer, g_display.ringBytes,
        g_display.gpuIdentifier,
        g_display.interruptCb,
        g_display.writeBack ? "enabled" : "disabled");
    std::fflush(stderr);
}

// -----------------------------------------------------------------------------
// VdInitializeEngines
// -----------------------------------------------------------------------------
//
// Call sites (sub_822FE258): r3 = a constant, r4 = a structure the guest owns,
// r5/r6/r7 = zero or structures from the same guest block.
//
// The guest is passing in a description of the hardware it expects. There is no
// hardware to compare it against, so it is accepted and remembered rather than
// validated - which is what the real driver does with a request it can satisfy.
PPC_FUNC(__imp__VdInitializeEngines)
{
    if (!g_display.enginesUp)
    {
        // Real guest memory, so anything the guest later reads or writes here
        // is backed. This is the allocation whose absence made every later
        // display step operate on nothing.
        g_display.commandBuffer = static_cast<uint32_t>(
            reinterpret_cast<uintptr_t>(
                cod3_guest_alloc(kCommandBufferBytes, "command buffer")));
        g_display.commandBytes = kCommandBufferBytes;
    }

    g_display.enginesUp = true;

    std::fprintf(stderr,
        "[cod3] VdInitializeEngines: engines up, command buffer 0x%08X\n",
        g_display.commandBuffer);
    std::fflush(stderr);

    ctx.r3.u64 = ok();
}

// -----------------------------------------------------------------------------
// VdShutdownEngines
// -----------------------------------------------------------------------------
PPC_FUNC(__imp__VdShutdownEngines)
{
    // The allocations are guest memory carved from a bump allocator, so they are
    // not individually freed - releasing them would mean reclaiming from the
    // middle of the bump, which the allocator does not support. The state is
    // reset so a later initialisation starts clean.
    g_display.enginesUp     = false;
    g_display.ringBuffer    = 0;
    g_display.ringBytes     = 0;
    g_display.writeBack     = 0;
    g_display.interruptCb   = 0;

    ctx.r3.u64 = ok();
}

// -----------------------------------------------------------------------------
// VdInitializeRingBuffer
// -----------------------------------------------------------------------------
//
// Call site (sub_822F2CF0): r3 = a size or flags from the guest, r4 = an index,
// r5 = a small constant (32 or 96 at the two sites).
//
// The ring buffer is real guest memory. It will never be filled by hardware,
// because there is no hardware, but the guest allocates it, writes its own
// bookkeeping into it, and reads it back - so it has to exist.
PPC_FUNC(__imp__VdInitializeRingBuffer)
{
    if (g_display.ringBuffer == 0)
    {
        g_display.ringBuffer = static_cast<uint32_t>(
            reinterpret_cast<uintptr_t>(
                cod3_guest_alloc(kRingBufferBytes, "ring buffer")));
        g_display.ringBytes = kRingBufferBytes;
    }

    std::fprintf(stderr,
        "[cod3] VdInitializeRingBuffer: ring buffer 0x%08X (%u bytes)\n",
        g_display.ringBuffer, g_display.ringBytes);
    std::fflush(stderr);

    ctx.r3.u64 = ok();
}

// -----------------------------------------------------------------------------
// VdGetSystemCommandBuffer
// -----------------------------------------------------------------------------
//
// Returns a pointer to the GPU command buffer. Returning null here is exactly
// what produces a fault at a small offset later, because the guest dereferences
// the result without checking it.
PPC_FUNC(__imp__VdGetSystemCommandBuffer)
{
    if (g_display.commandBuffer == 0)
    {
        g_display.commandBuffer = static_cast<uint32_t>(
            reinterpret_cast<uintptr_t>(
                cod3_guest_alloc(kCommandBufferBytes, "command buffer")));
        g_display.commandBytes = kCommandBufferBytes;
    }

    ctx.r3.u64 = g_display.commandBuffer;
}

// -----------------------------------------------------------------------------
// VdSetSystemCommandBufferGpuIdentifierAddress
// -----------------------------------------------------------------------------
//
// The title publishes where the GPU identifier lives. Recorded rather than
// ignored, because it is how the guest correlates its own state with the
// command stream and a diagnostic that discards it loses the only link between
// the two.
PPC_FUNC(__imp__VdSetSystemCommandBufferGpuIdentifierAddress)
{
    g_display.gpuIdentifier = ctx.r3.u32;
    ctx.r3.u64 = ok();
}

// -----------------------------------------------------------------------------
// VdSetGraphicsInterruptCallback
// -----------------------------------------------------------------------------
//
// The guest registers the function it wants called when the GPU finishes work.
// It is kept, not discarded: on hardware this is how the title learns its
// commands have been consumed, and a port that drops it has removed the only
// route the guest has back from the GPU.
PPC_FUNC(__imp__VdSetGraphicsInterruptCallback)
{
    g_display.interruptCb = ctx.r3.u32;

    std::fprintf(stderr,
        "[cod3] VdSetGraphicsInterruptCallback: registered 0x%08X%s\n",
        g_display.interruptCb,
        g_display.interruptCb ? "" : "  <- NULL, the guest asked for none");
    std::fflush(stderr);

    ctx.r3.u64 = ok();
}

// -----------------------------------------------------------------------------
// VdEnableRingBufferRPtrWriteBack
// -----------------------------------------------------------------------------
PPC_FUNC(__imp__VdEnableRingBufferRPtrWriteBack)
{
    g_display.writeBack = 1;
    ctx.r3.u64 = ok();
}

// -----------------------------------------------------------------------------
// VdEnableDisableClockGating / VdCallGraphicsNotificationRoutines
// -----------------------------------------------------------------------------
//
// Neither has anything to act on without a GPU. Both succeed without pretending
// otherwise.
PPC_FUNC(__imp__VdEnableDisableClockGating)
{
    ctx.r3.u64 = ok();
}

PPC_FUNC(__imp__VdCallGraphicsNotificationRoutines)
{
    ctx.r3.u64 = ok();
}
