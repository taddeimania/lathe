#include "Bounce.h"

namespace lathe::bounce
{
Plan plan (const Edit& edit, int groupId)
{
    Plan p;
    p.groupId = groupId;
    auto group = edit.findGroup (groupId);
    if (group == nullptr)
    {
        p.problem = "That group isn't there anymore.";
        return p;
    }
    p.groupName = group->name;
    p.tempo = edit.tempo;

    // What reaches the master when the group is soloed: members that play straight to it, and
    // every return or bus they play into or send to (a drum bus, the reverb). Only the group's
    // tracks play in the render, so those returns carry nothing but the group.
    auto findBus = [&] (int id) -> std::shared_ptr<Track>
    {
        for (auto& b : edit.buses)
            if (b->id == id)
                return b;
        return {};
    };
    std::set<int> reached;   // returns and buses the group feeds
    if (auto bus = edit.findGroupBus (groupId))
        reached.insert (bus->id);

    bool anyClip = false;
    p.startBeat = std::numeric_limits<double>::max();
    for (auto& t : edit.groupMembers (groupId))
    {
        p.playing.insert (t->id);
        if (t->output.kind == OutputRoute::Kind::master)
            p.taps.insert (t->id);
        else if (t->output.kind == OutputRoute::Kind::bus)
        {
            if (findBus (t->output.busId) != nullptr)
                reached.insert (t->output.busId);
            else
                p.taps.insert (t->id);   // a return that's gone: the engine plays it to the master
        }
        for (auto& s : t->sends)
            if (s->levelDb.load() > kSilenceDb && findBus (s->busId) != nullptr)
            {
                reached.insert (s->busId);
                p.anySends = true;
            }
        for (auto& c : t->clips)
        {
            anyClip = true;
            p.startBeat = jmin (p.startBeat, c.start);
            p.endBeat = jmax (p.endBeat, c.end());
        }
    }

    // A return plays to the master unless it goes to an interface output (a return can't feed
    // another return; the engine sends one that tries to the master).
    for (auto id : reached)
        if (auto b = findBus (id); b != nullptr && b->output.kind != OutputRoute::Kind::hardware)
        {
            p.playing.insert (id);   // listed, so the render hands out its signal
            p.taps.insert (id);
            p.viaReturns = true;
        }

    if (! anyClip)
        p.problem = "\"" + p.groupName + "\" has no clips to bounce.";
    else if (p.taps.empty())
        p.problem = "Nothing in \"" + p.groupName + "\" reaches the master (its tracks and returns play to interface outputs), so there's nothing to bounce.";
    if (! anyClip)
        p.startBeat = 0.0;
    return p;
}

Bounced render (Engine& engine, const Plan& p, const File& folder, double tailSeconds, const std::function<bool (double)>& progress)
{
    Bounced r;
    r.startBeat = p.startBeat;
    if (p.problem.isNotEmpty())
    {
        r.error = p.problem;
        return r;
    }
    if (! folder.createDirectory())
    {
        r.error = "Couldn't make the project's Audio folder.";
        return r;
    }

    Engine::ExportSettings s;
    s.mix = false;
    s.stems = true;
    s.startBeat = p.startBeat;
    s.endBeat = p.endBeat;
    s.tailSeconds = tailSeconds;
    s.trackIds = p.playing;

    const double sr = engine.getSampleRate();
    r.file = folder.getNonexistentChildFile (File::createLegalFileName (p.groupName) + " bounce", ".wav", false);
    std::unique_ptr<AudioFormatWriter> writer;
    std::vector<bool> wanted;

    AudioBuffer<float> sum;
    int64 sumAt = -1;
    int sumLen = 0;
    int64 lastLoud = -1;
    bool writeFailed = false;

    auto flush = [&]
    {
        if (sumAt < 0 || sumLen <= 0 || writer == nullptr)
            return;
        for (int ch = 0; ch < 2; ++ch)
        {
            const auto* d = sum.getReadPointer (ch);
            for (int i = sumLen; --i >= 0;)
                if (std::abs (d[i]) > 1.0e-4f)   // -80 dB
                {
                    lastLoud = jmax (lastLoud, sumAt + i);
                    break;
                }
        }
        if (! writer->writeFromAudioSampleBuffer (sum, 0, sumLen))
            writeFailed = true;
        sumLen = 0;
    };

    auto prepare = [&] (const std::vector<Engine::RenderTap>& taps, int64) -> juce::Result
    {
        for (auto& t : taps)
            wanted.push_back (p.taps.count (t.trackId) > 0);
        std::unique_ptr<OutputStream> stream (r.file.createOutputStream());
        WavAudioFormat wav;
        if (stream != nullptr)
            writer = wav.createWriterFor (stream, AudioFormatWriterOptions().withSampleRate (sr)
                                                                            .withNumChannels (2)
                                                                            .withBitsPerSample (32)
                                                                            .withSampleFormat (AudioFormatWriterOptions::SampleFormat::floatingPoint));
        return writer != nullptr ? Result::ok() : Result::fail ("Couldn't create " + r.file.getFileName());
    };

    auto listen = [&] (int tap, const AudioBuffer<float>& b, int start, int count, int64 at)
    {
        if (tap < 0 || tap >= (int) wanted.size() || ! wanted[(size_t) tap])
            return;
        if (at != sumAt)
        {
            flush();
            sumAt = at;
            sumLen = count;
            sum.setSize (2, count, false, false, true);
            sum.clear();
        }
        for (int ch = 0; ch < 2; ++ch)
            sum.addFrom (ch, 0, b, jmin (ch, b.getNumChannels() - 1), start, jmin (count, sumLen));
    };

    const auto done = engine.renderOffline (s, progress ? progress : [] (double) { return true; }, prepare, listen);
    flush();
    writer.reset();

    if (done.failed())
    {
        r.file.deleteFile();
        r.cancelled = done.getErrorMessage().containsIgnoreCase ("cancel");
        r.error = r.cancelled ? String ("Bounce cancelled.") : done.getErrorMessage().replace ("exporting", "bouncing");
        return r;
    }
    if (writeFailed)
    {
        r.file.deleteFile();
        r.error = "Couldn't write " + r.file.getFileName() + " (is the disk full?).";
        return r;
    }
    if (lastLoud < 0)
    {
        r.file.deleteFile();
        r.error = "\"" + p.groupName + "\" is silent, so there's nothing to bounce (is its bus muted?).";
        return r;
    }

    // To the last clip's end at least, and on through any tail (a reverb ringing out).
    const double samplesPerBeat = sr * 60.0 / p.tempo;
    const double heard = (double) (lastLoud + 1) / samplesPerBeat;
    r.lengthBeats = jmax (p.endBeat - p.startBeat, heard);
    return r;
}

std::shared_ptr<Track> place (Edit& edit, const Plan& p, const Bounced& r)
{
    auto group = edit.findGroup (p.groupId);
    auto track = edit.addTrack (TrackKind::audio, p.groupName + " (bounced)");
    track->input = {};   // it plays the bounce; no interface input to record
    if (group != nullptr)
        track->colour = group->colour;

    // Right after the group's last track, outside the group (inside, it would play through the
    // group's bus a second time, and be muted with it).
    auto& list = edit.tracks;
    list.erase (std::remove (list.begin(), list.end(), track), list.end());
    auto at = list.end();
    for (auto it = list.begin(); it != list.end(); ++it)
        if ((*it)->groupId == p.groupId)
            at = it + 1;
    list.insert (at, track);

    Clip c;
    c.id = edit.newId();
    c.kind = ClipKind::audio;
    c.name = r.file.getFileNameWithoutExtension();
    c.file = r.file;
    c.start = r.startBeat;
    c.length = r.lengthBeats;
    track->clips.push_back (c);

    edit.muteGroupInNextStep (p.groupId);
    return track;
}
} // namespace lathe::bounce
