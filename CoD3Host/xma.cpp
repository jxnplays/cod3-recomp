// XMA decoding through FFmpeg, loaded at run time. See xma.h.

#include "xma.h"

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include <Windows.h>

#ifdef COD3_HAVE_FFMPEG_HEADERS
extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/mem.h>
#include <libavutil/version.h>
}
#endif

namespace
{
#ifdef COD3_HAVE_FFMPEG_HEADERS
    // The handful of entry points used, resolved by name so the libraries
    // are optional.
    struct Ffmpeg
    {
        decltype(&avcodec_find_decoder) find_decoder = nullptr;
        decltype(&avcodec_alloc_context3) alloc_context = nullptr;
        decltype(&avcodec_open2) open = nullptr;
        decltype(&avcodec_send_packet) send_packet = nullptr;
        decltype(&avcodec_receive_frame) receive_frame = nullptr;
        decltype(&avcodec_free_context) free_context = nullptr;
        decltype(&avcodec_flush_buffers) flush = nullptr;
        decltype(&av_packet_alloc) packet_alloc = nullptr;
        decltype(&av_packet_free) packet_free = nullptr;
        decltype(&av_frame_alloc) frame_alloc = nullptr;
        decltype(&av_frame_free) frame_free = nullptr;
        decltype(&av_frame_unref) frame_unref = nullptr;
        decltype(&av_malloc) malloc = nullptr;
        decltype(&av_channel_layout_default) channel_layout_default = nullptr;
        decltype(&av_log_set_level) log_set_level = nullptr;
        bool loaded = false;
        std::string problem;
    };

    HMODULE LoadFrom(const wchar_t* name)
    {
        // Beside the executable first, then the usual install, then the path.
        wchar_t exe[MAX_PATH];
        if (GetModuleFileNameW(nullptr, exe, MAX_PATH) != 0)
        {
            std::wstring beside(exe);
            beside.resize(beside.find_last_of(L"\\/") + 1);
            beside += name;
            if (HMODULE module = LoadLibraryW(beside.c_str())) return module;
        }
        std::wstring usual = L"C:\\ffmpeg\\bin\\";
        usual += name;
        if (HMODULE module = LoadLibraryExW(usual.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH)) return module;
        return LoadLibraryW(name);
    }

    const Ffmpeg& Library()
    {
        static Ffmpeg ffmpeg;
        static std::once_flag once;
        std::call_once(once, []() {
            // The names carry the major version the headers were for; a
            // library of another major would not match the structures.
            const std::wstring codec = L"avcodec-" + std::to_wstring(LIBAVCODEC_VERSION_MAJOR) + L".dll";
            const std::wstring util = L"avutil-" + std::to_wstring(LIBAVUTIL_VERSION_MAJOR) + L".dll";
            HMODULE utilModule = LoadFrom(util.c_str());
            HMODULE codecModule = utilModule ? LoadFrom(codec.c_str()) : nullptr;
            if (utilModule == nullptr || codecModule == nullptr)
            {
                char narrow[64];
                snprintf(narrow, sizeof(narrow), "%ls and %ls were not found", codec.c_str(), util.c_str());
                ffmpeg.problem = narrow;
                return;
            }
            bool ok = true;
            auto get = [&](HMODULE module, const char* name, auto& slot) {
                slot = reinterpret_cast<std::remove_reference_t<decltype(slot)>>(GetProcAddress(module, name));
                if (slot == nullptr) { ok = false; ffmpeg.problem = std::string(name) + " is missing"; }
            };
            get(codecModule, "avcodec_find_decoder", ffmpeg.find_decoder);
            get(codecModule, "avcodec_alloc_context3", ffmpeg.alloc_context);
            get(codecModule, "avcodec_open2", ffmpeg.open);
            get(codecModule, "avcodec_send_packet", ffmpeg.send_packet);
            get(codecModule, "avcodec_receive_frame", ffmpeg.receive_frame);
            get(codecModule, "avcodec_free_context", ffmpeg.free_context);
            get(codecModule, "avcodec_flush_buffers", ffmpeg.flush);
            get(codecModule, "av_packet_alloc", ffmpeg.packet_alloc);
            get(codecModule, "av_packet_free", ffmpeg.packet_free);
            get(utilModule, "av_frame_alloc", ffmpeg.frame_alloc);
            get(utilModule, "av_frame_free", ffmpeg.frame_free);
            get(utilModule, "av_frame_unref", ffmpeg.frame_unref);
            get(utilModule, "av_malloc", ffmpeg.malloc);
            get(utilModule, "av_channel_layout_default", ffmpeg.channel_layout_default);
            get(utilModule, "av_log_set_level", ffmpeg.log_set_level);
            if (!ok) return;
            ffmpeg.log_set_level(getenv("COD3_XMALOG") != nullptr ? AV_LOG_DEBUG : AV_LOG_ERROR);
            ffmpeg.loaded = true;
        });
        return ffmpeg;
    }
#endif
}

