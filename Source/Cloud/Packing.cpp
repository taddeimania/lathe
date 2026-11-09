#include "Packing.h"

namespace lathe::cloud
{
namespace
{
constexpr char kMagic[8] = { 'L', 'T', 'H', 'F', 'L', 'A', 'C', '1' };
constexpr int64 kMaxSideBytes = 16 * 1024 * 1024;   // header or trailer bigger than this: don't pack
constexpr int kBlockFrames = 1 << 16;

struct WavLayout
{
    int64 dataStart = 0, dataSize = 0;
    int channels = 0, bits = 0;
    int sampleRate = 0;
};

uint32 readLE32 (const uint8* p)   { return (uint32) p[0] | ((uint32) p[1] << 8) | ((uint32) p[2] << 16) | ((uint32) p[3] << 24); }
uint16 readLE16 (const uint8* p)   { return (uint16) (p[0] | (p[1] << 8)); }

/** Finds the sample data of a plain integer PCM WAV (16 or 24 bit, 1 to 8 channels). */
bool parseWav (FileInputStream& in, int64 fileSize, WavLayout& w)
{
    uint8 riff[12];
    if (in.read (riff, 12) != 12 || std::memcmp (riff, "RIFF", 4) != 0 || std::memcmp (riff + 8, "WAVE", 4) != 0)
        return false;

    bool haveFormat = false;
    int64 pos = 12;
    while (pos + 8 <= fileSize && pos <= kMaxSideBytes)
    {
        uint8 head[8];
        if (! in.setPosition (pos) || in.read (head, 8) != 8)
            return false;
        const int64 size = readLE32 (head + 4);
        const int64 body = pos + 8;

        if (std::memcmp (head, "fmt ", 4) == 0)
        {
            uint8 f[40] {};
            if (size < 16 || in.read (f, (int) jmin<int64> (size, 40)) < 16)
                return false;
            int tag = readLE16 (f);
            if (tag == 0xfffe && size >= 26)
                tag = readLE16 (f + 24);   // WAVE_FORMAT_EXTENSIBLE: the sub-format's first two bytes
            w.channels = readLE16 (f + 2);
            w.sampleRate = (int) readLE32 (f + 4);
            const int blockAlign = readLE16 (f + 12);
            w.bits = readLE16 (f + 14);
            if (tag != 1 || (w.bits != 16 && w.bits != 24) || w.channels < 1 || w.channels > 8
                || blockAlign != w.channels * w.bits / 8 || w.sampleRate <= 0 || w.sampleRate > 655350)
                return false;
            haveFormat = true;
        }
        else if (std::memcmp (head, "data", 4) == 0)
        {
            if (! haveFormat || size == 0xffffffff || body + size > fileSize)
                return false;
            w.dataStart = body;
            w.dataSize = size;
            return size % (w.channels * w.bits / 8) == 0 && fileSize - (body + size) <= kMaxSideBytes;
        }
        pos = body + size + (size & 1);
    }
    return false;
}

void writeLE32 (OutputStream& out, uint32 v)   { out.writeInt ((int) v); }   // JUCE writes little-endian

bool copyBytes (InputStream& in, OutputStream& out, int64 count)
{
    return out.writeFromInputStream (in, count) == count;
}
} // namespace

bool isPacked (const File& f)
{
    FileInputStream in (f);
    char m[8];
    return in.openedOk() && in.read (m, 8) == 8 && std::memcmp (m, kMagic, 8) == 0;
}

bool packAudio (const File& wav, const File& packed, const std::function<bool (double)>& progress)
{
    FileInputStream in (wav);
    if (in.failedToOpen())
        return false;
    const int64 fileSize = in.getTotalLength();
    WavLayout w;
    if (! parseWav (in, fileSize, w))
        return false;

    packed.deleteFile();
    packed.getParentDirectory().createDirectory();
    auto out = std::make_unique<FileOutputStream> (packed);
    if (out->failedToOpen())
        return false;

    // Our header: the WAV's own bytes around the samples, so unpacking is exact.
    const int64 trailerStart = w.dataStart + w.dataSize;
    out->write (kMagic, 8);
    writeLE32 (*out, (uint32) w.dataStart);
    in.setPosition (0);
    if (! copyBytes (in, *out, w.dataStart))
        return false;
    writeLE32 (*out, (uint32) (fileSize - trailerStart));
    in.setPosition (trailerStart);
    if (! copyBytes (in, *out, fileSize - trailerStart))
        return false;
    out->writeInt64 (w.dataSize);
    writeLE32 (*out, (uint32) w.channels);
    writeLE32 (*out, (uint32) w.bits);
    writeLE32 (*out, (uint32) w.sampleRate);

    bool ok = true;
    {
        std::unique_ptr<OutputStream> stream (std::move (out));
        FlacAudioFormat flac;
        auto writer = flac.createWriterFor (stream, AudioFormatWriterOptions()
                                                        .withSampleRate (w.sampleRate)
                                                        .withNumChannels (w.channels)
                                                        .withBitsPerSample (w.bits));
        if (writer == nullptr)
            return false;

        const int bytesPerSample = w.bits / 8;
        const int frameBytes = bytesPerSample * w.channels;
        const int64 totalFrames = w.dataSize / frameBytes;
        HeapBlock<uint8> raw ((size_t) kBlockFrames * (size_t) frameBytes);
        HeapBlock<int> samples ((size_t) kBlockFrames * (size_t) w.channels);
        std::vector<const int*> chans ((size_t) w.channels + 1, nullptr);
        for (int c = 0; c < w.channels; ++c)
            chans[(size_t) c] = samples.get() + (size_t) c * kBlockFrames;

        in.setPosition (w.dataStart);
        for (int64 done = 0; done < totalFrames && ok;)
        {
            const int n = (int) jmin<int64> (kBlockFrames, totalFrames - done);
            if (in.read (raw.get(), n * frameBytes) != n * frameBytes)
            {
                ok = false;
                break;
            }
            // Interleaved little-endian samples -> one left-justified 32-bit int per sample.
            const uint8* p = raw.get();
            for (int i = 0; i < n; ++i)
                for (int c = 0; c < w.channels; ++c, p += bytesPerSample)
                {
                    const uint32 v = bytesPerSample == 2 ? ((uint32) p[0] << 16) | ((uint32) p[1] << 24)
                                                         : ((uint32) p[0] << 8) | ((uint32) p[1] << 16) | ((uint32) p[2] << 24);
                    samples[(size_t) c * kBlockFrames + (size_t) i] = (int) v;
                }
            ok = writer->write (chans.data(), n);
            done += n;
            if (progress != nullptr && ! progress ((double) done / (double) totalFrames))
                ok = false;
        }
    }   // the writer finishes the FLAC stream and closes the file

    if (! ok || packed.getSize() > fileSize - fileSize / 20)
    {
        packed.deleteFile();   // failed, cancelled, or not worth it (noise, already-compressed audio)
        return false;
    }
    return true;
}

Result unpackAudio (const File& packed, const File& wav, const std::function<bool (double)>& progress)
{
    auto in = std::make_unique<FileInputStream> (packed);
    if (in->failedToOpen())
        return Result::fail ("Couldn't read " + packed.getFullPathName());
    const auto bad = [&] { return Result::fail (packed.getFileName() + " isn't a packed take."); };

    char magic[8];
    if (in->read (magic, 8) != 8 || std::memcmp (magic, kMagic, 8) != 0)
        return bad();
    const int64 headerLen = (uint32) in->readInt();
    if (headerLen <= 0 || headerLen > kMaxSideBytes)
        return bad();
    MemoryBlock header;
    if (in->readIntoMemoryBlock (header, (ssize_t) headerLen) != (size_t) headerLen)
        return bad();
    const int64 trailerLen = (uint32) in->readInt();
    if (trailerLen < 0 || trailerLen > kMaxSideBytes)
        return bad();
    MemoryBlock trailer;
    if (trailerLen > 0 && in->readIntoMemoryBlock (trailer, (ssize_t) trailerLen) != (size_t) trailerLen)
        return bad();
    const int64 dataSize = in->readInt64();
    const int channels = in->readInt(), bits = in->readInt();
    in->readInt();   // sample rate (the FLAC stream has it too)
    if ((bits != 16 && bits != 24) || channels < 1 || channels > 8 || dataSize < 0)
        return bad();

    const int bytesPerSample = bits / 8;
    const int frameBytes = bytesPerSample * channels;
    const int64 totalFrames = dataSize / frameBytes;
    const int64 flacStart = in->getPosition();
    const int64 flacLength = in->getTotalLength() - flacStart;

    FlacAudioFormat flac;
    std::unique_ptr<AudioFormatReader> reader (flac.createReaderFor (new SubregionStream (in.release(), flacStart, flacLength, true), true));
    if (reader == nullptr || reader->numChannels != (unsigned int) channels || reader->lengthInSamples != totalFrames)
        return Result::fail ("The packed audio in " + packed.getFileName() + " is damaged.");

    wav.deleteFile();
    FileOutputStream out (wav);
    if (out.failedToOpen())
        return Result::fail ("Couldn't write " + wav.getFullPathName());
    out.write (header.getData(), header.getSize());

    HeapBlock<int> samples ((size_t) kBlockFrames * (size_t) channels);
    std::vector<int*> chans ((size_t) channels);
    for (int c = 0; c < channels; ++c)
        chans[(size_t) c] = samples.get() + (size_t) c * kBlockFrames;
    HeapBlock<uint8> raw ((size_t) kBlockFrames * (size_t) frameBytes);

    for (int64 done = 0; done < totalFrames;)
    {
        const int n = (int) jmin<int64> (kBlockFrames, totalFrames - done);
        if (! reader->read (chans.data(), channels, done, n, false))
            return Result::fail ("The packed audio in " + packed.getFileName() + " is damaged.");
        uint8* p = raw.get();
        for (int i = 0; i < n; ++i)
            for (int c = 0; c < channels; ++c)
            {
                const auto v = (uint32) chans[(size_t) c][i];
                if (bytesPerSample == 2)
                {
                    *p++ = (uint8) (v >> 16);
                    *p++ = (uint8) (v >> 24);
                }
                else
                {
                    *p++ = (uint8) (v >> 8);
                    *p++ = (uint8) (v >> 16);
                    *p++ = (uint8) (v >> 24);
                }
            }
        if (! out.write (raw.get(), (size_t) (n * frameBytes)))
            return Result::fail ("Couldn't write " + wav.getFullPathName());
        done += n;
        if (progress != nullptr && ! progress ((double) done / (double) totalFrames))
            return Result::fail ("Cancelled.");
    }

    if (trailer.getSize() > 0)
        out.write (trailer.getData(), trailer.getSize());
    out.flush();
    return out.getStatus();
}
} // namespace lathe::cloud
