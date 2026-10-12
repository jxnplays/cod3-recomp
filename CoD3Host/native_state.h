#pragma once

#include <cstdint>
#include <vector>

// The native path's draw state (docs/native-d3d11-plan.md, N1): a draw's
// state as the title's D3D device and its own command stream had it when
// the draw call was made, not as the register file has it when the command
// processor gets there.
//
// The device keeps a copy of every register it writes (the shadows the
// dirty masks send out before each draw); what else its stream sets (the
// title's own material blocks, constants from memory, the Z pass's own
// state) is read from the stream as it is written (native_state.cpp).
// Recorded keeps a draw's state by its packet's last word, and Check holds
// it against the register file when the command processor reaches the
// packet: the parity that has to hold before the draws can be made from
// these records alone.
namespace NativeState
{
    // COD3_NATIVECHECK=1: the draws' states are recorded and checked.
    bool Checking();

    // COD3_NATIVE=1: the draws are made from their records (N2): the
    // registers, constants and programs, not the register file's.
    bool Drawing();

    // The command processor is at a draw packet whose last word is at that
    // physical address: with COD3_NATIVE=1 and a record of it, the record
    // becomes what the draw is made from (RenderState::UseRegisters, the
    // programs loaded) until End. False when the register file stays.
    bool Begin(uint32_t lastWordPhysical);
    void End();

    // Between Begin and End: every register of the draw's image that is
    // not the register file's, told one by one; how many.
    int DiffImage(void (*tell)(uint32_t reg, uint32_t mine, uint32_t file));

    // COD3_NATIVE=2 (N3): the buffers the devices' streams were read over
    // are run from their native lists: only the packets the command
    // processor still has to run (the draws, made from their records; the
    // waits, events, interrupts, swaps, bin masks; register writes outside
    // a draw's state), none of the state the records already hold.
    bool Executing();

    // The packets of an indirect buffer at that physical address that are
    // still to run, in order: as far as its packets follow each other as a
    // device's stream was read, each still as it was and each draw with its
    // record. coveredDwords: how far that was; the rest (the end record the
    // kick writes after what was read) is run as it is, unless it draws.
    // False: the buffer is run whole.
    struct Item { uint32_t physical; uint32_t dwords; };
    bool NativeRun(uint32_t physical, uint32_t dwords, std::vector<Item>& items, uint32_t& coveredDwords);

    // A draw call on `device` has written its packets after `before` (the
    // device's write pointer before the call, on the last word written then)
    // up to `after` (on the last word it wrote).
    void Recorded(uint8_t* base, uint32_t device, uint32_t before, uint32_t after);

    // The device's segment is about to be kicked (sub_822F2818): what is in
    // it is read to its end. And it has been, or a new segment has been
    // started (sub_822F2678): the model goes on from the write pointer in
    // the next.
    void Kicking(uint8_t* base, uint32_t device);
    void Kicked(uint8_t* base, uint32_t device);

    // The command processor reached a draw packet whose last word is at
    // that physical address: its state from the device, held against the
    // register file's.
    void Check(uint32_t lastWordPhysical, uint32_t initiator, uint32_t indexBase);
}