namespace Xma
{
    struct Stream
    {
#ifdef COD3_HAVE_FFMPEG_HEADERS
        AVCodecContext* context = nullptr;
        AVPacket* packet = nullptr;
        AVFrame* frame = nullptr;
#endif
        int channels = 1;
    };

    bool Available()
    {
#ifdef COD3_HAVE_FFMPEG_HEADERS
        return Library().loaded;
#else
        return false;
#endif
    }

    const char* Unavailable()
    {
#ifdef COD3_HAVE_FFMPEG_HEADERS
        return Library().loaded ? "" : Library().problem.c_str();
#else
        return "this build was made without the FFmpeg headers";
#endif
    }

    Stream* Create(int channels, int sampleRate)
    {
#ifdef COD3_HAVE_FFMPEG_HEADERS
        const Ffmpeg& ff = Library();
        if (!ff.loaded) return nullptr;
        // This title is from 2006 and its sound is the first XMA; COD3_XMA=2
        // asks for the second's decoder instead, whose packet header differs.
        static const bool second = []() { const char* t = getenv("COD3_XMA"); return t != nullptr && t[0] == '2'; }();
        const AVCodec* codec = ff.find_decoder(second ? AV_CODEC_ID_XMA2 : AV_CODEC_ID_XMA1);
        if (codec == nullptr) return nullptr;
        Stream* stream = new Stream;
        stream->channels = channels < 2 ? 1 : 2;
        stream->context = ff.alloc_context(codec);
        stream->packet = ff.packet_alloc();
        stream->frame = ff.frame_alloc();
        if (stream->context == nullptr || stream->packet == nullptr || stream->frame == nullptr) { Destroy(stream); return nullptr; }
        AVCodecContext* c = stream->context;
        c->sample_rate = sampleRate;
        ff.channel_layout_default(&c->ch_layout, stream->channels);
        c->block_align = 2048;
        // What follows a WAVEFORMATEX in a file of the format: for the first
        // XMA the XMAWAVEFORMAT (tag, bits, options, skip, streams, loops,
        // version) and one XMASTREAMFORMAT (rates, loops, subframe data,
        // channels, mask); for the second the XMA2 fields, the stream count
        // first. The decoder reads the stream and channel counts from them.
        const int size = second ? 34 : 28;
        c->extradata = static_cast<uint8_t*>(ff.malloc(size + AV_INPUT_BUFFER_PADDING_SIZE));
        if (c->extradata == nullptr) { Destroy(stream); return nullptr; }
        memset(c->extradata, 0, size + AV_INPUT_BUFFER_PADDING_SIZE);
        uint8_t* e = c->extradata;
        if (second)
        {
            e[0] = 1;   // streams
        }
        else
        {
            e[0] = 0x65; e[1] = 0x01;   // WAVE_FORMAT_XMA
            e[2] = 16;                  // bits a sample
            e[4] = 1;                   // streams
            e[7] = 3;                   // version
            const uint32_t rate = uint32_t(sampleRate);
            e[8 + 4] = uint8_t(rate); e[8 + 5] = uint8_t(rate >> 8); e[8 + 6] = uint8_t(rate >> 16); e[8 + 7] = uint8_t(rate >> 24);
            e[8 + 17] = uint8_t(stream->channels);
            e[8 + 18] = stream->channels == 2 ? 3 : 4;   // the mask: front pair, or the centre
        }
        c->extradata_size = size;
        if (ff.open(c, codec, nullptr) < 0) { Destroy(stream); return nullptr; }
        return stream;
#else
        (void)channels; (void)sampleRate;
        return nullptr;
#endif
    }

