#include "ClipJoin.h"

namespace lathe
{
namespace
{
Clip joinMidi (const std::vector<const Clip*>& parts, double start, double end)
{
    Clip out;
    out.kind = ClipKind::midi;
    out.start = start;
    out.length = end - start;

    for (auto* c : parts)
    {
        const double shift = c->start - start;
        for (auto n : c->notes)
        {
            // Only what the clip actually plays: notes past its end are hidden, long ones are cut.
            if (n.start >= c->length || n.start + n.length <= 0.0)
                continue;
            n.length = jmin (n.length, c->length - n.start);
            n.start += shift;
            out.notes.push_back (n);
        }
        for (auto e : c->events)
        {
            if (e.beat < 0.0 || e.beat >= c->length)
                continue;
            e.beat += shift;
            out.events.push_back (e);
        }
    }

    std::stable_sort (out.notes.begin(), out.notes.end(), [] (const Note& a, const Note& b) { return a.start < b.start; });
    std::stable_sort (out.events.begin(), out.events.end(), [] (const MidiEvent& a, const MidiEvent& b) { return a.beat < b.beat; });
    return out;
}

Result joinAudio (Edit& edit, AudioPool& pool, const std::vector<const Clip*>& parts,
                  double start, double end, Clip& out)
{
    const double rate = edit.sampleRate;
    const double spb = edit.samplesPerBeat (rate);
    const auto total = (int64) std::llround ((end - start) * spb);
    if (total <= 0)
        return Result::fail ("The clips have no length.");
    if (total > (int64) std::numeric_limits<int>::max())
        return Result::fail ("The joined clip would be too long.");

    int channels = 1;
    for (auto* c : parts)
        if (auto src = pool.get (c->file))
            channels = jmax (channels, jmin (2, src->numChannels));

    AudioBuffer<float> mix (channels, (int) total);
    mix.clear();

    for (auto* c : parts)
    {
        std::unique_ptr<AudioFormatReader> reader (pool.formats.createReaderFor (c->file));
        if (reader == nullptr)
            return Result::fail ("Couldn't read " + c->file.getFileName() + ".");

        const auto from = (int64) std::llround ((c->start - start) * spb);
        auto n = (int64) std::llround (c->length * spb);
        n = jmin (n, (int64) reader->lengthInSamples - c->sourceOffset, total - from);
        if (n <= 0 || from < 0)
            continue;

        AudioBuffer<float> piece ((int) jmax ((int) reader->numChannels, 1), (int) n);
        reader->read (&piece, 0, (int) n, c->sourceOffset, true, true);

        // The same short fades the engine puts on clip edges, so it sounds identical.
        // Not on the joined clip's own outer edges: the engine still fades those.
        const auto clipLen = (int64) std::llround (c->length * spb);
        const int fade = (int) jmin ((int64) 64, clipLen / 2);
        const bool fadeIn = from > 0;
        const bool fadeOut = from + clipLen < total;
        const float gain = Decibels::decibelsToGain (c->gainDb);
        for (int ch = 0; ch < channels; ++ch)
        {
            const int srcCh = jmin (ch, piece.getNumChannels() - 1);
            const float* in = piece.getReadPointer (srcCh);
            float* dst = mix.getWritePointer (ch, (int) from);
            for (int i = 0; i < (int) n; ++i)
            {
                float g = gain;
                if (fade > 0)
                {
                    if (fadeIn && i < fade) g *= (float) i / (float) fade;
                    if (fadeOut && clipLen - i < fade) g *= (float) (clipLen - i) / (float) fade;
                }
                // Later clips are drawn on top, so they win where clips overlap.
                dst[i] = in[i] * g;
            }
        }
    }

    edit.ensureWorkDir();
    auto dir = edit.getAudioDir();
    dir.createDirectory();
    const auto file = dir.getNonexistentChildFile (File::createLegalFileName (parts.front()->name) + " joined", ".wav", false);

    std::unique_ptr<OutputStream> stream (file.createOutputStream());
    WavAudioFormat wav;
    // 32-bit float: clip gain can push peaks over 0 dB, and float keeps them.
    auto writer = stream != nullptr ? wav.createWriterFor (stream, AudioFormatWriterOptions()
                                                                      .withSampleRate (rate)
                                                                      .withNumChannels (channels)
                                                                      .withBitsPerSample (32)
                                                                      .withSampleFormat (AudioFormatWriterOptions::SampleFormat::floatingPoint))
                                    : nullptr;
    if (writer == nullptr || ! writer->writeFromAudioSampleBuffer (mix, 0, mix.getNumSamples()))
    {
        writer.reset();
        file.deleteFile();
        return Result::fail ("Couldn't write " + file.getFullPathName() + ".");
    }
    writer.reset();

    out.kind = ClipKind::audio;
    out.file = file;
    out.sourceOffset = 0;
    out.gainDb = 0.0f;
    out.start = start;
    out.length = end - start;
    return Result::ok();
}
} // namespace

Result joinClips (Edit& edit, AudioPool& pool, Track& track, const std::vector<int>& clipIds, int& newClipId)
{
    newClipId = -1;

    // In timeline order, then by their order on the track (later ones sit on top).
    std::vector<const Clip*> parts;
    for (auto& c : track.clips)
        if (std::find (clipIds.begin(), clipIds.end(), c.id) != clipIds.end())
            parts.push_back (&c);

    if (parts.size() < 2)
        return Result::fail ("Select two or more clips on the same track to join them.");

    const auto kind = parts.front()->kind;
    double start = parts.front()->start, end = parts.front()->end();
    for (auto* c : parts)
    {
        if (c->kind != kind)
            return Result::fail ("Audio and MIDI clips can't be joined together.");
        start = jmin (start, c->start);
        end = jmax (end, c->end());
    }

    Clip joined;
    if (kind == ClipKind::midi)
    {
        joined = joinMidi (parts, start, end);
    }
    else
    {
        auto r = joinAudio (edit, pool, parts, start, end, joined);
        if (r.failed())
            return r;
    }

    // Named after the first clip in time.
    const Clip* first = parts.front();
    for (auto* c : parts)
        if (c->start < first->start)
            first = c;
    joined.name = first->name;
    joined.id = edit.newId();

    std::vector<Clip> kept;
    for (auto& c : track.clips)
        if (std::find (clipIds.begin(), clipIds.end(), c.id) == clipIds.end())
            kept.push_back (c);
    track.clips = std::move (kept);

    // Anything else that was inside the joined range is now under the new clip.
    edit.clearRange (track, joined.start, joined.end());
    track.clips.push_back (joined);
    newClipId = joined.id;
    return Result::ok();
}
} // namespace lathe
