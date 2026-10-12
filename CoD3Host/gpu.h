#pragma once

// The GPU register file and command processor.

#include <atomic>
#include <cstdint>

namespace Gpu
{
    // The indirect buffer the command thread is inside, for reports.
    uint32_t CurrentIndirectBuffer();

    // Registers are addressed by their byte address in the aperture, which is
    // what the driver actually writes. These were found by watching what it
    // touched, not assumed: see the discovery reporting in gpu.cpp.
    inline constexpr uint32_t ApertureBase = 0x7FC80000;
    inline constexpr uint32_t RegisterWritePointer = 0x7FC80714;

    // The vertical blank handler reads this and does nothing unless bit 0 is
    // set. Found by disassembling the callback the title installs: on the
    // source 0 path it loads 0x7FC86544, keeps the low bit, and returns
    // immediately when it is clear.
    inline constexpr uint32_t RegisterInterruptStatus = 0x7FC86544;

    uint32_t ReadRegister(uint32_t address);
    // The bin select the command stream set last (the pass a replayed
    // buffer is drawing: 0x80000001 the depth pass, 2 the colour pass).
    uint64_t BinSelect();
    void WriteRegister(uint32_t address, uint32_t value);

    // Where the driver says it has filled the ring, in dwords.
    uint32_t WritePointer();

    // A copy of a range of registers, by index, taken under the lock once.
    void SnapshotRegisters(uint32_t firstIndex, uint32_t count, uint32_t* out);

    // The register file itself, 0x10000 words indexed by register number,
    // for a reader on the command thread that wants no copy; and how many
    // writes the constants have had, by range: 0 the vertex floats, 1 the
    // pixel floats, 2 the booleans and loops.
    const std::atomic<uint32_t>* RegisterFile();

    // With COD3_NATIVECHECK=1: the physical address of the packet that last
    // wrote a register (0 when none did, or the CPU did).
    uint32_t RegisterWriter(uint32_t index);

    // The display's colour table, as the title last wrote it through
    // DC_LUT_RW_INDEX and DC_LUT_30_COLOR: 256 entries of ten bits a
    // channel (blue low, red high), which the console applies to the
    // picture as it goes out. False until the title has written one;
    // the version changes with every write.
    bool DisplayGamma(uint32_t table[256], uint64_t& version);
    uint64_t ConstantWrites(uint32_t range);
    // The span of words written in a range since it was last taken, and
    // whether there was one; taking it clears it.
    bool TakeConstantSpan(uint32_t range, uint32_t& first, uint32_t& end);

    // How many interrupts the command stream has asked for since this was last
    // called, and clears the count. The command thread raises them.
    uint32_t TakePendingInterrupts();

    // Installs the routine that raises an interrupt where the packet stands,
    // on the command thread. Until one is installed they are only counted.
    void SetInterruptRaiser(void (*raiser)(uint32_t cpuMask));

    // Every address a fence has been written to, with a count.
    void ReportFences();

    struct Statistics
    {
        uint64_t packets = 0;
        uint64_t registerWrites = 0;
        uint64_t filler = 0;          // type 0 packets from register zero, skipped
        uint64_t draws = 0;
        uint64_t swaps = 0;
        uint64_t unknown = 0;
        uint32_t readPointer = 0;
    };

    // Consumes what the driver has queued, up to its write pointer or until
    // the budget runs out, whichever comes first. Returns the new read
    // pointer. The budget matters because the work behind a packet is done in
    // software here: without it, one busy buffer stops the vertical blank from
    // happening at all and the whole title slows to the rate this can draw.
    // Called, without any lock held, whenever the title moves the ring's
    // write pointer: the command thread waits on this rather than sleeping
    // and polling, so a submission is picked up at once.
    void SetSubmitHook(void (*hook)());

    uint32_t ProcessRing(uint32_t ringBase, uint32_t ringSizeDwords,
                         uint32_t readPointer, uint32_t writePointer,
                         uint32_t budgetMicroseconds = 0);

    Statistics Stats();

    // The context the title passed to VdSetGraphicsInterruptCallback. Its own
    // ring bookkeeping hangs off this, which is how the fields it spins on can
    // be read from outside.
    uint32_t InterruptContext();

    // Reports a register the driver is reading in a loop, which is how a wait
    // for the GPU shows up from this side.
    void ReportPolling();

    // What kinds of packet the title is actually sending.
    void ReportPacketMix();
}
