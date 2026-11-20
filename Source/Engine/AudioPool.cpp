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

Result copyFileWithProgress (const File& from, const File& to, const std::function<bool (double)>& progress)
{
    FileInputStream in (from);
    std::unique_ptr<FileOutputStream> out (to.createOutputStream());
    if (! in.openedOk() || out == nullptr || ! out->openedOk() || ! out->truncate().wasOk())
    {
        out.reset();
        to.deleteFile();
        return Result::fail ("Couldn't copy " + from.getFileName());
    }

    const auto total = jmax ((int64) 1, in.getTotalLength());
    HeapBlock<char> buffer (1 << 20);
    bool ok = true;
    while (! in.isExhausted())
    {
        const auto n = in.read (buffer, 1 << 20);
        if (n <= 0)
            break;
        if (! out->write (buffer, (size_t) n))
        {
            ok = false;
            break;
        }
        if (progress && ! progress (jlimit (0.0, 1.0, (double) in.getPosition() / (double) total)))
        {
            out.reset();
            to.deleteFile();
            return Result::fail ("Cancelled.");
        }
    }
    out->flush();
    ok = ok && out->getStatus().wasOk() && in.getPosition() == in.getTotalLength();
    out.reset();
    if (! ok)
    {
        to.deleteFile();
        return Result::fail ("Couldn't copy " + from.getFileName() + " (is the disk full?)");
    }
    return Result::ok();
}

File AudioPool::importFile (const File& source, const File& audioDir, double sampleRate, String& error,
                            const std::function<bool (double)>& progress)
{
    auto report = [&] (double p) { return progress == nullptr || progress (jlimit (0.0, 1.0, p)); };
    auto cancelled = [&] (const File& partial)
    {
        partial.deleteFile();
        error = "Import cancelled.";
        return File();
    };

    std::unique_ptr<AudioFormatReader> reader (formats.createReaderFor (source));
    if (reader == nullptr)
    {
        error = "Couldn't read " + source.getFileName() + " (unsupported format?)";
        return {};
    }

    audioDir.createDirectory();
    auto dest = audioDir.getNonexistentChildFile (File::createLegalFileName (source.getFileNameWithoutExtension()), ".wav", false);
    constexpr int block = 1 << 16;

    // Already a WAV at the right rate: a plain copy, a block at a time.
    if (source.hasFileExtension ("wav") && std::abs (reader->sampleRate - sampleRate) < 0.5 && ! reader->usesFloatingPointData)
    {
        reader.reset();
        const auto copied = copyFileWithProgress (source, dest, report);
        if (copied.wasOk())
            return dest;
        if (copied.getErrorMessage() == "Cancelled.")
            return cancelled (dest);
        reader.reset (formats.createReaderFor (source));   // fall back to converting it
        if (reader == nullptr)
        {
            error = "Couldn't copy " + source.getFileName();
            return {};
        }
    }

    // Converting: reading is about 60% of the work, the rate change 25%, writing the rest.
    const bool changeRate = std::abs (reader->sampleRate - sampleRate) >= 0.5;
    const double readShare = changeRate ? 0.6 : 0.8, rateShare = changeRate ? 0.25 : 0.0;
    const int numChannels = (int) jlimit ((unsigned int) 1, (unsigned int) 2, reader->numChannels);
    const auto length = (int) jmin ((int64) std::numeric_limits<int>::max() / 2, reader->lengthInSamples);

    AudioBuffer<float> in (numChannels, jmax (1, length));
    for (int pos = 0; pos < length; pos += block)
    {
        const int n = jmin (block, length - pos);
        reader->read (&in, pos, n, pos, true, numChannels > 1);
        if (! report (readShare * (pos + n) / (double) length))
            return cancelled (dest);
    }

    AudioBuffer<float>* out = &in;
    AudioBuffer<float> resampled;

    if (changeRate)
    {
        const double ratio = reader->sampleRate / sampleRate;   // input samples per output sample
        const int outLength = (int) std::ceil (length / ratio);
        resampled.setSize (numChannels, jmax (1, outLength));

        for (int ch = 0; ch < numChannels; ++ch)
        {
            WindowedSincInterpolator interp;
            int used = 0;
            for (int pos = 0; pos < outLength; pos += block)
            {
                const int n = jmin (block, outLength - pos);
                used += interp.process (ratio, in.getReadPointer (ch) + used, resampled.getWritePointer (ch) + pos, n,
                                        jmax (0, length - used), 0);
                if (! report (readShare + rateShare * (ch + (pos + n) / (double) outLength) / numChannels))
                    return cancelled (dest);
            }
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

    const int total = length > 0 ? out->getNumSamples() : 0;
    for (int pos = 0; pos < total; pos += block)
    {
        const int n = jmin (block, total - pos);
        if (! writer->writeFromAudioSampleBuffer (*out, pos, n))
        {
            writer.reset();
            dest.deleteFile();
            error = "Couldn't write " + dest.getFileName() + " (is the disk full?)";
            return {};
        }
        if (! report (readShare + rateShare + (1.0 - readShare - rateShare) * (pos + n) / (double) total))
        {
            writer.reset();
            return cancelled (dest);
        }
    }
    writer.reset();
    return dest;
}
} // namespace lathe
