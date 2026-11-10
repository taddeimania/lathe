#include "AssistantTools.h"
#include "AssistantShared.h"
#include "../Devices/Device.h"

#if JUCE_MAC
 #include <sys/sysctl.h>
#endif

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

Tools::Tools (Edit& e, Engine& en) : edit (e), engine (en)
{
    loadPlugin = [this] (const PluginDescription& d, String& error) { return engine.makeSlot (d, {}, error); };
}

//==============================================================================
var Tools::call (const String& name, const var& args, String& status)
{
    if (isLongTool (name))   // they play for a while: Service::callLongTool runs them (startLongTool)
        return toolText (name + " has to run through Lathe's assistant connection.", true);
    if (name == "describe_session")
        return toolText (JSON::toString (describeSession(), true, 4));

    if (name == "describe_track")
    {
        auto t = findTrack (args.getProperty ("track", {}));
        if (t == nullptr)
            return toolText ("No track called \"" + args.getProperty ("track", {}).toString() + "\". describe_session lists them.", true);
        return toolText (JSON::toString (describeTrack (*t), true, 4));
    }

    if (name == "get_notes")
        return getNotes (args);
    if (name == "write_notes")
        return writeNotes (args, status);
    if (name == "play")
        return play (args, status);
    if (name == "stop")
        return stop (status);
    if (name == "set_loop")
        return setLoop (args, status);
    if (name == "list_plugins")
        return listPlugins (args);
    if (name == "edit_session")
        return editSession (args, status);

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
    if (const double at = engine.getAutoStopBeats(); at >= 0.0)
        set (transport, "stops_at_bar", rounded (toBar (at), 0.01));
    if (const int passes = engine.getLoopPassesLeft(); passes > 0)
        set (transport, "loop_passes_left", passes);
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
                         "own notes (most do) hides the clips: bypass " + gen->getName() + " to hear them.");

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
    return toolText (JSON::toString (o, true, 4));
}

namespace
{
/** 60, "60", "C3", "F#2", "Bb-1" (C3 = middle C, as in Lathe). -1 if it isn't one. */
int parsePitch (const var& v)
{
    if (v.isInt() || v.isInt64() || v.isDouble())
    {
        const double d = (double) v;
        return std::abs (d - std::round (d)) < 1.0e-9 && d >= 0.0 && d <= 127.0 ? (int) std::round (d) : -1;
    }
    auto s = v.toString().trim().removeCharacters (" ");
    if (s.isEmpty())
        return -1;
    if (s.containsOnly ("0123456789"))
        return s.getIntValue() <= 127 ? s.getIntValue() : -1;

    static const int semis[] { 9, 11, 0, 2, 4, 5, 7 };   // A B C D E F G
    const auto letter = CharacterFunctions::toUpperCase (s[0]);
    if (letter < 'A' || letter > 'G')
        return -1;
    int pitch = semis[letter - 'A'];
    int i = 1;
    for (; i < s.length() && (s[i] == '#' || s[i] == 'b'); ++i)
        pitch += s[i] == '#' ? 1 : -1;
    const auto octave = s.substring (i);
    if (octave.isEmpty() || ! octave.trimCharactersAtStart ("-").containsOnly ("0123456789"))
        return -1;
    pitch += (octave.getIntValue() + 2) * 12;
    return pitch >= 0 && pitch <= 127 ? pitch : -1;
}
} // namespace

var Tools::writeNotes (const var& args, String& status)
{
    auto t = findTrack (args.getProperty ("track", {}));
    if (t == nullptr)
        return toolText ("No track called \"" + args.getProperty ("track", {}).toString() + "\". describe_session lists them.", true);
    if (! t->isMidi())
        return toolText (("\"" + t->name + "\"") + " is " + Edit::trackKindToString (t->kind) + " track; only MIDI tracks have notes.", true);
    if (engine.isRecording() || engine.isCountingIn())
        return toolText ("Lathe is recording. Stop first (the stop tool keeps the take).", true);

    const double fromBar = (double) args.getProperty ("from_bar", 0.0), toBar_ = (double) args.getProperty ("to_bar", 0.0);
    if (! (fromBar >= 1.0) || ! (toBar_ > fromBar) || toBar_ > 100000.0)
        return toolText ("from_bar must be 1 or more, and to_bar after it.", true);
    const int repeat = (int) args.getProperty ("repeat", 1);
    if (repeat < 1 || repeat > 256)
        return toolText ("repeat must be 1 to 256.", true);
    const auto mode = args.getProperty ("mode", "replace").toString().trim().toLowerCase();
    if (mode != "replace" && mode != "add")
        return toolText ("mode is \"replace\" or \"add\".", true);
    auto* list = args.getProperty ("notes", {}).getArray();
    if (list == nullptr || list->isEmpty())
        return toolText ("Give \"notes\": a list of { pitch, start_bar, length_beats, velocity }.", true);
    if (list->size() * repeat > 4000)
        return toolText ("At most 4000 notes per call, repeats included.", true);

    const double from = toBeats (fromBar), span = toBeats (toBar_) - from, to = from + span * repeat;

    std::vector<Note> notes;
    for (int k = 0; k < list->size(); ++k)
    {
        const auto& v = list->getReference (k);
        const auto where = "Note " + String (k + 1) + ": ";
        const int pitch = parsePitch (v.getProperty ("pitch", {}));
        if (pitch < 0)
            return toolText (where + "pitch " + JSON::toString (v.getProperty ("pitch", {})) + " isn't a MIDI number (0-127) or a name like C3 (middle C).", true);
        if (! v.hasProperty ("start_bar"))
            return toolText (where + "give start_bar.", true);
        const double start = toBeats ((double) v["start_bar"]);
        if (start < from - 1.0e-9 || start >= from + span - 1.0e-9)
            return toolText (where + "start_bar " + String ((double) v["start_bar"]) + " is outside bars "
                             + barText (from) + " to " + barText (from + span) + ".", true);
        const double length = (double) v.getProperty ("length_beats", 0.25);
        if (! (length > 0.0) || length > 10000.0)
            return toolText (where + "length_beats must be more than 0.", true);
        const int velocity = (int) v.getProperty ("velocity", 100);
        if (velocity < 1 || velocity > 127)
            return toolText (where + "velocity must be 1-127.", true);
        notes.push_back ({ pitch, start - from, length, (float) velocity / 127.0f });
    }

    edit.commitUndoStep();   // what was done before this call stays a step of its own
    edit.beginClipEdit();

    // "add" joins the clip that already covers the bars, if there's exactly that one.
    Clip* target = nullptr;
    if (mode == "add")
    {
        std::vector<Clip*> overlapping;
        for (auto& c : t->clips)
            if (c.kind == ClipKind::midi && c.end() > from + 1.0e-9 && c.start < to - 1.0e-9)
                overlapping.push_back (&c);
        if (overlapping.size() == 1 && overlapping[0]->start <= from + 1.0e-9 && overlapping[0]->end() >= to - 1.0e-9)
            target = overlapping[0];
        else if (! overlapping.empty())
        {
            edit.discardUncommitted();
            return toolText ("Those bars are partly covered by " + String ((int) overlapping.size())
                             + " clip(s) on " + ("\"" + t->name + "\"") + ", so the notes can't be added to one. Use mode \"replace\", "
                             "or bars that one clip covers (describe_track lists them).", true);
        }
    }

    if (target == nullptr)
    {
        if (mode == "replace")
            edit.clearRange (*t, from, to);
        Clip c;
        c.id = edit.newId();
        c.kind = ClipKind::midi;
        c.name = t->name;
        c.start = from;
        c.length = to - from;
        t->clips.push_back (c);
        target = &t->clips.back();
    }

    const double offset = from - target->start;
    for (int r = 0; r < repeat; ++r)
        for (auto n : notes)
        {
            n.start += offset + span * r;
            target->notes.push_back (n);
        }
    std::stable_sort (target->notes.begin(), target->notes.end(), [] (const Note& a, const Note& b) { return a.start < b.start; });
    const int clipId = target->id;
    edit.changed();

    const int count = (int) notes.size() * repeat;
    status = "AI assistant: wrote " + String (count) + " notes on " + ("\"" + t->name + "\"");
    return toolText ("Wrote " + String (count) + (count == 1 ? " note" : " notes") + " on " + ("\"" + t->name + "\"")
                     + ", bars " + barText (from) + " to " + barText (to) + " (clip " + String (clipId) + "). "
                     "In Lathe this is one undo step (Cmd/Ctrl+Z).");
}

