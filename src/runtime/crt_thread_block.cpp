// =============================================================================
//  COD3 Recomp - the CRT thread block (r13)
// =============================================================================
//
//  ## Why this exists
//
//  PowerPC on Xbox 360 reserves r13 as a per-thread data base. The CRT keeps the
//  thread's file-descriptor table, its errno slot and its locale state in a block
//  r13 points at, and reads them through fixed displacements. The guest uses only
//  these offsets:
//
//      [r13 + 0]      a pointer, read 13 times
//      [r13 + 256]    a pointer to a sub-block, read 11 times
//      [r13 + 268]    a byte flag, read 10 times
//      [r13 + 336]    a word flag, read 3 times
//      [r13 + 17288]  a word, read 4 times
//      [r13 + 17300]  a word, read 2 times
//
//  ## How the requirement was established
//
//  The first boot after the CRT heap deadlock was fixed stopped here:
//
//      guest addr : 0x823438C8  (sub_823438C8, +8 bytes)
//      fault addr : 0x100
//
//      sub_823438C8:
//        lwz r11,256(r13)
//        lwz r3,332(r11)     <- faults, r11 == 0
//
//  So [r13+256] read as zero and the second load dereferenced 0x100.
//
//  Decisively: only two functions in the whole recompiled image ever WRITE r13,
//  and neither runs during boot. They are thread trampolines that restore a
//  saved register file - sub_8234EEC0 does `ld r13,152(r7)` and
//  sub_824A6150 does `ld r13,88(r1)`. Nothing on the boot path assigns r13, so
//  it has to arrive already set. On hardware the XEX loader creates the initial
//  thread and passes this block to the CRT before _xstart runs. Building it is
//  loader work, so it belongs in the runtime rather than in a kernel import.
//
//  ## Layout
//
//  The two blocks are separate because the guest treats them separately.
//  [r13+256] points at the file/errno sub-block; offsets within it (88, 332, 352)
//  are relative to that, not to r13. The outer block is at least 17,304 bytes
//  because of the 17288 and 17300 accesses.
//
//  Nothing in the boot path touches offsets above +352, so the high part of the
//  outer block is left zeroed rather than guessed at. A wrong guess would be
//  worse than a zero: the guest would read a plausible-looking value that is not
//  what the loader would have supplied.
//
// =============================================================================

#include "ppc_recomp_shared.h"

#include "cod3_runtime.h"

#include <cstdio>

namespace {

// Offsets in the outer thread block. Named rather than written inline so the
// table above and the code that fills it cannot drift apart.
constexpr uint32_t kThreadDataPtr      = 0;      // [r13+0]
constexpr uint32_t kThreadIoBlock      = 256;    // [r13+256]
constexpr uint32_t kThreadFlagByte     = 268;    // [r13+268]
constexpr uint32_t kThreadErrnoEnabled = 336;    // [r13+336]

// Offsets inside the io sub-block reached through [r13+256].
constexpr uint32_t kIoField88          = 88;
constexpr uint32_t kIoHandleTable      = 332;    // the handle NtClose reads
constexpr uint32_t kIoErrnoSlot        = 352;    // the handle _errno writes

// The outer block has to reach at least r13+17300.
constexpr uint32_t kThreadBlockBytes = 17304;

// The io sub-block has to reach +352.
constexpr uint32_t kIoBlockBytes = 512;

// "No handle". sub_82345A58 stores an errno here, and sub_823438C8 reads a handle
// from the table at +332 to pass to NtClose. Seeding the slot with an invalid
// handle means NtClose is asked to close something that was never opened, which
// is the state a real CRT starts in: no standard streams open yet. NtClose is
// expected to reject it, and the caller only checks the status.
constexpr uint32_t kInvalidHandle = 0xFFFFFFFFu;

} // namespace

