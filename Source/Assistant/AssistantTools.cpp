#include "AssistantTools.h"
#include "AssistantShared.h"

namespace lathe::assistant
{
namespace
{
double rounded (double x, double step = 1.0e-4)   { return std::round (x / step) * step; }

var obj()   { return var (new DynamicObject()); }

void set (var& o, const char* key, const var& value)
{
    o.getDynamicObject()->setProperty (key, value);
}

String monitorName (int m)
{
    return m == (int) Monitor::in ? "in" : m == (int) Monitor::off ? "off" : "auto";
}

bool isGenerator (const PluginSlot& s)
{
    return s.instance != nullptr && s.instance->producesMidi();
}

/** The generator a MIDI track's own notes (clips, played notes) go into before its
    instrument, if one is switched on. */
const PluginSlot* activeGeneratorBeforeInstrument (const Track& t)
{
    for (auto& p : t.plugins)
    {
        if (p->instance == nullptr || p->bypassed.load())
            continue;
        if (isGenerator (*p))
            return p.get();
        if (p->isInstrument())
            return nullptr;
    }
    return nullptr;
}

const PluginSlot* soundingInstrument (const Track& t)
{
    for (auto& p : t.plugins)
        if (p->instance != nullptr && ! p->bypassed.load() && p->isInstrument())
            return p.get();
    return nullptr;
}

String dbText (float db)
{
    return db <= kSilenceDb ? String ("-inf dB") : String (db, 1) + " dB";
}
} // namespace

Tools::Tools (Edit& e, Engine& en) : edit (e), engine (en) {}

//==============================================================================
var Tools::call (const String& name, const var& args, String& status)
{
    if (name == "describe_session")
        return toolText (JSON::toString (describeSession(), true));

    if (name == "describe_track")
    {
        auto t = findTrack (args.getProperty ("track", {}));
        if (t == nullptr)
            return toolText ("No track called \"" + args.getProperty ("track", {}).toString() + "\". describe_session lists them.", true);
        return toolText (JSON::toString (describeTrack (*t), true));
    }

    if (name == "get_notes")
        return getNotes (args);
    if (name == "play")
        return play (args, status);
    if (name == "stop")
        return stop (status);

    return toolText ("Unknown tool: " + name, true);
}

std::shared_ptr<Track> Tools::findTrack (const var& key) const
{
    auto byId = [this] (int id) { return edit.findTrack (id); };

    if (key.isInt() || key.isInt64() || key.isDouble())
        return byId ((int) key);

    const auto s = key.toString().trim();
    if (s.isEmpty())
        return {};
    if (s.equalsIgnoreCase ("master"))
        return edit.master;

    for (auto* list : { &edit.tracks, &edit.buses })
        for (auto& t : *list)
            if (t->name.equalsIgnoreCase (s))
                return t;

    if (s.containsOnly ("0123456789"))
        return byId (s.getIntValue());
    return {};
}

//==============================================================================
String Tools::describeInput (const Track& t) const
{
    if (t.input.isTrack())
    {
        auto src = edit.findTrack (t.input.track);
        return "from track \"" + (src != nullptr ? src->name : String ("(deleted)")) + "\"";
    }
    if (t.isMidi())
        return t.midiInput == "All" ? String ("all MIDI inputs") : t.midiInput == "None" ? String ("none") : "MIDI device \"" + t.midiInput + "\"";
    if (t.isAudio())
    {
        if (t.input.channel < 0)
            return "none";
        const auto names = engine.getInputChannelNames();
        const bool available = t.input.channel + (t.input.stereo ? 1 : 0) < names.size();
        return "interface input " + String (t.input.channel + 1) + (t.input.stereo ? "+" + String (t.input.channel + 2) : String())
               + (available ? String() : " (not available on the current interface)");
    }
    return {};
}

String Tools::describeOutput (const Track& t) const
{
    switch (t.output.kind)
    {
        case OutputRoute::Kind::bus:
        {
            auto b = edit.findTrack (t.output.busId);
            return "return \"" + (b != nullptr ? b->name : String ("(deleted)")) + "\"";
        }
        case OutputRoute::Kind::hardware:
            return "interface output " + String (t.output.channel + 1) + (t.output.stereo ? "+" + String (t.output.channel + 2) : String());
        case OutputRoute::Kind::master:
        default:
            return "master";
    }
}

var Tools::pluginList (const Track& t) const
{
    Array<var> list;
    for (size_t i = 0; i < t.plugins.size(); ++i)
    {
        auto& p = *t.plugins[i];
        var o = obj();
        set (o, "position", (int) i + 1);
        set (o, "name", p.getName());
        set (o, "maker", p.description.manufacturerName);
        set (o, "role", p.isMissing() ? "missing (not installed or not scanned)"
                        : isGenerator (p) ? (p.isInstrument() ? "note generator (also makes sound)" : "note generator")
                        : p.isInstrument() ? "instrument" : "effect");
        set (o, "bypassed", p.bypassed.load());
        list.add (o);
    }
    return list;
}

var Tools::trackSummary (const Track& t) const
{
    var o = obj();
    set (o, "id", t.id);
    set (o, "name", t.name);
    set (o, "kind", Edit::trackKindToString (t.kind));
    if (t.holdsClips())
    {
        set (o, "input", describeInput (t));
        set (o, "monitoring", monitorName (t.monitor.load()));
        set (o, "armed", t.arm.load());
    }
    set (o, "output", t.isMaster() ? String ("interface") : describeOutput (t));
    set (o, "muted", t.isMaster() ? t.mute.load() : edit.isEffectivelyMuted (t));
    if (! t.isMaster())
        set (o, "soloed", edit.isEffectivelySoloed (t));
    set (o, "volume_db", rounded (t.volumeDb.load(), 0.1));
    set (o, "pan", rounded (t.pan.load(), 0.01));
    set (o, "plugins", pluginList (t));
    if (t.holdsClips())
        set (o, "clips", (int) t.clips.size());
    if (auto g = edit.findGroup (t.groupId))
        set (o, "group", g->name);
    return o;
}

var Tools::describeSession() const
{
    var o = obj();
    set (o, "project", edit.getName());
    set (o, "tempo_bpm", edit.tempo);
    set (o, "time_signature", String (edit.timeSigNum) + "/" + String (edit.timeSigDen));
    set (o, "project_sample_rate", edit.sampleRate);

    var transport = obj();
    set (transport, "playing", engine.isPlaying());
    set (transport, "recording", engine.isRecording());
    set (transport, "counting_in", engine.isCountingIn());
    set (transport, "position_bar", rounded (toBar (engine.getPlayheadBeats()), 0.01));
    set (transport, "insert_marker_bar", rounded (toBar (edit.insertMarker), 0.01));
    set (transport, "metronome", engine.metronomeOn.load());
    var loop = obj();
    set (loop, "on", edit.loopEnabled);
    set (loop, "from_bar", rounded (toBar (edit.loopStart), 0.01));
    set (loop, "to_bar", rounded (toBar (edit.loopEnd), 0.01));
    set (transport, "loop", loop);
    set (o, "transport", transport);

    var device = obj();
    auto* d = engine.deviceManager.getCurrentAudioDevice();
    set (device, "name", d != nullptr ? d->getName() : String ("none"));
    set (device, "sample_rate", engine.getSampleRate());
    set (o, "audio_device", device);

    set (o, "song_length_bars", rounded (edit.getEndBeat() / edit.beatsPerBar(), 0.01));

    Array<var> tracks, returns, groups;
    for (auto& t : edit.tracks)
        tracks.add (trackSummary (*t));
    for (auto& t : edit.buses)
        returns.add (trackSummary (*t));
    for (auto& g : edit.groups)
    {
        var go = obj();
        set (go, "name", g->name);
        StringArray members;
        for (auto& m : edit.groupMembers (g->id))
            members.add (m->name);
        set (go, "tracks", members.joinIntoString (", "));
        set (go, "muted", g->mute.load());
        set (go, "soloed", g->solo.load());
        groups.add (go);
    }
    set (o, "tracks", tracks);
    set (o, "returns", returns);
    if (edit.master != nullptr)
        set (o, "master", trackSummary (*edit.master));
    if (! groups.isEmpty())
        set (o, "groups", groups);

    Array<var> problems;
    for (auto& p : findProblems())
        problems.add (p);
    set (o, "possible_problems", problems);
    return o;
}

var Tools::describeTrack (const Track& t) const
{
    auto o = trackSummary (t);

    if (t.isMidi())
    {
        set (o, "midi_channel", t.midiChannel);
        set (o, "midi_output", t.midiOutput.isNotEmpty() ? t.midiOutput : String ("none"));
    }

    Array<var> sends;
    for (auto& s : t.sends)
    {
        if (s->levelDb.load() <= kSilenceDb)
            continue;
        auto b = edit.findTrack (s->busId);
        var so = obj();
        set (so, "to", b != nullptr ? b->name : String ("(deleted)"));
        set (so, "level_db", rounded (s->levelDb.load(), 0.1));
        set (so, "pre_fader", s->preFader.load());
        sends.add (so);
    }
    set (o, "sends", sends);

    var eq = obj();
    set (eq, "on", t.eq.enabled.load());
    StringArray active;
    for (int i = 0; i < ChannelEq::numBands; ++i)
    {
        auto& b = t.eq.bands[i];
        if (! b.isActive())
            continue;
        String d = ChannelEq::bandName (i) + " " + String (roundToInt (b.freq.load())) + " Hz";
        if (b.hasGain())
            d << " " << (b.gainDb.load() >= 0 ? "+" : "") << String (b.gainDb.load(), 1) << " dB";
        active.add (d);
    }
    set (eq, "bands", active.isEmpty() ? String ("flat") : active.joinIntoString ("; "));
    set (o, "eq", eq);

    // Who takes this track's notes or sound as their input.
    StringArray feeds;
    for (auto& other : edit.tracks)
        if (other->input.isTrack() && other->input.track == t.id)
            feeds.add (other->name);
    if (! feeds.isEmpty())
        set (o, "feeds", feeds.joinIntoString (", "));

    Array<var> clips;
    for (auto& c : t.clips)
    {
        var co = obj();
        set (co, "id", c.id);
        set (co, "name", c.name);
        set (co, "start_bar", rounded (toBar (c.start)));
        set (co, "end_bar", rounded (toBar (c.end())));
        set (co, "length_beats", rounded (c.length));
        if (c.kind == ClipKind::midi)
        {
            int count = 0, lo = 128, hi = -1;
            for (auto& n : c.notes)
                if (n.start < c.length)
                {
                    ++count;
                    lo = jmin (lo, n.pitch);
                    hi = jmax (hi, n.pitch);
                }
            set (co, "notes", count);
            if (count > 0)
                set (co, "range", MidiMessage::getMidiNoteName (lo, true, true, 3) + "-" + MidiMessage::getMidiNoteName (hi, true, true, 3));
            if (! c.events.empty())
                set (co, "controller_events", (int) c.events.size());
        }
        else
        {
            set (co, "file", c.file.getFileName());
            set (co, "gain_db", rounded (c.gainDb, 0.1));
        }
        clips.add (co);
    }
    set (o, "clip_list", clips);
    return o;
}

StringArray Tools::findProblems() const
{
    StringArray out;

    if (engine.getSampleRate() <= 0.0)
        out.add ("No audio device is running, so nothing can be heard (Options > Audio / MIDI Settings).");

    if (edit.master != nullptr)
    {
        if (edit.master->mute.load())
            out.add ("The master is muted.");
        else if (edit.master->volumeDb.load() <= kSilenceDb)
            out.add ("The master fader is all the way down.");
    }

    StringArray soloed;
    for (auto& t : edit.tracks)
        if (edit.isEffectivelySoloed (*t))
            soloed.add (t->name);
    if (! soloed.isEmpty())
        out.add ("Only soloed tracks are heard: " + soloed.joinIntoString (", ") + ".");

    for (auto& tp : edit.tracks)
    {
        auto& t = *tp;
        const auto quoted = "\"" + t.name + "\"";

        if (t.mute.load())
            out.add (quoted + " is muted.");
        else if (auto g = edit.findGroup (t.groupId); g != nullptr && g->mute.load())
            out.add (quoted + " is muted by its group \"" + g->name + "\".");

        if (t.volumeDb.load() <= kSilenceDb)
            out.add (quoted + "'s fader is all the way down.");

        if (t.output.kind == OutputRoute::Kind::bus)
            if (auto b = edit.findTrack (t.output.busId); b != nullptr && b->mute.load())
                out.add (quoted + " plays through the return \"" + b->name + "\", which is muted.");

        for (auto& p : t.plugins)
            if (p->isMissing())
                out.add (quoted + ": the plug-in " + p->getName() + " isn't installed (or hasn't been scanned), so it's skipped.");

        if (t.input.isTrack())
        {
            auto src = edit.findTrack (t.input.track);
            const auto from = src != nullptr ? "\"" + src->name + "\"" : String ("a deleted track");
            const int mon = t.monitor.load();
            if (mon == (int) Monitor::off)
                out.add (quoted + " takes its input from " + from + ", but its monitoring is Off, so it never plays it.");
            else if (mon == (int) Monitor::automatic && ! t.arm.load())
                out.add (quoted + " takes its input from " + from + ", but only plays it while armed (or with monitoring set to In).");
        }

        if (t.isMidi())
        {
            if (auto* gen = activeGeneratorBeforeInstrument (t); gen != nullptr && ! t.clips.empty())
                out.add (quoted + "'s clips go into " + gen->getName() + " before anything else. A generator that only plays its "
                         "own notes (Dicey does) hides the clips: bypass " + gen->getName() + " to hear them.");

            bool feedsAnother = false;
            for (auto& other : edit.tracks)
                feedsAnother = feedsAnother || (other->input.isTrack() && other->input.track == t.id);

            if (soundingInstrument (t) == nullptr && t.midiOutput.isEmpty() && ! feedsAnother)
            {
                bool bypassedInstrument = false;
                for (auto& p : t.plugins)
                    bypassedInstrument = bypassedInstrument || (p->isInstrument() && p->bypassed.load());
                out.add (quoted + (bypassedInstrument ? "'s instrument is bypassed" : " has no instrument")
                         + " (and doesn't feed another track or a MIDI output), so it makes no sound.");
            }
        }
    }

    for (auto& b : edit.buses)
        if (b->mute.load())
            out.add ("The return \"" + b->name + "\" is muted.");

    return out;
}

//==============================================================================
var Tools::getNotes (const var& args) const
{
    auto t = findTrack (args.getProperty ("track", {}));
    if (t == nullptr)
        return toolText ("No track called \"" + args.getProperty ("track", {}).toString() + "\". describe_session lists them.", true);
    if (! t->isMidi())
        return toolText ("\"" + t->name + "\" is " + Edit::trackKindToString (t->kind) + " track; only MIDI tracks have notes.", true);

    const bool oneClip = args.hasProperty ("clip") && ! args["clip"].isVoid();
    const int clipId = oneClip ? (int) args["clip"] : -1;
    if (oneClip && t->findClip (clipId) == nullptr)
        return toolText ("\"" + t->name + "\" has no clip " + String (clipId) + ". describe_track lists its clips.", true);

    const double from = args.hasProperty ("from_bar") ? toBeats ((double) args["from_bar"]) : -1.0e9;
    const double to = args.hasProperty ("to_bar") ? toBeats ((double) args["to_bar"]) : 1.0e9;

    struct Row { double beat; const Note* n; int clip; };
    std::vector<Row> rows;
    for (auto& c : t->clips)
    {
        if (c.kind != ClipKind::midi || (oneClip && c.id != clipId))
            continue;
        for (auto& n : c.notes)
        {
            const double beat = c.start + n.start;
            if (n.start < c.length && beat >= from - 1.0e-9 && beat < to)
                rows.push_back ({ beat, &n, c.id });
        }
    }
    std::stable_sort (rows.begin(), rows.end(), [] (auto& a, auto& b) { return a.beat < b.beat; });

    constexpr size_t limit = 2000;
    Array<var> notes;
    for (size_t i = 0; i < rows.size() && i < limit; ++i)
    {
        auto& r = rows[i];
        var n = obj();
        set (n, "pitch", r.n->pitch);
        set (n, "name", MidiMessage::getMidiNoteName (r.n->pitch, true, true, 3));
        set (n, "start_bar", rounded (toBar (r.beat)));
        set (n, "start_beat", rounded (r.beat));
        set (n, "length_beats", rounded (r.n->length));
        set (n, "velocity", jlimit (1, 127, roundToInt (r.n->velocity * 127.0f)));
        set (n, "clip", r.clip);
        notes.add (n);
    }

    var o = obj();
    set (o, "track", t->name);
    set (o, "time_signature", String (edit.timeSigNum) + "/" + String (edit.timeSigDen));
    set (o, "total", (int) rows.size());
    if (rows.size() > limit)
        set (o, "truncated", "Only the first " + String ((int) limit) + " notes are listed. Ask for fewer bars or one clip.");
    set (o, "notes", notes);
    return toolText (JSON::toString (o, true));
}

var Tools::play (const var& args, String& status)
{
    if (engine.isRecording() || engine.isCountingIn())
        return toolText ("Lathe is recording. Stop first (the stop tool keeps the take).", true);

    const bool jump = args.hasProperty ("from_bar") && ! args["from_bar"].isVoid();
    if (jump)
    {
        const double bar = (double) args["from_bar"];
        if (! (bar >= 1.0) || bar > 100000.0)
            return toolText ("from_bar must be 1 or more.", true);
        const double beats = toBeats (bar);
        edit.insertMarker = beats;
        if (engine.isPlaying())
            engine.seekTo (beats);
        edit.sendChangeMessage();
    }
    else if (engine.isPlaying())
    {
        return toolText ("Already playing, at bar " + String (toBar (engine.getPlayheadBeats()), 2) + ".");
    }

    if (! engine.isPlaying())
        engine.play();

    const auto where = String (toBar (edit.insertMarker), 2).trimCharactersAtEnd ("0").trimCharactersAtEnd (".");
    status = "AI assistant: playing from bar " + where;
    String text = "Playing from bar " + where + " at " + String (edit.tempo, 1).trimCharactersAtEnd ("0").trimCharactersAtEnd (".") + " bpm.";
    if (engine.getSampleRate() <= 0.0)
        text << " Note: no audio device is running, so nothing will be heard.";
    return toolText (text);
}

var Tools::stop (String& status)
{
    if (engine.isRecording() || engine.isCountingIn())
    {
        const bool counting = engine.isCountingIn();
        engine.stop();
        status = counting ? "AI assistant: stopped the count-in" : "AI assistant: stopped recording";
        return toolText (counting ? "Stopped the count-in; nothing was recorded."
                                  : "Stopped. The recording was kept (as if Stop was pressed).");
    }
    if (! engine.isPlaying())
        return toolText ("Lathe wasn't playing.");

    const auto at = toBar (engine.getPlayheadBeats());
    engine.stop();
    status = "AI assistant: stopped";
    return toolText ("Stopped at bar " + String (at, 2) + ". The playhead went back to the insert marker (bar "
                     + String (toBar (edit.insertMarker), 2) + ").");
}
} // namespace lathe::assistant
