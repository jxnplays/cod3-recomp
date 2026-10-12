#pragma once

// The controller.
//
// The title polls for one about five hundred thousand times a second while it
// waits for the player, so this has to be cheap to ask and has to give the same
// answer to every caller within a frame.
//
// A real pad is used when one is plugged in. When there is not, the keyboard
// stands in, so the title can be driven without hardware.

#include <cstdint>

namespace Input
{
    // A pad, in the shape the console reports one: a button mask, two analogue
    // triggers and two sticks. The keyboard and mouse fill the same structure,
    // so nothing downstream needs to know which was used.
    struct Pad
    {
        uint16_t buttons;
        uint8_t leftTrigger;
        uint8_t rightTrigger;
        int16_t thumbLX;
        int16_t thumbLY;
        int16_t thumbRX;
        int16_t thumbRY;
    };

    Pad State();

    // The Xbox button mask the pad is currently reporting.
    uint16_t Buttons();

    // Whether the mouse is being held by the window and turned into the right
    // stick. Clicking in the window takes it; Escape gives it back.
    bool MouseHeld();

    // Changes only when the buttons do. Titles use it to tell a held button
    // from a new press without comparing states themselves.
    uint32_t PacketNumber();

    // Which keys stand in for which buttons, for the status view to show.
    const char* KeyboardHelp();
}
