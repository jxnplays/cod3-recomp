#include "audio_out.h"
#include "kernel.h"

#include <algorithm>
#include <cmath>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <chrono>

#include <Windows.h>
#include <mmsystem.h>

namespace
{
    // The shape of a frame, fixed by the console: 256 samples a channel, six
    // channels, 48 kHz.
    constexpr uint32_t SamplesPerChannel = 256;
    constexpr uint32_t Channels = 6;
    constexpr uint32_t SampleRate = 48000;

    // Eight frames in flight is about forty three milliseconds of sound. Fewer
    // and the device runs dry whenever the title misses a frame; more and the
    // sound lags visibly behind what is on screen.
    constexpr int Buffers = 8;

    std::mutex g_mutex;
    HWAVEOUT g_device = nullptr;
    bool g_tried = false;

    struct Block
    {
        WAVEHDR header{};
        int16_t samples[SamplesPerChannel * 2]{};   // stereo
    };
    Block g_blocks[Buffers];
    int g_next = 0;

    std::atomic<uint64_t> g_played{ 0 };
    std::atomic<uint64_t> g_dropped{ 0 };

    // The loudest sample that has gone out, so a run that plays perfect silence
    // can be told apart from one that plays sound nobody can hear. If the title
    // mixes nothing but its XMA voices, and that decoder is not here, every
    // sample leaving this file is a zero and the speakers are correctly silent.
    std::atomic<int> g_peak{ 0 };
    std::atomic<uint32_t> g_nonzeroBytes{ 0 };   // the most of a frame's bytes ever seen nonzero

    // COD3_AUDIODUMP=path: everything that goes to the device, as a stereo
    // 48 kHz WAV, for listening to a run after it or looking at the wave.
    // The header's sizes are brought up to date every so often, so a run
    // cut short still leaves a file that plays.
    FILE* g_dump = nullptr;
    uint32_t g_dumpBytes = 0;
    uint32_t g_dumpFrames = 0;

    void DumpHeader()
    {
        uint8_t header[44] = {};
        auto put32 = [&](int at, uint32_t v) { header[at] = uint8_t(v); header[at + 1] = uint8_t(v >> 8); header[at + 2] = uint8_t(v >> 16); header[at + 3] = uint8_t(v >> 24); };
        auto put16 = [&](int at, uint32_t v) { header[at] = uint8_t(v); header[at + 1] = uint8_t(v >> 8); };
        memcpy(header, "RIFF", 4); put32(4, 36 + g_dumpBytes); memcpy(header + 8, "WAVEfmt ", 8);
        put32(16, 16); put16(20, 1); put16(22, 2); put32(24, SampleRate); put32(28, SampleRate * 4); put16(32, 4); put16(34, 16);
        memcpy(header + 36, "data", 4); put32(40, g_dumpBytes);
        fseek(g_dump, 0, SEEK_SET);
        fwrite(header, 1, sizeof(header), g_dump);
        fseek(g_dump, 0, SEEK_END);
    }

    void Dump(const int16_t* samples, size_t bytes)
    {
        static const char* const path = getenv("COD3_AUDIODUMP");
        if (path == nullptr) return;
        if (g_dump == nullptr)
        {
            g_dump = fopen(path, "wb");
            if (g_dump == nullptr) return;
            DumpHeader();
        }
        fwrite(samples, 1, bytes, g_dump);
        g_dumpBytes += uint32_t(bytes);
        if (++g_dumpFrames % 64 == 0) { DumpHeader(); fflush(g_dump); }
    }

    // A big endian float out of guest memory.
    float GuestFloat(uint32_t address)
    {
        const uint32_t bits = Guest::Read32(Guest::Base, address);
        float value;
        memcpy(&value, &bits, sizeof(value));
        return value;
    }

    int16_t ToPcm(float value)
    {
        // A little headroom, because six channels folded into two can add up to
        // more than one and clipping is far more audible than being quiet.
        const float scaled = value * 0.8f * 32767.0f;
        return int16_t(std::clamp(scaled, -32768.0f, 32767.0f));
    }
}

