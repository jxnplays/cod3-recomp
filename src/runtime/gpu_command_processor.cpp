// =============================================================================
//  COD3 Recomp - Xenos GPU command processor
// =============================================================================
//
//  ## Why this exists
//
//  Boot reaches the graphics stack and then writes to 0x7FC80000:
//
//      stw   r29,10820(r26)
//      sync
//      lis   r11,32712          ; r11 = 0x7FC80000
//      stw   r29,1812(r11)      ; write the Xenos command processor
//      eieio
//      sync
//
//  `lis` for the address with `sync`/`eieio` bracketing the access is
//  memory-mapped I/O. 0x7FC80000 is the Xenos GPU command processor, and there
//  is no such device on a PC, so the store faults and the command stream is lost.
//
//  ## Why this is the right place to intercept
//
//  Every MMIO access in the recompiled guest was surveyed: eleven
//  PPC_MM_STORE_U32 in nine functions, no other forms. The ENTIRE Xenos command
//  stream reaches the hardware through those eleven sites.
//
//  So the macro is redirected centrally in ppc_context.h rather than at each
//  call site. The sites are in nine separate functions, and a maintained list of
//  them would break the moment the recompiler output changed.
//
//  ## What is captured, and why it matters more than it looks
//
//  The writes are 32-bit words pushed through the command processor's write
//  port. That stream is the Xenos command sequence - and the vertex and pixel
//  programs travel INSIDE it, as commands, rather than as separate blobs
//  somewhere in the image.
//
//  That is the reason this matters: Xenos bytecode to DXBC, previously treated
//  as an independent hunt for shader blobs across a 12 MB binary, is downstream
//  of capturing this stream. Decode what arrives here and the shaders arrive
//  with it.
//
//  ## What this does NOT do
//
//  It does not render. Nothing here draws a pixel, creates a D3D device, or
//  translates a single Xenos opcode. It captures the stream and reports it.
//
//  It is the measurement step, and it is deliberately that. The alternative -
//  writing a speculative Xenos decoder and attaching it to a stream nobody has
//  looked at - is how a project ends up confident about a format it has not
//  actually observed.
// =============================================================================

#include "ppc_recomp_shared.h"

#include "cod3_runtime.h"

#include <cstdio>

namespace {

// The Xenos command processor register block, and the registers observed so far
// (from tools/mmio_survey.py): 0x0000 is the command write port the ring buffer
// is pushed through, the rest are control registers.
constexpr uint32_t kGpuMmioBase = 0x7FC80000u;
constexpr uint32_t kGpuMmioEnd  = 0x7FC81000u;

constexpr uint32_t kWritePortOffset = 0x0000u;

bool gpu_mmio_range(uint32_t addr)
{
    return addr >= kGpuMmioBase && addr < kGpuMmioEnd;
}

// The capture is a bounded ring so a long boot cannot grow without limit. The
// stream is pushed continuously once rendering starts, so keeping all of it
// would be a leak, not a record.
constexpr size_t kStreamCapacity = 4096;

struct GpuCommand
{
    uint32_t offset;   // command processor register
    uint32_t value;    // the word written
};

GpuCommand g_stream[kStreamCapacity];
size_t     g_streamLen   = 0;
uint64_t   g_totalWrites = 0;
uint64_t   g_otherStores = 0;

} // namespace

// -----------------------------------------------------------------------------
// cod3_mmio_store_u32
// -----------------------------------------------------------------------------
//
// Reached from every PPC_MM_STORE_U32 in the recompiled guest.
//
// The big-endian store has to be reproduced exactly for anything that is not
// GPU MMIO. PPC_STORE_U32 is
//
//     *(volatile uint32_t*)(base + x) = __builtin_bswap32(y)
//
// so a native store would put the bytes the wrong way round and the guest would
// read a byte-swapped value everywhere. cod3_store_u32 does the same swap.
void cod3_mmio_store_u32(uint8_t* base, uint32_t addr, uint32_t value)
{
    if (gpu_mmio_range(addr))
    {
        ++g_totalWrites;

        if (g_streamLen < kStreamCapacity)
        {
            g_stream[g_streamLen].offset = addr - kGpuMmioBase;
            g_stream[g_streamLen].value  = value;
            ++g_streamLen;
        }

        // Report the first few, and then every thousandth, so the shape of the
        // stream is visible without flooding the log. Off by one on purpose:
        // entry 0 is the first word, and counting from 0 reads better here.
        if (g_totalWrites <= 16 || (g_totalWrites % 1000) == 0)
        {
            std::fprintf(stderr,
                "[cod3] gpu +0x%04X <- 0x%08X   (%llu total)\n",
                addr - kGpuMmioBase, value,
                static_cast<unsigned long long>(g_totalWrites));
            std::fflush(stderr);
        }

        // Deliberately NOT stored. There is no device there, and the plain
        // store is what faulted. This is where a D3D backend would consume the
        // word.
        return;
    }

    // Not GPU MMIO: some other device region the guest pokes. Store it as the
    // hardware would, so behaviour is unchanged from before this hook existed.
    ++g_otherStores;
    cod3_store_u32(addr, value);
    (void)base;
}

// -----------------------------------------------------------------------------
// cod3_report_gpu_stream
// -----------------------------------------------------------------------------
//
// Prints what the command processor was asked to do. Called on a crash so a
// fault anywhere downstream can be read against the commands that preceded it.
void cod3_report_gpu_stream()
{
    if (g_totalWrites == 0)
        return;

    std::fprintf(stderr,
        "\n[cod3] Xenos command processor: %llu write(s), "
        "%llu non-GPU MMIO store(s)\n",
        static_cast<unsigned long long>(g_totalWrites),
        static_cast<unsigned long long>(g_otherStores));

    // Count words per register, which distinguishes a stream that is mostly
    // command-port traffic from one that is mostly poking control registers.
    uint32_t perRegister[16] = {};
    uint32_t writesToPort = 0;

    for (size_t i = 0; i < g_streamLen; ++i)
    {
        const uint32_t reg = g_stream[i].offset / 4u;
        if (reg < 16)
            ++perRegister[reg];

        if (g_stream[i].offset == kWritePortOffset)
            ++writesToPort;
    }

    std::fprintf(stderr,
        "[cod3]   %u word(s) through the command write port (+0x0000)\n",
        writesToPort);

    for (uint32_t reg = 0; reg < 16; ++reg)
    {
        if (perRegister[reg] == 0)
            continue;
        std::fprintf(stderr, "[cod3]   register +0x%04X : %u write(s)\n",
                     reg * 4u, perRegister[reg]);
    }

    std::fflush(stderr);
}
