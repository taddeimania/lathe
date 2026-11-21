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

Result writeWav (const File& file, const AudioBuffer<float>& mix, double rate, const std::function<bool (double)>& progress)
{
    std::unique_ptr<OutputStream> stream (file.createOutputStream());
    WavAudioFormat wav;
    // 32-bit float: clip gain can push peaks over 0 dB, and float keeps them.
    auto writer = stream != nullptr ? wav.createWriterFor (stream, AudioFormatWriterOptions()
                                                                      .withSampleRate (rate)
                                                                      .withNumChannels (mix.getNumChannels())
                                                                      .withBitsPerSample (32)
                                                                      .withSampleFormat (AudioFormatWriterOptions::SampleFormat::floatingPoint))
                                    : nullptr;
    if (writer == nullptr)
        return Result::fail ("Couldn't write " + file.getFullPathName() + ".");

    constexpr int block = 1 << 16;
    for (int pos = 0; pos < mix.getNumSamples(); pos += block)
    {
        if (! writer->writeFromAudioSampleBuffer (mix, pos, jmin (block, mix.getNumSamples() - pos)))
            return Result::fail ("Couldn't write " + file.getFullPathName() + ".");
        if (progress && ! progress ((double) (pos + block) / (double) mix.getNumSamples()))
            return Result::fail ("Cancelled.");
    }
    return Result::ok();
}
} // namespace

JoinPlan planJoin (Edit& edit, const Track& track, const std::vector<int>& clipIds)
{
    JoinPlan plan;
    plan.trackId = track.id;
    plan.clipIds = clipIds;

    // In timeline order, then by their order on the track (later ones sit on top).
    for (auto& c : track.clips)
        if (std::find (clipIds.begin(), clipIds.end(), c.id) != clipIds.end())
            plan.parts.push_back (c);

    if (plan.parts.size() < 2)
    {
        plan.problem = "Select two or more clips on the same track to join them.";
        return plan;
    }

    plan.kind = plan.parts.front().kind;
    plan.start = plan.parts.front().start;
    plan.end = plan.parts.front().end();
    for (auto& c : plan.parts)
    {
        if (c.kind != plan.kind)
        {
            plan.problem = "Audio and MIDI clips can't be joined together.";
            return plan;
        }
        plan.start = jmin (plan.start, c.start);
        plan.end = jmax (plan.end, c.end());
    }

    plan.sampleRate = edit.sampleRate;
    plan.samplesPerBeat = edit.samplesPerBeat (edit.sampleRate);
    if (plan.isAudio())
    {
        edit.ensureWorkDir();
        plan.audioDir = edit.getAudioDir();
    }
    return plan;
}

