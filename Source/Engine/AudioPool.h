#pragma once

#include "../Common.h"

namespace lathe
{
/** One audio file, memory-mapped for allocation-free reads on the audio thread. */
struct AudioFileSource
{
    File file;
    std::unique_ptr<MemoryMappedAudioFormatReader> reader;
    int64 lengthInSamples = 0;
    int numChannels = 0;
    double sampleRate = 0.0;
};

/** Owns every audio file the project plays, plus their waveform thumbnails.
    Message thread only, except that the engine reads through the readers. */
class AudioPool
{
public:
    AudioPool();

    AudioFormatManager formats;

    /** Returns the mapped source for a WAV file, opening it on first use. */
    std::shared_ptr<AudioFileSource> get (const File&);

    /** Waveform overview for drawing. Loads in the background; listeners get a change message. */
    AudioThumbnail& getThumbnail (const File&);

    /** Copies an audio file into the project's Audio folder as a WAV at the project
        sample rate (converting format and rate if needed). Returns the new file. */
    File importFile (const File& source, const File& audioDir, double sampleRate, String& error);

    /** Drops sources no longer referenced by anything but the pool. */
    void purgeUnused();

private:
    AudioThumbnailCache thumbnailCache { 200 };
    std::map<String, std::shared_ptr<AudioFileSource>> sources;
    std::map<String, std::unique_ptr<AudioThumbnail>> thumbnails;
};
} // namespace lathe
