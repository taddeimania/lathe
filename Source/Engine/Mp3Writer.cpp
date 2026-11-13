#include "Mp3Writer.h"

#include <lame.h>

namespace lathe
{
namespace
{
class Mp3Writer final : public AudioFormatWriter
{
public:
    Mp3Writer (OutputStream* out, double rate, int channels, int kbps, const String& title)
        : AudioFormatWriter (out, "MP3 file", rate, (unsigned int) channels, 32)
    {
        usesFloatingPointData = true;

        lame = lame_init();
        if (lame == nullptr)
            return;

        lame_set_in_samplerate (lame, roundToInt (rate));
        // MP3 stops at 48 kHz. LAME would take 88.2 kHz to 48; halving it to 44.1 is cleaner.
        if (rate > 48000.0)
            lame_set_out_samplerate (lame, roundToInt (rate) % 44100 == 0 ? 44100 : 48000);
        lame_set_num_channels (lame, channels);
        lame_set_mode (lame, channels == 1 ? MONO : JOINT_STEREO);
        lame_set_VBR (lame, vbr_off);
        lame_set_brate (lame, kbps);
        lame_set_quality (lame, 2);              // LAME's "high quality": slower, worth it for a final mix
        lame_set_bWriteVbrTag (lame, 1);         // the Info frame (length, gapless playback)
        lame_set_write_id3tag_automatic (lame, 0);

        id3tag_init (lame);
        id3tag_add_v2 (lame);
        id3tag_v2_only (lame);
        setText ("TIT2", title);
        setText ("TSSE", "Lathe");

        if (lame_init_params (lame) < 0)
        {
            lame_close (lame);
            lame = nullptr;
            return;
        }

        // Written by hand (not left to LAME) so we know where the Info frame starts.
        const auto tagSize = lame_get_id3v2_tag (lame, nullptr, 0);
        if (tagSize > 0)
        {
            HeapBlock<unsigned char> tag (tagSize);
            const auto n = lame_get_id3v2_tag (lame, tag, tagSize);
            output->write (tag, n);
        }
        infoFramePosition = output->getPosition();
        ok = true;
    }

    ~Mp3Writer() override
    {
        if (lame != nullptr)
        {
            if (ok)
            {
                unsigned char end[7200];
                const auto n = lame_encode_flush (lame, end, (int) sizeof (end));
                if (n > 0)
                    output->write (end, (size_t) n);

                // The Info frame was written blank at the start; now it can say how long the file is.
                unsigned char info[2880];
                const auto size = lame_get_lametag_frame (lame, info, sizeof (info));
                const auto endPosition = output->getPosition();
                if (size > 0 && size <= sizeof (info) && output->setPosition (infoFramePosition))
                {
                    output->write (info, size);
                    output->setPosition (endPosition);
                }
                output->flush();
            }
            lame_close (lame);
        }
    }

    bool write (const int** samples, int numSamples) override
    {
        if (! ok)
            return false;

        // usesFloatingPointData: these are floats.
        auto left  = reinterpret_cast<const float*> (samples[0]);
        auto right = (numChannels > 1 && samples[1] != nullptr) ? reinterpret_cast<const float*> (samples[1]) : left;

        constexpr int chunk = 4096;
        for (int done = 0; done < numSamples;)
        {
            const auto n = jmin (chunk, numSamples - done);
            const auto bytes = lame_encode_buffer_ieee_float (lame, left + done, right + done, n,
                                                              encoded.data(), (int) encoded.size());
            if (bytes < 0 || (bytes > 0 && ! output->write (encoded.data(), (size_t) bytes)))
                return false;
            done += n;
        }
        return true;
    }

    bool ok = false;

private:
    void setText (const char* id, const String& text)
    {
        if (text.isEmpty())
            return;
        // UTF-16 with a byte order mark, so any song name survives.
        std::vector<unsigned short> utf16 { 0xfeff };
        const auto chars = text.toUTF16();
        for (auto* c = chars.getAddress(); *c != 0; ++c)
            utf16.push_back ((unsigned short) *c);
        utf16.push_back (0);
        id3tag_set_textinfo_utf16 (lame, id, utf16.data());
    }

    lame_global_flags* lame = nullptr;
    int64 infoFramePosition = 0;
    // LAME's worst case for 4096 samples: 1.25 x samples + 7200 bytes.
    std::array<unsigned char, 4096 * 5 / 4 + 7200> encoded {};
};
}

std::unique_ptr<AudioFormatWriter> makeMp3Writer (std::unique_ptr<OutputStream> stream, double sampleRate,
                                                  int numChannels, int kbps, const String& title)
{
    if (stream == nullptr || numChannels < 1 || numChannels > 2)
        return {};
    auto w = std::make_unique<Mp3Writer> (stream.release(), sampleRate, numChannels, kbps, title);
    if (! w->ok)
        return {};
    return w;
}
}
