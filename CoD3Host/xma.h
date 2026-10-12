// XMA decoding for the console's audio contexts, through FFmpeg's xma2
// decoder when its libraries are on the machine.
//
// FFmpeg is not linked: avcodec and avutil are loaded at run time from
// beside the executable, from C:\ffmpeg\bin, or from the path, and when they
// are not there every context decodes to silence, as it did before. The
// headers are needed at build time (CMake looks for them; without them this
// is the silent version throughout).
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace Xma
{
    // Whether a decoder can be made at all: the libraries loaded.
    bool Available();

    // Why not, for the log, when Available() is false.
    const char* Unavailable();

    struct Stream;

    // One XMA2 stream: one or two channels at the sample rate, fed the
    // title's 2048 byte packets in order.
    Stream* Create(int channels, int sampleRate);
    void Destroy(Stream* stream);

    // One packet of 2048 bytes, as it lies in guest memory. The samples it
    // yields (whole frames of 512 a channel, interleaved, host order) go on
    // the end of `out`. Returns false when the decoder refused the packet.
    bool Decode(Stream* stream, const uint8_t* packet, std::vector<int16_t>& out);

    // Forgets what came before: the stream starts again at its next packet.
    void Reset(Stream* stream);
}