String Tools::barText (double beats) const
{
    return String (toBar (beats), 2).trimCharactersAtEnd ("0").trimCharactersAtEnd (".");
}

var Tools::play (const var& args, String& status)
{
    if (engine.isRecording() || engine.isCountingIn())
        return toolText ("Lathe is recording. Stop first (the stop tool keeps the take).", true);

    auto given = [&args] (const char* k) { return args.hasProperty (k) && ! args[k].isVoid(); };
    const bool hasFrom = given ("from_bar"), hasTo = given ("to_bar"), hasTimes = given ("loop_times");
    const bool loopOn = edit.loopEnabled && edit.loopEnd > edit.loopStart;
    const auto loopText = "bars " + barText (edit.loopStart) + " to " + barText (edit.loopEnd);

    if (hasTo && hasTimes)
        return toolText ("Use to_bar or loop_times, not both.", true);

    double startBeats = engine.isPlaying() ? engine.getPlayheadBeats() : edit.insertMarker;
    if (hasFrom)
    {
        const double bar = (double) args["from_bar"];
        if (! (bar >= 1.0) || bar > 100000.0)
            return toolText ("from_bar must be 1 or more.", true);
        startBeats = toBeats (bar);
    }

    int times = 0;
    if (hasTimes)
    {
        times = (int) args["loop_times"];
        if (times < 1 || times > 1000)
            return toolText ("loop_times must be 1 to 1000.", true);
        if (! loopOn)
            return toolText ("The loop is off. Turn it on with set_loop (from_bar and to_bar) first.", true);
        if (! hasFrom)
            startBeats = edit.loopStart;
        else if (startBeats < edit.loopStart - 1.0e-9 || startBeats >= edit.loopEnd)
            return toolText ("from_bar must be inside the loop (" + loopText + ").", true);
    }

    double stopBeats = -1.0;
    if (hasTo)
    {
        const double bar = (double) args["to_bar"];
        if (! (bar >= 1.0) || bar > 100000.0)
            return toolText ("to_bar must be 1 or more.", true);
        stopBeats = toBeats (bar);
        if (stopBeats <= startBeats + 1.0e-9)
            return toolText ("to_bar must be after where playback starts (bar " + barText (startBeats) + ").", true);
        if (loopOn && startBeats < edit.loopEnd && stopBeats > edit.loopEnd + 1.0e-9)
            return toolText ("The loop is on (" + loopText + "), so playback never gets to bar " + barText (stopBeats)
                             + ". Use loop_times to play the loop a number of times, turn the loop off with set_loop, "
                               "or pick a to_bar inside the loop.", true);
    }

    if (engine.isPlaying() && ! hasFrom && ! hasTimes && ! hasTo)
        return toolText ("Already playing, at bar " + barText (engine.getPlayheadBeats()) + ".");

    if (hasFrom || hasTimes)
    {
        edit.insertMarker = startBeats;
        if (engine.isPlaying())
            engine.seekTo (startBeats);
        edit.sendChangeMessage();
    }
    if (! engine.isPlaying())
        engine.play();
    engine.setAutoStop (stopBeats, times);   // after play(): starting or seeking cancels any earlier one

    const auto tempo = String (edit.tempo, 1).trimCharactersAtEnd ("0").trimCharactersAtEnd (".");
    String text;
    if (hasTimes)
    {
        text = "Playing the loop (" + loopText + ") " + (times == 1 ? String ("once") : String (times) + " times")
             + " at " + tempo + " bpm, then stopping at bar " + barText (edit.loopEnd) + ".";
        status = "AI assistant: playing the loop " + (times == 1 ? String ("once") : String (times) + " times");
    }
    else if (hasTo)
    {
        text = "Playing from bar " + barText (startBeats) + " at " + tempo + " bpm. Lathe stops by itself at bar "
             + barText (stopBeats) + ".";
        status = "AI assistant: playing bars " + barText (startBeats) + " to " + barText (stopBeats);
    }
    else
    {
        text = "Playing from bar " + barText (startBeats) + " at " + tempo + " bpm.";
        if (loopOn && startBeats < edit.loopEnd)
            text << " The loop is on (" << loopText << "), so it repeats until stopped.";
        status = "AI assistant: playing from bar " + barText (startBeats);
    }
    text << " Afterwards the playhead goes back to bar " << barText (edit.insertMarker) << ".";
    if (engine.getSampleRate() <= 0.0)
        text << " Note: no audio device is running, so nothing will be heard.";
    return toolText (text);
}