// -----------------------------------------------------------------------------
// cod3_populate_crt_thread_block
// -----------------------------------------------------------------------------
//
// Allocates both blocks from the guest heap and publishes them.
//
// COD3_TRACE_TLS=1 prints what was published.
void cod3_populate_crt_thread_block()
{
    auto* outer = static_cast<uint8_t*>(
        cod3_pool_alloc(kThreadBlockBytes, 'CRTT', 0));
    auto* io = static_cast<uint8_t*>(
        cod3_pool_alloc(kIoBlockBytes, 'CRTI', 0));

    if (outer == nullptr || io == nullptr)
    {
        std::fprintf(stderr,
            "[cod3] FATAL: could not allocate the CRT thread block"
            " (outer=%p io=%p); r13 cannot be published.\n",
            static_cast<void*>(outer), static_cast<void*>(io));
        return;
    }

    // The pool hands out zeroed memory, but say so explicitly: every field below
    // is a deliberate value, and a future change to the allocator's behaviour
    // should not silently alter them.
    __builtin_memset(outer, 0, kThreadBlockBytes);
    __builtin_memset(io, 0, kIoBlockBytes);

    auto store32 = [](uint8_t* block, uint32_t off, uint32_t value) {
        block[off + 0] = static_cast<uint8_t>(value >> 24);
        block[off + 1] = static_cast<uint8_t>(value >> 16);
        block[off + 2] = static_cast<uint8_t>(value >> 8);
        block[off + 3] = static_cast<uint8_t>(value);
    };

    // [r13+0]: read 13 times by code that then dereferences it. It is the CRT's
    // own data pointer. Point it at the outer block so a dereference lands on
    // zeroed, valid memory rather than on a wild address.
    store32(outer, kThreadDataPtr,
            static_cast<uint32_t>(reinterpret_cast<uintptr_t>(outer)));

    // [r13+256]: the io/errno sub-block.
    //
    // COD3_TRACE_TLS=2 installs a guard page immediately after this word, so a
    // store that walks off the front of the block faults at the address that did
    // it rather than silently zeroing the pointer. The value is correct right
    // after this function returns but reads back as zero by the time the CRT
    // reaches sub_823438C8, and no instruction in the recompiled guest stores to
    // r13+256 directly - so something clears it through a pointer held
    // elsewhere. The guard turns that silent zeroing into a report instead of
    // leaving it to be inferred.
    store32(outer, kThreadIoBlock,
            static_cast<uint32_t>(reinterpret_cast<uintptr_t>(io)));

    if (std::getenv("COD3_TRACE_TLS") != nullptr &&
        std::getenv("COD3_TRACE_TLS")[0] == '2')
    {
        g_thread_io_word = reinterpret_cast<uint32_t*>(outer + kThreadIoBlock);
        std::fprintf(stderr,
            "[cod3] watch on [r13+256] at %p (value 0x%08X)\n",
            static_cast<void*>(g_thread_io_word),
            *g_thread_io_word);
    }

    // [r13+268]: a byte flag read 10 times. No boot-path code reads it and its
    // meaning is not recoverable from the access sites alone, so it stays zero -
    // "not set" is the state a fresh thread starts in.

    // [r13+336]: read by sub_82345A58, which skips the errno write when this is
    // non-zero. Zero means errors ARE recorded, which is the correct initial
    // state and what makes a later failure diagnosable.
    store32(outer, kThreadErrnoEnabled, 0);

    // Inside the io block:
    //   +88   read by sub_822E8500 (follows a pointer chain)
    //   +332  the handle sub_823438C8 passes to NtClose
    //   +352  the errno slot sub_82345A58 writes
    store32(io, kIoField88, 0);
    store32(io, kIoHandleTable, kInvalidHandle);
    store32(io, kIoErrnoSlot, 0);

    g_crt_thread_block = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(outer));
    g_thread_io_block_expected =
        static_cast<uint32_t>(reinterpret_cast<uintptr_t>(io));

    if (std::getenv("COD3_TRACE_TLS") != nullptr)
    {
        std::fprintf(stderr,
            "[cod3] CRT thread block: r13 base=0x%08X (0x%X bytes)\n"
            "[cod3]                  [r13+0]   = 0x%08X\n"
            "[cod3]                  [r13+256] = 0x%08X (0x%X bytes)\n"
            "[cod3]                  [r13+268] = 0x%02X\n"
            "[cod3]                  [r13+336] = 0x%08X\n"
            "[cod3]   io[+88]  = 0x%08X  io[+332] = 0x%08X  io[+352] = 0x%08X\n",
            g_crt_thread_block, kThreadBlockBytes,
            cod3_load_u32(g_crt_thread_block + kThreadDataPtr),
            cod3_load_u32(g_crt_thread_block + kThreadIoBlock), kIoBlockBytes,
            // The +268 slot is a byte. Reading it back through the word helper
            // would report four bytes it does not own, so mask to the byte.
            cod3_load_u32(g_crt_thread_block + kThreadFlagByte) & 0xFFu,
            cod3_load_u32(g_crt_thread_block + kThreadErrnoEnabled),
            cod3_load_u32(
                cod3_load_u32(g_crt_thread_block + kThreadIoBlock) + kIoField88),
            cod3_load_u32(
                cod3_load_u32(g_crt_thread_block + kThreadIoBlock) + kIoHandleTable),
            cod3_load_u32(
                cod3_load_u32(g_crt_thread_block + kThreadIoBlock) + kIoErrnoSlot));
    }
}

// -----------------------------------------------------------------------------
// cod3_crt_thread_block
// -----------------------------------------------------------------------------
//
// The value the runtime publishes into r13 before calling the guest entry point.
uint32_t cod3_crt_thread_block()
{
    return g_crt_thread_block;
}