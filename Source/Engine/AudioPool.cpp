#include "AudioPool.h"

namespace lathe
{
AudioPool::AudioPool()
{
    formats.registerBasicFormats();
}

std::shared_ptr<AudioFileSource> AudioPool::get (const File& file)
{
    const auto key = file.getFullPathName();

    if (auto it = sources.find (key); it != sources.end())
        return it->second;

    if (! file.existsAsFile())
        return {};

    WavAudioFormat wav;
    std::unique_ptr<MemoryMappedAudioFormatReader> reader (wav.createMemoryMappedReader (file));

    if (reader == nullptr || ! reader->mapEntireFile() || reader->getMappedSection().isEmpty())
        return {};

    auto src = std::make_shared<AudioFileSource>();
    src->file = file;
    src->lengthInSamples = reader->lengthInSamples;
    src->numChannels = (int) reader->numChannels;
    src->sampleRate = reader->sampleRate;
    src->reader = std::move (reader);

    sources[key] = src;
    return src;
}

AudioThumbnail& AudioPool::getThumbnail (const File& file)
{
    const auto key = file.getFullPathName();

    if (auto it = thumbnails.find (key); it != thumbnails.end())
        return *it->second;

    auto thumb = std::make_unique<AudioThumbnail> (256, formats, thumbnailCache);
    thumb->setSource (new FileInputSource (file));
    auto& ref = *thumb;
    thumbnails[key] = std::move (thumb);
    return ref;
}

void AudioPool::purgeUnused()
{
    for (auto it = sources.begin(); it != sources.end();)
    {
        if (it->second.use_count() == 1)
            it = sources.erase (it);
        else
            ++it;
    }
}

File AudioPool::importFile (const File& source, const File& audioDir, double sampleRate, String& error)
{
    std::unique_ptr<AudioFormatReader> reader (formats.createReaderFor (source));
    if (reader == nullptr)
    {
        error = "Couldn't read " + source.getFileName() + " (unsupported format?)";
        return {};
    }

    audioDir.createDirectory();
    auto dest = audioDir.getNonexistentChildFile (File::createLegalFileName (source.getFileNameWithoutExtension()), ".wav", false);

    // Already a WAV at the right rate: plain copy.
    if (source.hasFileExtension ("wav") && std::abs (reader->sampleRate - sampleRate) < 0.5 && ! reader->usesFloatingPointData)
    {
        reader.reset();
        if (source.copyFileTo (dest))
            return dest;
    }

    const int numChannels = (int) jlimit ((unsigned int) 1, (unsigned int) 2, reader->numChannels);
    const auto length = (int) jmin ((int64) std::numeric_limits<int>::max() / 2, reader->lengthInSamples);

    AudioBuffer<float> in (numChannels, length);
    reader->read (&in, 0, length, 0, true, numChannels > 1);

    AudioBuffer<float>* out = &in;
    AudioBuffer<float> resampled;

    if (std::abs (reader->sampleRate - sampleRate) >= 0.5)
    {
        const double ratio = reader->sampleRate / sampleRate;   // input samples per output sample
        const int outLength = (int) std::ceil (length / ratio);
        resampled.setSize (numChannels, outLength);

        for (int ch = 0; ch < numChannels; ++ch)
        {
            WindowedSincInterpolator interp;
            interp.process (ratio, in.getReadPointer (ch), resampled.getWritePointer (ch), outLength, length, 0);
        }
        out = &resampled;
    }

    std::unique_ptr<OutputStream> stream (dest.createOutputStream());
    if (stream == nullptr)
    {
        error = "Couldn't write to " + dest.getFullPathName();
        return {};
    }

    WavAudioFormat wav;
    auto writer = wav.createWriterFor (stream, AudioFormatWriterOptions()
                                                  .withSampleRate (sampleRate)
                                                  .withNumChannels (numChannels)
                                                  .withBitsPerSample (24));
    if (writer == nullptr)
    {
        error = "Couldn't create " + dest.getFileName();
        return {};
    }

    writer->writeFromAudioSampleBuffer (*out, 0, out->getNumSamples());
    writer.reset();
    return dest;
}
} // namespace lathe