var Tools::setLoop (const var& args, String& status)
{
    auto given = [&args] (const char* k) { return args.hasProperty (k) && ! args[k].isVoid(); };
    const bool hasFrom = given ("from_bar"), hasTo = given ("to_bar");
    const bool on = given ("on") ? (bool) args["on"] : true;

    if (hasFrom != hasTo)
        return toolText ("Give both from_bar and to_bar (or neither, to just turn the loop on or off).", true);

    if (hasFrom)
    {
        const double from = (double) args["from_bar"], to = (double) args["to_bar"];
        if (! (from >= 1.0) || ! (to > from) || to > 100000.0)
            return toolText ("from_bar must be 1 or more, and to_bar after it.", true);
        edit.loopStart = toBeats (from);
        edit.loopEnd = toBeats (to);
    }
    else if (on && ! (edit.loopEnd > edit.loopStart))
        return toolText ("There's no loop region yet: give from_bar and to_bar.", true);

    const bool wasOn = edit.loopEnabled;
    edit.loopEnabled = on;
    edit.changed();   // the engine picks up the new loop; the project is marked as changed

    const auto region = "bars " + barText (edit.loopStart) + " to " + barText (edit.loopEnd);
    const double bars = (edit.loopEnd - edit.loopStart) / edit.beatsPerBar();
    const auto length = String (bars, 2).trimCharactersAtEnd ("0").trimCharactersAtEnd (".");
    status = on ? "AI assistant: loop " + region : String ("AI assistant: loop off");
    if (on)
        return toolText ("Loop on: " + region + " (" + length + (bars == 1.0 ? " bar" : " bars") + "). "
                         + (engine.isPlaying() ? "Playback loops there now." : "play with loop_times plays it a number of times."));
    return toolText (String (wasOn ? "Loop off" : "The loop was already off") + " (the region stays at " + region + ").");
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

//==============================================================================
// Plug-ins

String Tools::pluginKind (const PluginDescription& d) const
{
    // Lathe's own devices say what they are.
    if (auto* info = devices::find (d))
        return info->kind == devices::Kind::instrument ? "instrument" : "effect";

    // Loaded in this project already: then the plug-in itself says.
    for (auto* list : { &edit.tracks, &edit.buses })
        for (auto& t : *list)
            for (auto& p : t->plugins)
                if (p->instance != nullptr && p->description.name.equalsIgnoreCase (d.name) && isGenerator (*p) && ! p->isInstrument())
                    return "note generator";

    const auto category = d.category.toLowerCase();
    if (category.contains ("generator") || category.contains ("midi"))
        return "note generator";
    return d.isInstrument ? "instrument" : "effect";
}

std::optional<PluginDescription> Tools::findInstalledPlugin (const String& wanted, String& error) const
{
    const auto name = wanted.trim();
    const auto types = engine.getPluginHost().allTypes();   // Lathe's own first, so "Reverb" means Lathe's

    for (auto& d : types)
        if (d.name.equalsIgnoreCase (name))
            return d;

    std::vector<PluginDescription> partial;
    StringArray names;
    for (auto& d : types)
    {
        if (d.name.containsIgnoreCase (name) || (d.manufacturerName + " " + d.name).containsIgnoreCase (name))
        {
            if (! names.contains (d.name, true))
            {
                names.add (d.name);
                partial.push_back (d);
            }
        }
    }

    if (partial.size() == 1)
        return partial.front();
    if (partial.size() > 1)
        error = "\"" + name + "\" could be " + names.joinIntoString (", ") + ". Give the full name.";
    else
        error = "No installed plug-in called \"" + name + "\". list_plugins shows what's installed (a plug-in installed "
                "just now needs a scan: Options > Scan for New Plug-ins).";
    return {};
}

var Tools::listPlugins (const var& args) const
{
    const auto kindFilter = args.getProperty ("kind", {}).toString().trim().toLowerCase();
    const auto search = args.getProperty ("search", {}).toString().trim();

    auto types = engine.getPluginHost().allTypes();
    std::stable_sort (types.begin(), types.end(), [] (const PluginDescription& a, const PluginDescription& b)
    {
        // Lathe's own first, then the rest by name.
        if (devices::isBuiltIn (a) != devices::isBuiltIn (b))
            return devices::isBuiltIn (a);
        return a.name.compareIgnoreCase (b.name) < 0;
    });

    Array<var> list;
    StringArray seen;
    for (auto& d : types)
    {
        const auto kind = pluginKind (d);
        if (kindFilter.isNotEmpty() && ! kind.startsWithIgnoreCase (kindFilter.upToFirstOccurrenceOf (" ", false, false)))
            continue;
        if (search.isNotEmpty() && ! d.name.containsIgnoreCase (search) && ! d.manufacturerName.containsIgnoreCase (search))
            continue;
        const auto key = d.name.toLowerCase() + "|" + d.pluginFormatName;
        if (seen.contains (key))
            continue;
        seen.add (key);

        var o = obj();
        set (o, "name", d.name);
        set (o, "maker", d.manufacturerName);
        set (o, "kind", kind);
        set (o, "format", devices::isBuiltIn (d) ? String ("built into Lathe") : d.pluginFormatName);
        if (devices::isBuiltIn (d))
            set (o, "about", d.descriptiveName);
        if (d.version.isNotEmpty())
            set (o, "version", d.version);
        list.add (o);
    }

    var o = obj();
    set (o, "plugins", list);
    if (engine.getPluginHost().isScanning())
        set (o, "note", "Lathe is scanning for plug-ins right now; the list may grow.");
    set (o, "how_they_go", "Instruments and note generators go on MIDI tracks: a note generator goes before the "
                           "instrument it drives. Effects go on any track, return or the master.");
    return toolText (JSON::toString (o, true, 4));
}

//==============================================================================
// Changing the session. One call is a batch: every change lands, as one undo step, or
// none does (the first one that can't be made stops it and nothing is kept).

namespace
{
struct Batch
{
    Edit& edit;
    std::vector<std::function<void()>> revert;   // for what undo doesn't cover (levels, monitoring)
    StringArray notes;                           // things worth telling the assistant

    template <typename T>
    void remember (std::atomic<T>& value)
    {
        const T old = value.load();
        revert.push_back ([&value, old] { value = old; });
    }
};

String trackWord (const Track& t)
{
    return t.isMidi() ? "MIDI track" : t.isAudio() ? "audio track" : t.isBus() ? "return" : "master";
}

String quoted (const String& s)   { return "\"" + s + "\""; }

bool hasKey (const var& c, const char* key)   { return c.hasProperty (key); }

int pluginRole (const PluginSlot& s, const String& catalogKind)
{
    // 0 = generator, 1 = instrument, 2 = effect
    if (catalogKind == "note generator")
        return 0;
    if (s.isInstrument())
        return 1;
    if (isGenerator (s))
        return 0;
    return 2;
}
} // namespace

var Tools::editSession (const var& args, String& status)
{
    auto* changes = args.getProperty ("changes", {}).getArray();
    if (changes == nullptr || changes->isEmpty())
        return toolText ("Give \"changes\": a list of changes, each with an \"action\" (see the tool description).", true);
    if (changes->size() > 100)
        return toolText ("At most 100 changes per call.", true);
    if (engine.isRecording())
        return toolText ("Lathe is recording. Stop first (the stop tool keeps the take), then change the session.", true);

    edit.commitUndoStep();   // what was done before this call stays a step of its own

    Batch batch { edit, {}, {} };
    StringArray done;

    auto nameTaken = [this] (const String& name, const Track* except)
    {
        if (name.equalsIgnoreCase ("master"))
            return true;
        for (auto* list : { &edit.tracks, &edit.buses })
            for (auto& t : *list)
                if (t.get() != except && t->name.equalsIgnoreCase (name))
                    return true;
        return false;
    };

    auto findPluginOn = [] (Track& t, const var& key, String& error) -> std::shared_ptr<PluginSlot>
    {
        if (key.isInt() || key.isInt64() || key.isDouble() || key.toString().trim().containsOnly ("0123456789"))
        {
            const int pos = key.isString() ? key.toString().getIntValue() : (int) key;
            if (pos >= 1 && pos <= (int) t.plugins.size())
                return t.plugins[(size_t) pos - 1];
            error = quoted (t.name) + " has " + String ((int) t.plugins.size()) + " plug-in(s); there's no position " + String (pos) + ".";
            return {};
        }
        const auto name = key.toString().trim();
        for (auto& p : t.plugins)
            if (p->getName().equalsIgnoreCase (name))
                return p;
        for (auto& p : t.plugins)
            if (p->getName().containsIgnoreCase (name))
                return p;
        StringArray names;
        for (auto& p : t.plugins)
            names.add (p->getName());
        error = quoted (t.name) + " has no plug-in called " + quoted (name)
                + (names.isEmpty() ? String (" (it has none).") : " (it has " + names.joinIntoString (", ") + ").");
        return {};
    };

    auto addPlugin = [&] (std::shared_ptr<Track> t, const String& pluginName, const var& position, String& error,
                          std::shared_ptr<PluginSlot>* added = nullptr) -> String
    {
        auto desc = findInstalledPlugin (pluginName, error);
        if (! desc.has_value())
            return {};

        const auto kind = pluginKind (*desc);
        if ((kind == "instrument" || kind == "note generator") && ! t->isMidi())
        {
            error = desc->name + " is " + (kind == "instrument" ? "an instrument" : "a note generator") + ", so it goes on a MIDI track, and "
                    + quoted (t->name) + " is " + (t->isAudio() ? "an audio track" : t->isBus() ? "a return" : "the master")
                    + ". Add a MIDI track for it (add_track with kind \"midi\").";
            return {};
        }
        if (kind == "instrument")
        {
            for (auto& p : t->plugins)
            {
                if (p->isInstrument() && pluginRole (*p, pluginKind (p->description)) == 1)
                {
                    error = quoted (t->name) + " already has an instrument (" + p->getName() + "). A MIDI track plays one: add a new MIDI "
                            "track for " + desc->name + ", or remove " + p->getName() + " first.";
                    return {};
                }
            }
        }

        String loadError;
        auto slot = loadPlugin (*desc, loadError);
        if (slot == nullptr || slot->instance == nullptr)
        {
            error = "couldn't load " + desc->name + (loadError.isNotEmpty() ? ": " + loadError : String());
            return {};
        }

        const int role = pluginRole (*slot, kind);
        int index = (int) t->plugins.size();
        if (! position.isVoid() && position.toString().isNotEmpty())
        {
            const auto p = position.toString().trim();
            index = p.equalsIgnoreCase ("first") ? 0 : p.equalsIgnoreCase ("last") ? (int) t->plugins.size()
                                                      : jlimit (0, (int) t->plugins.size(), p.getIntValue() - 1);
        }
        else if (role < 2)
        {
            // Generators before everything else they drive; an instrument before the effects.
            index = (int) t->plugins.size();
            for (size_t i = 0; i < t->plugins.size(); ++i)
            {
                const int other = pluginRole (*t->plugins[i], pluginKind (t->plugins[i]->description));
                if (other > role)
                {
                    index = (int) i;
                    break;
                }
            }
        }

        if (t->isBus())
            devices::startOnReturn (*slot->instance);
        t->plugins.insert (t->plugins.begin() + index, slot);
        if (added != nullptr)
            *added = slot;
        return "Added " + desc->name + " to " + quoted (t->name) + " at position " + String (index + 1)
               + (role == 0 ? " (note generator)" : role == 1 ? " (instrument)" : String());
    };

    auto applyOne = [&] (const var& c, String& error) -> String
    {
        const auto action = c.getProperty ("action", {}).toString().trim().toLowerCase();
        auto needTrack = [&] (const char* key = "track") -> std::shared_ptr<Track>
        {
            auto t = findTrack (c.getProperty (key, {}));
            if (t == nullptr)
                error = "no track called " + quoted (c.getProperty (key, {}).toString()) + " (describe_session lists them).";
            return t;
        };

        if (action == "add_track")
        {
            const auto kindName = c.getProperty ("kind", "audio").toString().trim().toLowerCase();
            const TrackKind kind = kindName == "midi" ? TrackKind::midi
                                 : (kindName == "return" || kindName == "bus") ? TrackKind::bus : TrackKind::audio;
            if (kindName != "midi" && kindName != "audio" && kindName != "return" && kindName != "bus")
            {
                error = "kind must be \"audio\", \"midi\" or \"return\".";
                return {};
            }
            const auto name = c.getProperty ("name", {}).toString().trim();
            if (name.isNotEmpty() && nameTaken (name, nullptr))
            {
                error = "there's already a track called " + quoted (name) + ". Track names must be different so they can be told apart.";
                return {};
            }

            auto t = edit.addTrack (kind, name);
            String line = "Added " + trackWord (*t) + " " + quoted (t->name);

            if (auto* plugins = c.getProperty ("plugins", {}).getArray())
            {
                for (auto& p : *plugins)
                {
                    addPlugin (t, p.toString(), {}, error);
                    if (error.isNotEmpty())
                        return {};
                }
                StringArray chain;
                for (auto& p : t->plugins)
                    chain.add (p->getName());
                if (! chain.isEmpty())
                    line << " with " << chain.joinIntoString (" -> ");
            }
            if (c.hasProperty ("position"))
            {
                const int pos = (int) c["position"];
                edit.moveTrack (t->id, pos - 1);
            }
            return line;
        }

        if (action == "remove_track")
        {
            auto t = needTrack();
            if (t == nullptr) return {};
            if (t->isMaster()) { error = "the master can't be removed."; return {}; }
            for (auto& p : t->plugins)
                p->window.reset();
            const auto name = t->name;
            edit.removeTrack (t->id);
            return "Removed " + trackWord (*t) + " " + quoted (name);
        }

        if (action == "rename_track")
        {
            auto t = needTrack();
            if (t == nullptr) return {};
            const auto name = c.getProperty ("name", {}).toString().trim();
            if (t->isMaster()) { error = "the master keeps its name."; return {}; }
            if (name.isEmpty()) { error = "give the new \"name\"."; return {}; }
            if (nameTaken (name, t.get())) { error = "there's already a track called " + quoted (name) + "."; return {}; }
            const auto old = t->name;
            t->name = name;
            return "Renamed " + quoted (old) + " to " + quoted (name);
        }

        if (action == "move_track")
        {
            auto t = needTrack();
            if (t == nullptr) return {};
            if (t->isMaster() || ! hasKey (c, "position")) { error = t->isMaster() ? "the master doesn't move." : "give \"position\" (1 = first)."; return {}; }
            edit.moveTrack (t->id, (int) c["position"] - 1);
            return "Moved " + quoted (t->name) + " to position " + String ((int) c["position"]);
        }

        if (action == "add_plugin")
        {
            auto t = needTrack();
            if (t == nullptr) return {};
            const auto plugin = c.getProperty ("plugin", {}).toString();
            if (plugin.trim().isEmpty()) { error = "give \"plugin\": its name (list_plugins)."; return {}; }
            std::shared_ptr<PluginSlot> slot;
            auto line = addPlugin (t, plugin, c.getProperty ("position", {}), error, &slot);
            if (slot != nullptr && c.hasProperty ("on") && ! (bool) c["on"])
            {
                slot->bypassed = true;
                line << ", bypassed";
            }
            return line;
        }

        if (action == "remove_plugin" || action == "move_plugin" || action == "set_plugin_on")
        {
            auto t = needTrack();
            if (t == nullptr) return {};
            auto slot = findPluginOn (*t, c.getProperty ("plugin", {}), error);
            if (slot == nullptr) return {};

            if (action == "remove_plugin")
            {
                slot->window.reset();
                t->plugins.erase (std::find (t->plugins.begin(), t->plugins.end(), slot));
                return "Removed " + slot->getName() + " from " + quoted (t->name);
            }
            if (action == "move_plugin")
            {
                if (! hasKey (c, "position")) { error = "give \"position\" (1 = first in the chain)."; return {}; }
                t->plugins.erase (std::find (t->plugins.begin(), t->plugins.end(), slot));
                const int index = jlimit (0, (int) t->plugins.size(), (int) c["position"] - 1);
                t->plugins.insert (t->plugins.begin() + index, slot);
                return "Moved " + slot->getName() + " on " + quoted (t->name) + " to position " + String (index + 1);
            }
            if (! hasKey (c, "on")) { error = "give \"on\": true to switch it on, false to bypass it."; return {}; }
            slot->bypassed = ! (bool) c["on"];
            return String ((bool) c["on"] ? "Switched on " : "Bypassed ") + slot->getName() + " on " + quoted (t->name);
        }

        if (action == "set_input")
        {
            auto t = needTrack();
            if (t == nullptr) return {};
            if (! t->holdsClips()) { error = quoted (t->name) + " is a " + trackWord (*t) + ": only audio and MIDI tracks have inputs."; return {}; }

            if (hasKey (c, "from_track"))
            {
                auto src = needTrack ("from_track");
                if (src == nullptr) return {};
                if (src == t || ! src->holdsClips() || (t->isMidi() && ! src->isMidi()))
                {
                    error = t->isMidi() ? "a MIDI track takes its notes from another MIDI track." : "an audio track takes its sound from another audio or MIDI track.";
                    return {};
                }
                // No loops: follow the source's own input back.
                int hops = 0;
                for (auto s = src; s != nullptr && s->input.isTrack() && ++hops < 1000; s = edit.findTrack (s->input.track))
                    if (s->input.track == t->id) { error = quoted (src->name) + " already takes its input from " + quoted (t->name) + " (that would be a loop)."; return {}; }

                t->input.track = src->id;
                String line = quoted (t->name) + " now takes " + (t->isMidi() ? "the notes " : "the sound ") + quoted (src->name) + " plays";
                if (! hasKey (c, "monitor") && t->monitor.load() != (int) Monitor::in)
                {
                    batch.remember (t->monitor);
                    t->monitor = (int) Monitor::in;
                    line << " (monitoring set to In, so it plays them without being armed)";
                }
                return line;
            }

            if (hasKey (c, "interface_input"))
            {
                if (! t->isAudio()) { error = "MIDI tracks take a MIDI input: use midi_input."; return {}; }
                const int ch = (int) c["interface_input"] - 1;
                const bool stereo = (bool) c.getProperty ("stereo", false);
                const int available = engine.getInputChannelNames().size();
                if (ch < 0 || ch + (stereo ? 1 : 0) >= available)
                {
                    error = "the interface has " + String (available) + " input(s); interface_input " + String (ch + 1) + (stereo ? " (stereo)" : "") + " isn't one.";
                    return {};
                }
                t->input.track = -1;
                t->input.channel = ch;
                t->input.stereo = stereo;
                return quoted (t->name) + " now records interface input " + String (ch + 1) + (stereo ? "+" + String (ch + 2) : String());
            }

            if (hasKey (c, "midi_input"))
            {
                if (! t->isMidi()) { error = "audio tracks take interface_input or from_track."; return {}; }
                auto wanted = c["midi_input"].toString().trim();
                String value;
                if (wanted.equalsIgnoreCase ("all")) value = "All";
                else if (wanted.equalsIgnoreCase ("none")) value = "None";
                else
                {
                    for (auto& n : engine.getMidiInputNames())
                        if (n.equalsIgnoreCase (wanted) || n.containsIgnoreCase (wanted))
                            value = n;
                    if (value.isEmpty())
                    {
                        error = "no MIDI input called " + quoted (wanted) + " (connected: " + (engine.getMidiInputNames().isEmpty() ? String ("none") : engine.getMidiInputNames().joinIntoString (", ")) + ").";
                        return {};
                    }
                }
                t->input.track = -1;
                t->midiInput = value;
                return quoted (t->name) + "'s MIDI input is now " + value;
            }

            if ((bool) c.getProperty ("none", false))
            {
                t->input = {};
                if (t->isMidi())
                    t->midiInput = "None";
                return quoted (t->name) + " has no input now";
            }

            error = "give one of from_track, interface_input, midi_input or none: true.";
            return {};
        }

        if (action == "set_output")
        {
            auto t = needTrack();
            if (t == nullptr) return {};
            if (t->isMaster()) { error = "the master always plays out of the interface."; return {}; }

            if (hasKey (c, "interface_output"))
            {
                const int ch = (int) c["interface_output"] - 1;
                const bool stereo = (bool) c.getProperty ("stereo", true);
                const int available = engine.getOutputChannelNames().size();
                if (ch < 0 || ch + (stereo ? 1 : 0) >= available)
                {
                    error = "the interface has " + String (available) + " output(s); interface_output " + String (ch + 1) + (stereo ? " (stereo)" : "") + " isn't one.";
                    return {};
                }
                t->output = { OutputRoute::Kind::hardware, -1, ch, stereo };
                return quoted (t->name) + " now plays out of interface output " + String (ch + 1) + (stereo ? "+" + String (ch + 2) : String());
            }

            const auto to = c.getProperty ("to", {}).toString().trim();
            if (to.isEmpty() || to.equalsIgnoreCase ("master"))
            {
                t->output = {};
                return quoted (t->name) + " now plays into the master";
            }
            auto bus = findTrack (to);
            if (bus == nullptr || ! bus->isBus())
            {
                error = quoted (to) + " isn't a return. A track plays into the master, a return, or an interface output (interface_output).";
                return {};
            }
            if (t->isBus())
            {
                error = "a return plays into the master or an interface output, not into another return.";
                return {};
            }
            t->output = { OutputRoute::Kind::bus, bus->id, 0, true };
            return quoted (t->name) + " now plays through the return " + quoted (bus->name);
        }

        if (action == "set_send")
        {
            auto t = needTrack();
            if (t == nullptr) return {};
            if (! t->holdsClips()) { error = "only audio and MIDI tracks have sends."; return {}; }
            auto bus = needTrack ("return");
            if (bus == nullptr) return {};
            if (! bus->isBus()) { error = quoted (bus->name) + " isn't a return."; return {}; }

            auto send = t->getSend (bus->id, false);
            if (send == nullptr && ! hasKey (c, "level_db"))
            {
                error = "give \"level_db\" for the new send (-12 is a usual reverb send; \"off\" turns it off).";
                return {};
            }
            if (send == nullptr)
                send = t->getSend (bus->id, true);
            else
            {
                batch.remember (send->levelDb);
                batch.remember (send->preFader);
            }

            const auto level = c.getProperty ("level_db", {});
            const bool off = level.toString().equalsIgnoreCase ("off") || level.toString().equalsIgnoreCase ("-inf");
            if (! level.isVoid())
                send->levelDb = off ? kSilenceDb : jlimit (kSilenceDb, 6.0f, (float) (double) level);
            if (hasKey (c, "pre_fader"))
                send->preFader = (bool) c["pre_fader"];
            return off ? "Turned off " + quoted (t->name) + "'s send to " + quoted (bus->name)
                       : quoted (t->name) + " sends to " + quoted (bus->name) + " at " + dbText (send->levelDb.load())
                           + (send->preFader.load() ? " (pre-fader)" : String());
        }

        if (action == "set_mixer")
        {
            auto t = needTrack();
            if (t == nullptr) return {};
            StringArray what;
            if (hasKey (c, "volume_db"))
            {
                batch.remember (t->volumeDb);
                t->volumeDb = jlimit (kSilenceDb, 6.0f, (float) (double) c["volume_db"]);
                what.add ("volume " + dbText (t->volumeDb.load()));
            }
            if (hasKey (c, "pan"))
            {
                batch.remember (t->pan);
                t->pan = jlimit (-1.0f, 1.0f, (float) (double) c["pan"]);
                what.add ("pan " + String (t->pan.load(), 2));
            }
            if (hasKey (c, "mute"))
            {
                batch.remember (t->mute);
                t->mute = (bool) c["mute"];
                what.add ((bool) c["mute"] ? "muted" : "unmuted");
            }
            if (hasKey (c, "solo"))
            {
                if (t->isMaster()) { error = "the master can't be soloed."; return {}; }
                batch.remember (t->solo);
                t->solo = (bool) c["solo"];
                what.add ((bool) c["solo"] ? "soloed" : "unsoloed");
            }
            if (what.isEmpty()) { error = "give volume_db, pan, mute or solo."; return {}; }
            return quoted (t->name) + ": " + what.joinIntoString (", ");
        }

        if (action == "set_track")
        {
            auto t = needTrack();
            if (t == nullptr) return {};
            StringArray what;
            if (hasKey (c, "monitor"))
            {
                if (! t->holdsClips()) { error = "only audio and MIDI tracks have monitoring."; return {}; }
                const auto m = c["monitor"].toString().trim().toLowerCase();
                if (m != "auto" && m != "in" && m != "off") { error = "monitor is \"auto\", \"in\" or \"off\"."; return {}; }
                batch.remember (t->monitor);
                t->monitor = (int) (m == "in" ? Monitor::in : m == "off" ? Monitor::off : Monitor::automatic);
                what.add ("monitoring " + m);
            }
            if (hasKey (c, "arm"))
            {
                if (! t->holdsClips()) { error = "only audio and MIDI tracks can be armed."; return {}; }
                batch.remember (t->arm);
                t->arm = (bool) c["arm"];
                what.add ((bool) c["arm"] ? "armed" : "disarmed");
            }
            if (hasKey (c, "midi_channel"))
            {
                if (! t->isMidi()) { error = "only MIDI tracks have a MIDI channel."; return {}; }
                const int ch = (int) c["midi_channel"];
                if (ch < 1 || ch > 16) { error = "midi_channel is 1 to 16."; return {}; }
                t->midiChannel = ch;
                what.add ("MIDI channel " + String (ch));
            }
            if (what.isEmpty()) { error = "give monitor, arm or midi_channel."; return {}; }
            return quoted (t->name) + ": " + what.joinIntoString (", ");
        }

        if (action == "set_tempo")
        {
            const double bpm = (double) c.getProperty ("bpm", 0.0);
            if (bpm < 20.0 || bpm > 400.0) { error = "bpm is 20 to 400."; return {}; }
            edit.setTempo (bpm);
            return "Tempo " + String (edit.tempo, 2).trimCharactersAtEnd ("0").trimCharactersAtEnd (".") + " BPM";
        }

        if (action == "set_time_signature")
        {
            const int num = (int) c.getProperty ("numerator", 0), den = (int) c.getProperty ("denominator", 0);
            if (num < 1 || num > 32 || (den != 2 && den != 4 && den != 8 && den != 16))
            {
                error = "numerator is 1 to 32 and denominator 2, 4, 8 or 16.";
                return {};
            }
            edit.timeSigNum = num;
            edit.timeSigDen = den;
            return "Time signature " + String (num) + "/" + String (den);
        }

        error = action.isEmpty() ? "each change needs an \"action\"." : "unknown action " + quoted (action) + ".";
        return {};
    };

    for (int i = 0; i < changes->size(); ++i)
    {
        String error;
        const auto line = applyOne (changes->getReference (i), error);
        if (error.isNotEmpty())
        {
            for (auto it = batch.revert.rbegin(); it != batch.revert.rend(); ++it)
                (*it)();
            edit.discardUncommitted();
            const auto action = changes->getReference (i).getProperty ("action", "?").toString();
            return toolText ("Change " + String (i + 1) + " (" + action + "): " + error
                             + "\nNothing was changed. Fix that one and send the whole list again.", true);
        }
        done.add (line);
    }

    edit.changed();
    edit.commitUndoStep();

    status = "Assistant: " + (done.size() == 1 ? done[0] : String (done.size()) + " changes to the session")
             + ". Cmd/Ctrl+Z undoes it.";

    String text = "Done. In Lathe this is one undo step (Cmd/Ctrl+Z); volume, pan, mute, solo, send levels, "
                  "monitoring and arm change live and stay as they are on undo.\n";
    for (auto& d : done)
        text << "- " << d << "\n";
    const auto problems = findProblems();
    if (! problems.isEmpty())
    {
        text << "\nWorth checking:\n";
        for (auto& p : problems)
            text << "- " << p << "\n";
    }
    return toolText (text.trimEnd());
}


//==============================================================================
// Measuring: plays part of the project for real and times every audio block and track.

namespace
{
/** What was going on with the computer, so two measurements can be told apart. */
var machineConditions (Engine& engine)
{
    var o = obj();
    const auto model = SystemStats::getDeviceDescription();
    set (o, "computer", model.isNotEmpty() ? model : String ("unknown model"));
    set (o, "cpu", SystemStats::getCpuModel().isNotEmpty() ? SystemStats::getCpuModel() : SystemStats::getCpuVendor());
    set (o, "physical_cores", SystemStats::getNumPhysicalCpus());
    set (o, "auto_processing_threads", Engine::autoProcessingThreads());
    set (o, "os", SystemStats::getOperatingSystemName());

   #if JUCE_MAC
    int perf = 0, eff = 0;
    size_t size = sizeof (perf);
    if (sysctlbyname ("hw.perflevel0.physicalcpu", &perf, &size, nullptr, 0) == 0 && perf > 0)
        set (o, "performance_cores", perf);
    size = sizeof (eff);
    if (sysctlbyname ("hw.perflevel1.physicalcpu", &eff, &size, nullptr, 0) == 0 && eff > 0)
        set (o, "efficiency_cores", eff);
   #endif

    var audio = obj();
    auto* d = engine.deviceManager.getCurrentAudioDevice();
    set (audio, "device", d != nullptr ? d->getName() : String ("none"));
    set (audio, "sample_rate", engine.getSampleRate());
    set (audio, "buffer_samples", engine.getBlockSize());
    set (audio, "deadline_ms", engine.getSampleRate() > 0 ? rounded (engine.getBlockSize() * 1000.0 / engine.getSampleRate(), 0.001) : 0.0);
    set (o, "audio", audio);
    return o;
}

struct Stats
{
    int blocks = 0;
    double meanPct = 0.0, p99Pct = 0.0, peakPct = 0.0;
    int over = 0;   // blocks that missed their deadline

    static Stats of (std::vector<float> us, double deadlineUs)
    {
        Stats s;
        s.blocks = (int) us.size();
        if (us.empty() || deadlineUs <= 0.0)
            return s;
        double sum = 0.0;
        for (auto v : us)
        {
            sum += v;
            if (v > deadlineUs)
                ++s.over;
        }
        std::sort (us.begin(), us.end());
        s.meanPct = sum / (double) us.size() / deadlineUs * 100.0;
        s.p99Pct = us[(size_t) jmin ((int) us.size() - 1, (int) std::ceil (0.99 * (double) us.size()) - 1)] / deadlineUs * 100.0;
        s.peakPct = us.back() / deadlineUs * 100.0;
        return s;
    }

    var toVar() const
    {
        var o = obj();
        set (o, "mean_load_pct", rounded (meanPct, 0.1));
        set (o, "p99_load_pct", rounded (p99Pct, 0.1));
        set (o, "peak_load_pct", rounded (peakPct, 0.1));
        set (o, "blocks_measured", blocks);
        set (o, "blocks_over_deadline", over);
        return o;
    }
};
} // namespace

class Tools::Measurer : private Timer
{
public:
    struct Run
    {
        int threads = -1;   // -1: leave the setting alone; 0: auto; n: that many
        std::vector<float> blockUs;
        std::map<int, double> trackUs;   // by track id, summed over trackBlocks
        int64 trackBlocks = 0;
        int threadsUsed = 1;
        int xruns = 0;
        bool xrunsKnown = true;
        double seconds = 0.0;
    };

    Measurer (Tools& t, std::shared_ptr<Job> j, bool comparing, std::vector<Run> plan, double fromBeats, double toBeats, double targetSeconds)
        : tools (t), job (std::move (j)), compare (comparing), runs (std::move (plan)),
          startBeats (fromBeats), endBeats (toBeats), target (targetSeconds)
    {
        originalThreads = tools.engine.getProcessingThreads();
        if (tools.engine.isPlaying())
            tools.engine.stop();
        startTimer (20);
    }

    ~Measurer() override
    {
        stopTimer();
        if (tools.engine.isMeasuring())
            tools.engine.endMeasurement();
        if (state != State::finished)
        {
            if (tools.engine.isPlaying())
                tools.engine.stop();
            tools.engine.setProcessingThreads (originalThreads);
            finish (toolText ("Lathe is closing (or the measurement was cancelled).", true));
        }
    }

    /** Done and reported: what's left is only letting go of it. */
    bool isFinished() const noexcept   { return state == State::finished; }

private:
    enum class State { settling, playing, finished };

    void timerCallback() override
    {
        auto& engine = tools.engine;
        const double now = Time::getMillisecondCounterHiRes();

        if (engine.isRecording() || engine.isCountingIn())
        {
            if (engine.isMeasuring())
                engine.endMeasurement();
            engine.setProcessingThreads (originalThreads);
            finish (toolText ("Stopped measuring: recording started.", true));
            return;
        }

        if (state == State::settling)
        {
            if (now < settleUntil)
                return;
            startPass();
            return;
        }

        if (state == State::playing)
        {
            auto& run = runs[runIndex];
            const double played = (now - passStarted) / 1000.0;
            const bool reachedTarget = run.seconds + played >= target + warmupSeconds (passIndex == 0);
            if (engine.isPlaying() && ! reachedTarget)
                return;

            if (engine.isPlaying())
                engine.stop();
            collectPass (played);

            ++passIndex;
            if (run.seconds < target && passIndex < 10 && ! reachedTarget)
            {
                state = State::settling;
                settleUntil = now + 60.0;
                return;
            }

            ++runIndex;
            passIndex = 0;
            if (runIndex < runs.size())
            {
                beginRun();
                return;
            }

            engine.setProcessingThreads (originalThreads);
            finish (toolText (JSON::toString (report(), true, 4)));
        }
    }

public:
    void beginRun()
    {
        auto& run = runs[runIndex];
        if (run.threads >= 0)
            tools.engine.setProcessingThreads (run.threads);
        if (tools.showStatus)
            tools.showStatus ("Assistant: measuring CPU" + (compare ? " (" + modeName (run.threads) + ", run " + String ((int) runIndex + 1) + " of "
                                                                      + String ((int) runs.size()) + ")" : String())
                              + ", bars " + tools.barText (startBeats) + " to " + tools.barText (endBeats) + "...");
        if (auto* d = tools.engine.deviceManager.getCurrentAudioDevice())
        {
            xrunsBefore = d->getXRunCount();
            runs[runIndex].xrunsKnown = xrunsBefore >= 0;
        }
        state = State::settling;
        settleUntil = Time::getMillisecondCounterHiRes() + 300.0;   // the new thread pool settles in
    }

private:
    static String modeName (int threads)
    {
        return threads == 1 ? "1 core" : threads == 0 ? "all cores (Auto, " + String (Engine::autoProcessingThreads()) + ")" : String (threads) + " cores";
    }

    double warmupSeconds (bool firstPass) const   { return firstPass ? 0.75 : 0.05; }

    void startPass()
    {
        auto& engine = tools.engine;
        auto& edit = tools.edit;
        const double rate = jmax (1.0, engine.getSampleRate());
        const int block = jmax (1, engine.getBlockSize());

        // Play from startBeats without moving the insert marker (Stop goes back to it, as usual).
        const double marker = edit.insertMarker;
        edit.insertMarker = startBeats;
        engine.play();
        edit.insertMarker = marker;
        engine.setAutoStop (endBeats, 0);

        engine.beginMeasurement ((int) std::ceil (warmupSeconds (passIndex == 0) * rate / block));
        passStarted = Time::getMillisecondCounterHiRes();
        state = State::playing;
    }

    void collectPass (double played)
    {
        auto& run = runs[runIndex];
        const auto m = tools.engine.endMeasurement();
        run.blockUs.insert (run.blockUs.end(), m.blockUs.begin(), m.blockUs.end());
        run.trackBlocks += m.trackBlocks;
        run.threadsUsed = m.threads;
        run.seconds += jmax (0.0, played - warmupSeconds (passIndex == 0));
        for (auto* list : { &tools.edit.tracks, &tools.edit.buses })
            for (auto& t : *list)
                run.trackUs[t->id] += t->measuredUs.load();

        if (auto* d = tools.engine.deviceManager.getCurrentAudioDevice(); d != nullptr && xrunsBefore >= 0)
        {
            const int after = d->getXRunCount();
            if (after >= 0)
                run.xruns = after - xrunsBefore;
        }
        blockSize = m.blockSize;
        sampleRate = m.sampleRate;
    }

    var runToVar (const Run& run) const
    {
        const double deadline = blockSize * 1.0e6 / jmax (1.0, sampleRate);
        var o = Stats::of (run.blockUs, deadline).toVar();
        set (o, "processing_threads", run.threadsUsed);
        set (o, "seconds_measured", rounded (run.seconds, 0.1));
        set (o, "dropouts_reported_by_device", run.xrunsKnown ? var (run.xruns) : var ("not reported by this device"));
        return o;
    }

    var trackCosts (const Run& run) const
    {
        const double deadline = blockSize * 1.0e6 / jmax (1.0, sampleRate);
        std::vector<std::pair<double, std::shared_ptr<Track>>> costs;
        for (auto* list : { &tools.edit.tracks, &tools.edit.buses })
            for (auto& t : *list)
                if (auto it = run.trackUs.find (t->id); it != run.trackUs.end() && run.trackBlocks > 0)
                    costs.push_back ({ it->second / (double) run.trackBlocks, t });
        std::sort (costs.begin(), costs.end(), [] (auto& a, auto& b) { return a.first > b.first; });

        Array<var> list;
        for (auto& [us, t] : costs)
        {
            if (list.size() >= 24)
                break;
            var o = obj();
            set (o, "track", t->name);
            set (o, "kind", Edit::trackKindToString (t->kind));
            set (o, "us_per_block", rounded (us, 0.1));
            set (o, "pct_of_deadline", rounded (us / deadline * 100.0, 0.1));
            StringArray plugins;
            for (auto& p : t->plugins)
                if (! p->bypassed.load())
                    plugins.add (p->getName());
            if (! plugins.isEmpty())
                set (o, "plugins", plugins.joinIntoString (" -> "));
            list.add (o);
        }
        return list;
    }

    var report() const
    {
        var o = obj();
        set (o, "bars", "from bar " + tools.barText (startBeats) + " to bar " + tools.barText (endBeats));
        const double deadline = blockSize * 1.0e6 / jmax (1.0, sampleRate);

        if (! compare)
        {
            const auto& run = runs.front();
            set (o, "result", runToVar (run));
            set (o, "per_track", trackCosts (run));
            set (o, "per_track_note", "Each track's own time per block (its plug-ins and clips), as a share of the deadline. "
                                      "With several processing threads, tracks run side by side, so these add up to more than the total.");
        }
        else
        {
            std::map<int, std::vector<float>> pooled;
            std::map<int, int> threadsUsed;
            Array<var> list;
            for (size_t i = 0; i < runs.size(); ++i)
            {
                auto r = runToVar (runs[i]);
                set (r, "run", (int) i + 1);
                set (r, "mode", modeName (runs[i].threads));
                list.add (r);
                auto& p = pooled[runs[i].threads];
                p.insert (p.end(), runs[i].blockUs.begin(), runs[i].blockUs.end());
                threadsUsed[runs[i].threads] = runs[i].threadsUsed;
            }

            const auto one = Stats::of (pooled[1], deadline), all = Stats::of (pooled[0], deadline);
            var summary = obj();
            auto side = [&] (const Stats& s, int threads)
            {
                auto v = s.toVar();
                set (v, "processing_threads", threadsUsed[threads]);
                return v;
            };
            set (summary, "one_core", side (one, 1));
            set (summary, "all_cores", side (all, 0));
            if (all.meanPct > 0.0)
                set (summary, "speed_up", rounded (one.meanPct / all.meanPct, 0.01));
            set (o, "summary", summary);
            set (o, "runs", list);
            set (o, "per_track_one_core", trackCosts (runs.front()));
            set (o, "how", "The same bars played on 1 core and on all cores in turn (1, all, 1, all...), each after a short "
                           "warm-up, so a warming or throttling computer doesn't favour one side. Your Processing cores "
                           "setting is back as it was.");
        }

        set (o, "conditions", machineConditions (tools.engine));
        set (o, "to_compare_elsewhere",
             "Use the same buffer size (" + String (blockSize) + " samples) and sample rate (" + String (sampleRate, 0)
             + " Hz), the same plug-ins with the same settings, play the same bars, and read the other app's CPU meter while "
             "it plays. Lathe's figures are the time each audio block took as a share of the time it lasts, which is what "
             "most CPU meters show.");
        return o;
    }

    void finish (const var& result)
    {
        if (state == State::finished)
            return;
        state = State::finished;
        stopTimer();
        job->result = result;
        job->done.signal();
        if (! tools.alive->load())
            return;   // Lathe is closing
        if (tools.showStatus)
            tools.showStatus ((bool) result["isError"] ? "Assistant: measurement stopped." : "Assistant: finished measuring CPU.");
        tools.measurerFinished (this);
    }

    Tools& tools;
    std::shared_ptr<Job> job;
    const bool compare;
    std::vector<Run> runs;
    size_t runIndex = 0;
    int passIndex = 0;
    const double startBeats, endBeats, target;
    State state = State::settling;
    double settleUntil = 0.0, passStarted = 0.0;
    int originalThreads = 0, xrunsBefore = -1, blockSize = 0;
    double sampleRate = 0.0;
};

Tools::~Tools()
{
    alive->store (false);
    measurer.reset();
}

bool Tools::isLongTool (const String& name)
{
    return assistant::isLongTool (name);
}

std::shared_ptr<Tools::Job> Tools::startLongTool (const String& name, const var& args, var& immediate)
{
    if (! isLongTool (name))
    {
        immediate = toolText ("Unknown tool: " + name, true);
        return {};
    }
    if (measurer != nullptr && ! measurer->isFinished())
    {
        immediate = toolText ("Lathe is already measuring. Wait for that to finish.", true);
        return {};
    }
    measurer.reset();   // a finished one whose clean-up hasn't come round yet
    if (engine.getSampleRate() <= 0.0)
    {
        immediate = toolText ("No audio device is running, so there's nothing to measure (Options > Audio / MIDI Settings).", true);
        return {};
    }
    if (engine.isRecording() || engine.isCountingIn())
    {
        immediate = toolText ("Lathe is recording. Stop first, then measure.", true);
        return {};
    }

    // Which bars: as given, else the loop if it's on, else the first four bars (or the song, if shorter).
    double from = 0.0, to = 0.0;
    if (args.hasProperty ("from_bar") || args.hasProperty ("to_bar"))
    {
        from = toBeats ((double) args.getProperty ("from_bar", 1.0));
        to = args.hasProperty ("to_bar") ? toBeats ((double) args["to_bar"]) : from + 4.0 * edit.beatsPerBar();
    }
    else if (edit.loopEnabled && edit.loopEnd > edit.loopStart)
    {
        from = edit.loopStart;
        to = edit.loopEnd;
    }
    else
    {
        const double end = edit.getEndBeat();
        from = 0.0;
        to = end >= edit.beatsPerBar() ? jmin (end, 4.0 * edit.beatsPerBar()) : 4.0 * edit.beatsPerBar();
    }
    if (from < 0.0 || to <= from + 1.0e-6)
    {
        immediate = toolText ("to_bar must come after from_bar (bars are 1-based).", true);
        return {};
    }

    const bool comparing = name == "compare_processing_cores";
    const double seconds = jlimit (2.0, 20.0, (double) args.getProperty ("seconds", comparing ? 6.0 : 8.0));
    std::vector<Measurer::Run> plan;
    if (comparing)
    {
        const int rounds = jlimit (1, 3, (int) args.getProperty ("rounds", 2));
        for (int i = 0; i < rounds; ++i)
        {
            plan.push_back ({}); plan.back().threads = 1;
            plan.push_back ({}); plan.back().threads = 0;
        }
    }
    else
    {
        plan.push_back ({});   // as the setting is now
    }

    auto job = std::make_shared<Job>();
    measurer = std::make_unique<Measurer> (*this, job, comparing, std::move (plan), from, to, seconds);
    measurer->beginRun();
    return job;
}

void Tools::measurerFinished (const Measurer* m)
{
    // Called from inside the measurer: let go of it once its call has returned.
    auto flag = alive;
    MessageManager::callAsync ([this, flag, m]
    {
        // A new measurement may have taken its place (even at the same address): only a finished one goes.
        if (flag->load() && measurer != nullptr && measurer.get() == m && measurer->isFinished())
            measurer.reset();
    });
}

} // namespace lathe::assistant
