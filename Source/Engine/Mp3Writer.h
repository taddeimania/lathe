#pragma once

#include "../Common.h"

namespace lathe
{
/** An MP3 writer (JUCE can read MP3s but not write them). Constant bit rate, encoded by LAME.

    The file starts with an ID3v2 tag holding `title`, then LAME's Info frame, which players
    use for the exact length and to play it back gaplessly. That frame is filled in when the
    writer is deleted, so the stream must be able to seek (a FileOutputStream can).

    LAME takes any sample rate. Above 48 kHz, the most MP3 allows, it's resampled:
    88.2 and 176.4 kHz to 44.1, others to 48.
    Returns nullptr if LAME won't take the settings. */
std::unique_ptr<AudioFormatWriter> makeMp3Writer (std::unique_ptr<OutputStream> stream, double sampleRate,
                                                  int numChannels, int kbps, const String& title);

/** The bit rates the export offers, highest first. */
inline const std::array<int, 4> mp3BitRates { 320, 256, 192, 128 };
}