bool Audio::Open()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_device != nullptr) return true;
    if (g_tried) return false;
    g_tried = true;

    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 2;
    format.nSamplesPerSec = SampleRate;
    format.wBitsPerSample = 16;
    format.nBlockAlign = format.nChannels * format.wBitsPerSample / 8;
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;

    const MMRESULT opened = waveOutOpen(&g_device, WAVE_MAPPER, &format,
                                        0, 0, CALLBACK_NULL);
    if (opened != MMSYSERR_NOERROR)
    {
        g_device = nullptr;
        printf("audio: no output device, the run will be silent (error %u)\n",
            unsigned(opened));
        fflush(stdout);
        return false;
    }

    for (Block& block : g_blocks)
    {
        block.header.lpData = reinterpret_cast<LPSTR>(block.samples);
        block.header.dwBufferLength = sizeof(block.samples);
        waveOutPrepareHeader(g_device, &block.header, sizeof(WAVEHDR));
        // Marked done so the first pass through finds them all free.
        block.header.dwFlags |= WHDR_DONE;
    }

    printf("audio: playing the title's own mix, 48 kHz stereo\n");
    fflush(stdout);
    return true;
}

void Audio::Close()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_device == nullptr) return;

    waveOutReset(g_device);
    for (Block& block : g_blocks)
        waveOutUnprepareHeader(g_device, &block.header, sizeof(WAVEHDR));
    waveOutClose(g_device);
    g_device = nullptr;
}

bool Audio::SubmitFrame(uint32_t guestAddress)
{
    if (guestAddress == 0) return false;
    if (!Open()) return false;

    std::unique_lock<std::mutex> lock(g_mutex);
    if (g_device == nullptr) return false;

    // The next buffer in the ring, once the device has finished with it.
    // The frames come from this runtime's own pump thread, so a short wait
    // here is how the pump keeps the device's time rather than its own:
    // a frame a second was dropped with a click when the two drifted
    // apart. Longer than a few frames' worth and the frame is dropped
    // rather than the pump held.
    Block& block = g_blocks[g_next];
    for (int waited = 0; (block.header.dwFlags & WHDR_DONE) == 0; waited++)
    {
        if (waited >= 20)
        {
            g_dropped.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        lock.unlock();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        lock.lock();
    }
    g_next = (g_next + 1) % Buffers;

    // Six channels, one after another in memory, in the console's order: left,
    // right, centre, low frequency, left surround, right surround. Folded into
    // two the usual way, with the centre and the surrounds shared between them
    // at about seven tenths, which keeps the total energy the same.
    const uint32_t channelBytes = SamplesPerChannel * 4;
    {
        // How much of the frame is anything at all, for the report: a mixer
        // that writes nothing and one whose layout this misreads look the
        // same at the speakers and different here.
        const uint8_t* bytes = Guest::Ptr(guestAddress);
        uint32_t nonzero = 0;
        for (uint32_t i = 0; i < SamplesPerChannel * Channels * 4; i++) nonzero += bytes[i] != 0;
        if (nonzero > g_nonzeroBytes.load(std::memory_order_relaxed)) g_nonzeroBytes.store(nonzero, std::memory_order_relaxed);
    }
    for (uint32_t i = 0; i < SamplesPerChannel; i++)
    {
        const uint32_t offset = guestAddress + i * 4;
        const float l  = GuestFloat(offset + 0 * channelBytes);
        const float r  = GuestFloat(offset + 1 * channelBytes);
        const float c  = GuestFloat(offset + 2 * channelBytes);
        const float ls = GuestFloat(offset + 4 * channelBytes);
        const float rs = GuestFloat(offset + 5 * channelBytes);

        block.samples[i * 2 + 0] = ToPcm(l + 0.707f * c + 0.707f * ls);
        block.samples[i * 2 + 1] = ToPcm(r + 0.707f * c + 0.707f * rs);

        const int left = std::abs(int(block.samples[i * 2 + 0]));
        const int right = std::abs(int(block.samples[i * 2 + 1]));
        const int loudest = (left > right) ? left : right;
        if (loudest > g_peak.load(std::memory_order_relaxed))
            g_peak.store(loudest, std::memory_order_relaxed);
    }

    Dump(block.samples, sizeof(block.samples));
    block.header.dwFlags &= ~WHDR_DONE;
    block.header.dwBufferLength = sizeof(block.samples);
    if (waveOutWrite(g_device, &block.header, sizeof(WAVEHDR)) != MMSYSERR_NOERROR)
    {
        block.header.dwFlags |= WHDR_DONE;
        g_dropped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    g_played.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void Audio::Report()
{
    const uint64_t played = g_played.load();
    if (played == 0) return;

    printf("audio: at most %u of %u frame bytes nonzero; ", g_nonzeroBytes.load(), SamplesPerChannel * Channels * 4);
    printf("%llu frames played, %llu dropped, loudest sample %.1f%% of "
           "full scale\n",
        (unsigned long long)played, (unsigned long long)g_dropped.load(),
        g_peak.load() * 100.0 / 32767.0);
    fflush(stdout);
}