    void Destroy(Stream* stream)
    {
        if (stream == nullptr) return;
#ifdef COD3_HAVE_FFMPEG_HEADERS
        const Ffmpeg& ff = Library();
        if (stream->frame != nullptr) ff.frame_free(&stream->frame);
        if (stream->packet != nullptr) ff.packet_free(&stream->packet);
        if (stream->context != nullptr) ff.free_context(&stream->context);
#endif
        delete stream;
    }

    bool Decode(Stream* stream, const uint8_t* packet, std::vector<int16_t>& out)
    {
#ifdef COD3_HAVE_FFMPEG_HEADERS
        if (stream == nullptr) return false;
        const Ffmpeg& ff = Library();
        // The packet is copied: the decoder keeps a reference to what it is
        // given until the next one, and the title's buffer is about to be
        // refilled.
        static thread_local std::vector<uint8_t> copy;
        copy.assign(packet, packet + 2048);
        // COD3_XMADUMP=path: the first two hundred packets any context is
        // fed, raw, one after another, for trying decoders on outside.
        {
            static const char* const dumpPath = getenv("COD3_XMADUMP");
            static int dumped = 0;
            if (dumpPath != nullptr && dumped < 200)
            {
                if (FILE* out = fopen(dumpPath, dumped == 0 ? "wb" : "ab")) { fwrite(packet, 1, 2048, out); fclose(out); }
                dumped++;
            }
        }
        copy.resize(2048 + AV_INPUT_BUFFER_PADDING_SIZE, 0);
        stream->packet->data = copy.data();
        stream->packet->size = 2048;
        const int sent = ff.send_packet(stream->context, stream->packet);
        if (sent < 0)
        {
            static int announced = 0;
            if (announced++ < 6)
            {
                printf("xma: the decoder refused a packet (%d); its first words %02X%02X%02X%02X %02X%02X%02X%02X\n", sent,
                    packet[0], packet[1], packet[2], packet[3], packet[4], packet[5], packet[6], packet[7]);
                fflush(stdout);
            }
            return false;
        }
        for (;;)
        {
            const int got = ff.receive_frame(stream->context, stream->frame);
            if (got < 0) break;
            AVFrame* frame = stream->frame;
            const int samples = frame->nb_samples;
            const int channels = stream->channels;
            const size_t at = out.size();
            out.resize(at + size_t(samples) * channels);
            if (frame->format == AV_SAMPLE_FMT_FLTP)
            {
                for (int ch = 0; ch < channels; ch++)
                {
                    const float* plane = reinterpret_cast<const float*>(frame->data[ch]);
                    for (int i = 0; i < samples; i++)
                    {
                        float v = plane[i] * 32767.0f;
                        v = v > 32767.0f ? 32767.0f : (v < -32768.0f ? -32768.0f : v);
                        out[at + size_t(i) * channels + ch] = int16_t(v);
                    }
                }
            }
            else if (frame->format == AV_SAMPLE_FMT_S16)
            {
                memcpy(&out[at], frame->data[0], size_t(samples) * channels * 2);
            }
            else if (frame->format == AV_SAMPLE_FMT_S16P)
            {
                for (int ch = 0; ch < channels; ch++)
                {
                    const int16_t* plane = reinterpret_cast<const int16_t*>(frame->data[ch]);
                    for (int i = 0; i < samples; i++) out[at + size_t(i) * channels + ch] = plane[i];
                }
            }
            else
            {
                // A format this does not convert: silence of the right length.
                memset(&out[at], 0, size_t(samples) * channels * 2);
            }
            ff.frame_unref(frame);
        }
        return true;
#else
        (void)stream; (void)packet; (void)out;
        return false;
#endif
    }

    void Reset(Stream* stream)
    {
#ifdef COD3_HAVE_FFMPEG_HEADERS
        if (stream != nullptr && stream->context != nullptr) Library().flush(stream->context);
#else
        (void)stream;
#endif
    }
}
