#pragma once

// Sound leaving the machine.
//
// The title mixes its own audio and hands the finished frame to the console
// once every 5.33 milliseconds: 256 samples for each of six channels, as big
// endian floats, one channel after another rather than interleaved. Everything
// here does is take that frame, fold six channels into two, and give it to
// Windows.
//
// Nothing about this decodes anything. Voices the title fed through the
// console's XMA hardware are still silent, because that hardware is not here;
// what comes out is everything it mixed itself.

#include <cstdint>

namespace Audio
{
    // Opened on the first frame, so a run that never makes a sound never takes
    // the sound device.
    bool Open();
    void Close();

    // One frame from the guest, at the guest address it was submitted at.
    // Returns false if it could not be played, which is not an error worth
    // stopping for: the title is told the frame was accepted either way, since
    // a title that waits for its own audio to drain stops dead.
    bool SubmitFrame(uint32_t guestAddress);

    // How many frames were played and how many had to be dropped because the
    // device had not finished with the ones before.
    void Report();
}