Result renderJoin (const JoinPlan& plan, AudioFormatManager& formats, File& wav, const std::function<bool (double)>& progress)
{
    wav = File();
    if (plan.problem.isNotEmpty())
        return Result::fail (plan.problem);
    if (! plan.isAudio())
        return Result::ok();

    const double spb = plan.samplesPerBeat;
    const auto total = (int64) std::llround ((plan.end - plan.start) * spb);
    if (total <= 0)
        return Result::fail ("The clips have no length.");
    if (total > (int64) std::numeric_limits<int>::max())
        return Result::fail ("The joined clip would be too long.");

    std::vector<std::unique_ptr<AudioFormatReader>> readers;
    int channels = 1;
    for (auto& c : plan.parts)
    {
        readers.emplace_back (formats.createReaderFor (c.file));
        if (readers.back() == nullptr)
            return Result::fail ("Couldn't read " + c.file.getFileName() + ".");
        channels = jmax (channels, jmin (2, (int) readers.back()->numChannels));
    }

    AudioBuffer<float> mix (channels, (int) total);
    mix.clear();

    // Reading is about two thirds of the time, writing the rest.
    int64 toRead = 0;
    for (auto& c : plan.parts)
        toRead += jmax ((int64) 1, (int64) std::llround (c.length * spb));
    int64 read = 0;
    auto reading = [&] (int64 done) { return ! progress || progress (0.65 * (double) (read + done) / (double) toRead); };

    for (size_t k = 0; k < plan.parts.size(); ++k)
    {
        auto& c = plan.parts[k];
        auto& reader = *readers[k];
        const auto from = (int64) std::llround ((c.start - plan.start) * spb);
        const auto clipLen = (int64) std::llround (c.length * spb);
        auto n = jmin (clipLen, (int64) reader.lengthInSamples - c.sourceOffset, total - from);
        if (n > 0 && from >= 0)
        {
            // The same short fades the engine puts on clip edges, so it sounds identical.
            // Not on the joined clip's own outer edges: the engine still fades those.
            const int fade = (int) jmin ((int64) 64, clipLen / 2);
            const bool fadeIn = from > 0;
            const bool fadeOut = from + clipLen < total;
            const float gain = Decibels::decibelsToGain (c.gainDb);

            constexpr int block = 1 << 17;
            AudioBuffer<float> piece (jmax ((int) reader.numChannels, 1), block);
            for (int64 at = 0; at < n; at += block)
            {
                const int len = (int) jmin ((int64) block, n - at);
                reader.read (&piece, 0, len, c.sourceOffset + at, true, true);
                for (int ch = 0; ch < channels; ++ch)
                {
                    const int srcCh = jmin (ch, piece.getNumChannels() - 1);
                    const float* in = piece.getReadPointer (srcCh);
                    float* dst = mix.getWritePointer (ch, (int) (from + at));
                    for (int j = 0; j < len; ++j)
                    {
                        const int64 i = at + j;
                        float g = gain;
                        if (fade > 0)
                        {
                            if (fadeIn && i < fade) g *= (float) i / (float) fade;
                            if (fadeOut && clipLen - i < fade) g *= (float) (clipLen - i) / (float) fade;
                        }
                        // Later clips are drawn on top, so they win where clips overlap.
                        dst[j] = in[j] * g;
                    }
                }
                if (! reading (at + len))
                    return Result::fail ("Cancelled.");
            }
        }
        read += jmax ((int64) 1, clipLen);
    }

    plan.audioDir.createDirectory();
    const auto file = plan.audioDir.getNonexistentChildFile (File::createLegalFileName (plan.parts.front().name) + " joined", ".wav", false);
    const auto r = writeWav (file, mix, plan.sampleRate, [&] (double p) { return ! progress || progress (0.65 + 0.35 * jmin (1.0, p)); });
    if (r.failed())
    {
        file.deleteFile();
        return r;
    }
    wav = file;
    return Result::ok();
}

Result applyJoin (Edit& edit, Track& track, const JoinPlan& plan, const File& wav, int& newClipId)
{
    newClipId = -1;
    for (int id : plan.clipIds)
        if (track.findClip (id) == nullptr)
        {
            wav.deleteFile();
            return Result::fail ("The clips changed while they were being joined.");
        }

    Clip joined;
    if (plan.isAudio())
    {
        joined.kind = ClipKind::audio;
        joined.file = wav;
        joined.sourceOffset = 0;
        joined.gainDb = 0.0f;
        joined.start = plan.start;
        joined.length = plan.end - plan.start;
    }
    else
    {
        std::vector<const Clip*> parts;
        for (auto& c : plan.parts)
            parts.push_back (&c);
        joined = joinMidi (parts, plan.start, plan.end);
    }

    // Named after the first clip in time.
    const Clip* first = &plan.parts.front();
    for (auto& c : plan.parts)
        if (c.start < first->start)
            first = &c;
    joined.name = first->name;
    joined.id = edit.newId();

    std::vector<Clip> kept;
    for (auto& c : track.clips)
        if (std::find (plan.clipIds.begin(), plan.clipIds.end(), c.id) == plan.clipIds.end())
            kept.push_back (c);
    track.clips = std::move (kept);

    // Anything else that was inside the joined range is now under the new clip.
    edit.clearRange (track, joined.start, joined.end());
    track.clips.push_back (joined);
    newClipId = joined.id;
    return Result::ok();
}

Result joinClips (Edit& edit, AudioPool& pool, Track& track, const std::vector<int>& clipIds, int& newClipId)
{
    newClipId = -1;
    const auto plan = planJoin (edit, track, clipIds);
    if (plan.problem.isNotEmpty())
        return Result::fail (plan.problem);
    File wav;
    if (auto r = renderJoin (plan, pool.formats, wav); r.failed())
        return r;
    return applyJoin (edit, track, plan, wav, newClipId);
}
} // namespace lathe
