#include "Edit.h"

namespace lathe
{
const Array<Colour>& getTrackPalette()
{
    static const Array<Colour> palette {
        Colour (0xffc9805c), Colour (0xffd2aa5e), Colour (0xffa4b968), Colour (0xff66b08f),
        Colour (0xff5f9fc6), Colour (0xff8c80c6), Colour (0xffc479a1), Colour (0xffa59a8c)
    };
    return palette;
}

//==============================================================================
std::shared_ptr<Send> Track::getSend (int busId, bool create)
{
    for (auto& s : sends)
        if (s->busId == busId)
            return s;

    if (! create)
        return {};

    auto s = std::make_shared<Send>();
    s->busId = busId;
    sends.push_back (s);
    return s;
}

Clip* Track::findClip (int clipId)
{
    for (auto& c : clips)
        if (c.id == clipId)
            return &c;
    return nullptr;
}

int Track::indexOfClip (int clipId) const
{
    for (size_t i = 0; i < clips.size(); ++i)
        if (clips[i].id == clipId)
            return (int) i;
    return -1;
}

//==============================================================================
Edit::Edit()
{
    reset();
}

void Edit::reset()
{
    tempo = 120.0;
    timeSigNum = timeSigDen = 4;
    loopEnabled = false;
    loopStart = 0.0;
    loopEnd = 16.0;
    insertMarker = 0.0;
    tracks.clear();
    buses.clear();
    groups.clear();
    cues.clear();
    clickInMain = true;
    projectFile = File();
    workDir = File();
    undoStack.clear();
    redoStack.clear();
    lastId = 0;
    colourIndex = 0;
    dirty = false;
    needsRebuild = true;

    master = std::make_shared<Track>();
    master->id = newId();
    master->kind = TrackKind::master;
    master->name = "Master";
    master->colour = Colour (0xff9a9a9a);
    master->output.kind = OutputRoute::Kind::hardware;
    master->output.channel = 0;
}

void Edit::ensureWorkDir()
{
    if (workDir == File())
        workDir = getUnsavedDir().getChildFile ("Untitled " + Time::getCurrentTime().formatted ("%Y-%m-%d %H.%M.%S"));

    getAudioDir().createDirectory();
}

Colour Edit::nextColour()
{
    auto& p = getTrackPalette();
    return p[colourIndex++ % p.size()];
}

std::shared_ptr<Track> Edit::addTrack (TrackKind kind, const String& name)
{
    auto t = std::make_shared<Track>();
    t->id = newId();
    t->kind = kind;
    t->colour = nextColour();

    if (kind == TrackKind::bus)
    {
        const auto letter = String::charToString ((juce_wchar) ('A' + (int) (buses.size() % 26)));
        t->name = name.isNotEmpty() ? name : letter + " Return";
        buses.push_back (t);
    }
    else
    {
        int count = 1;
        for (auto& other : tracks)
            if (other->kind == kind)
                ++count;

        t->name = name.isNotEmpty() ? name : (kind == TrackKind::midi ? "MIDI " : "Audio ") + String (count);

        if (kind == TrackKind::audio)
        {
            // Default to the next unused mono input, so adding tracks on a
            // multi-input interface just walks across the inputs.
            int used = -1;
            for (auto& other : tracks)
                if (other->isAudio() && other->input.channel >= 0)
                    used = jmax (used, other->input.channel + (other->input.stereo ? 1 : 0));
            t->input.channel = used + 1;
        }

        tracks.push_back (t);
    }

    changed();
    return t;
}

void Edit::removeTrack (int trackId)
{
    auto removeFrom = [trackId] (std::vector<std::shared_ptr<Track>>& list)
    {
        list.erase (std::remove_if (list.begin(), list.end(), [trackId] (auto& t) { return t->id == trackId; }), list.end());
    };

    removeFrom (tracks);
    removeFrom (buses);
    removeEmptyGroups();

    // Anything routed to a deleted bus falls back to the master; its sends go away.
    auto fixRouting = [trackId] (Track& t)
    {
        if (t.output.kind == OutputRoute::Kind::bus && t.output.busId == trackId)
            t.output = {};
        t.sends.erase (std::remove_if (t.sends.begin(), t.sends.end(), [trackId] (auto& s) { return s->busId == trackId; }), t.sends.end());
    };

    for (auto& t : tracks) fixRouting (*t);
    for (auto& t : buses)  fixRouting (*t);
    for (auto& g : groups)
        if (g->busId == trackId)
            g->busId = -1;

    changed();
}

void Edit::moveTrack (int trackId, int newIndex)
{
    for (auto* list : { &tracks, &buses })
    {
        for (size_t i = 0; i < list->size(); ++i)
        {
            if ((*list)[i]->id == trackId)
            {
                auto t = (*list)[i];
                list->erase (list->begin() + (long) i);
                newIndex = jlimit (0, (int) list->size(), newIndex);
                list->insert (list->begin() + newIndex, t);
                changed();
                return;
            }
        }
    }
}

//==============================================================================
std::shared_ptr<TrackGroup> Edit::findGroup (int groupId) const
{
    for (auto& g : groups)
        if (g->id == groupId)
            return g;
    return {};
}

std::vector<std::shared_ptr<Track>> Edit::groupMembers (int groupId) const
{
    std::vector<std::shared_ptr<Track>> members;
    for (auto& t : tracks)
        if (t->groupId == groupId)
            members.push_back (t);
    return members;
}

void Edit::removeEmptyGroups()
{
    groups.erase (std::remove_if (groups.begin(), groups.end(), [this] (auto& g) { return groupMembers (g->id).empty(); }), groups.end());
}

std::shared_ptr<TrackGroup> Edit::createGroup (const std::vector<int>& trackIds, const String& name)
{
    auto g = std::make_shared<TrackGroup>();
    g->id = newId();
    g->name = name.isNotEmpty() ? name : "Group " + String ((int) groups.size() + 1);
    g->colour = nextColour();

    // Members sit together, where the first of them was.
    std::vector<std::shared_ptr<Track>> members;
    int firstIndex = -1;
    for (size_t i = 0; i < tracks.size(); ++i)
    {
        if (std::find (trackIds.begin(), trackIds.end(), tracks[i]->id) != trackIds.end() && tracks[i]->holdsClips())
        {
            if (firstIndex < 0)
                firstIndex = (int) i;
            members.push_back (tracks[i]);
        }
    }
    if (members.empty())
        return {};

    tracks.erase (std::remove_if (tracks.begin(), tracks.end(), [&] (auto& t)
    {
        return std::find (members.begin(), members.end(), t) != members.end();
    }), tracks.end());
    tracks.insert (tracks.begin() + jmin (firstIndex, (int) tracks.size()), members.begin(), members.end());

    for (auto& t : members)
        t->groupId = g->id;

    groups.push_back (g);
    removeEmptyGroups();
    changed();
    return g;
}

void Edit::setTrackGroup (int trackId, int groupId)
{
    auto t = findTrack (trackId);
    if (t == nullptr || ! t->holdsClips())
        return;

    // Leaving a group with a bus: stop playing through it.
    if (auto oldBus = findGroupBus (t->groupId); oldBus != nullptr && t->groupId != groupId
        && t->output.kind == OutputRoute::Kind::bus && t->output.busId == oldBus->id)
        t->output = {};
    // Joining one: play through its bus (unless it was sent somewhere on purpose).
    if (auto newBus = findGroupBus (groupId); newBus != nullptr && t->output.kind == OutputRoute::Kind::master)
    {
        t->output.kind = OutputRoute::Kind::bus;
        t->output.busId = newBus->id;
    }

    if (groupId >= 0 && findGroup (groupId) != nullptr)
    {
        // Move it next to the other members.
        auto members = groupMembers (groupId);
        t->groupId = groupId;
        if (! members.empty() && members.back() != t)
        {
            tracks.erase (std::find (tracks.begin(), tracks.end(), t));
            auto after = std::find (tracks.begin(), tracks.end(), members.back());
            tracks.insert (after + 1, t);
        }
    }
    else
    {
        t->groupId = -1;
    }

    removeEmptyGroups();
    changed();
}

std::shared_ptr<Track> Edit::findGroupBus (int groupId) const
{
    if (auto g = findGroup (groupId); g != nullptr && g->busId >= 0)
        for (auto& b : buses)
            if (b->id == g->busId)
                return b;
    return {};
}

std::shared_ptr<Track> Edit::makeGroupBus (int groupId)
{
    auto g = findGroup (groupId);
    if (g == nullptr)
        return {};
    if (auto existing = findGroupBus (groupId))
        return existing;

    auto bus = addTrack (TrackKind::bus, g->name + " Bus");
    bus->colour = g->colour;
    g->busId = bus->id;
    for (auto& t : groupMembers (groupId))
    {
        if (t->output.kind == OutputRoute::Kind::master)
        {
            t->output.kind = OutputRoute::Kind::bus;
            t->output.busId = bus->id;
        }
    }
    changed();
    return bus;
}

void Edit::renameGroup (int groupId, const String& newName)
{
    auto g = findGroup (groupId);
    const auto name = newName.trim();
    if (g == nullptr || name.isEmpty() || name == g->name)
        return;
    if (auto bus = findGroupBus (groupId); bus != nullptr && bus->name == g->name + " Bus")
        bus->name = name + " Bus";
    g->name = name;
    changed();
}

void Edit::removeGroupBus (int groupId)
{
    auto g = findGroup (groupId);
    auto bus = findGroupBus (groupId);
    if (g == nullptr || bus == nullptr)
        return;
    g->busId = -1;
    removeTrack (bus->id);   // members routed to it fall back to the master
}

void Edit::ungroup (int groupId)
{
    for (auto& t : tracks)
        if (t->groupId == groupId)
            t->groupId = -1;
    removeEmptyGroups();
    changed();
}

bool Edit::isGroupArmed (int groupId) const
{
    auto members = groupMembers (groupId);
    if (members.empty())
        return false;
    for (auto& t : members)
        if (! t->arm.load())
            return false;
    return true;
}

void Edit::toggleGroupArm (int groupId)
{
    const bool arm = ! isGroupArmed (groupId);
    for (auto& t : groupMembers (groupId))
        t->arm = arm;
    touched();
}

bool Edit::isEffectivelyMuted (const Track& t) const
{
    if (t.mute.load())
        return true;
    auto g = findGroup (t.groupId);
    return g != nullptr && g->mute.load();
}

bool Edit::isEffectivelySoloed (const Track& t) const
{
    if (t.solo.load())
        return true;
    auto g = findGroup (t.groupId);
    return g != nullptr && g->solo.load();
}

//==============================================================================
std::shared_ptr<CueMix> Edit::addCue (const String& name, int firstOutputChannel)
{
    auto c = std::make_shared<CueMix>();
    c->id = newId();
    c->name = name.isNotEmpty() ? name : "Cue " + String ((int) cues.size() + 1);
    static const Colour cueColours[] = { Colour (0xff5fb3a8), Colour (0xffb28ad6), Colour (0xffd98c6a), Colour (0xff8cb85f) };
    c->colour = cueColours[cues.size() % 4];
    c->channel = jmax (0, firstOutputChannel);
    copyMainMixToCue (*c);
    cues.push_back (c);
    changed();
    return c;
}

void Edit::removeCue (int cueId)
{
    cues.erase (std::remove_if (cues.begin(), cues.end(), [cueId] (auto& c) { return c->id == cueId; }), cues.end());
    changed();
}

std::shared_ptr<CueMix> Edit::findCue (int cueId) const
{
    for (auto& c : cues)
        if (c->id == cueId)
            return c;
    return {};
}

std::shared_ptr<CueLevel> Edit::cueLevel (CueMix& cue, const Track& t)
{
    auto& slot = cue.levels[t.id];
    if (slot == nullptr)
    {
        slot = std::make_shared<CueLevel>();
        slot->levelDb = t.volumeDb.load();
        slot->pan = t.pan.load();
        slot->mute = t.mute.load();
    }
    return slot;
}

void Edit::copyMainMixToCue (CueMix& cue)
{
    for (auto* list : { &tracks, &buses })
        for (auto& t : *list)
        {
            auto l = cueLevel (cue, *t);
            l->levelDb = t->volumeDb.load();
            l->pan = t->pan.load();
            l->mute = t->mute.load();
        }
}

std::shared_ptr<Track> Edit::findTrack (int trackId) const
{
    if (master != nullptr && master->id == trackId)
        return master;

    for (auto& t : tracks) if (t->id == trackId) return t;
    for (auto& t : buses)  if (t->id == trackId) return t;
    return {};
}

double Edit::getEndBeat() const
{
    double end = 0.0;
    for (auto& t : tracks)
        for (auto& c : t->clips)
            end = jmax (end, c.end());
    return end;
}

void Edit::setTempo (double newTempo)
{
    newTempo = jlimit (20.0, 400.0, newTempo);
    if (newTempo == tempo)
        return;

    // Audio isn't time-stretched: keep each audio clip covering the same audio.
    const double ratio = newTempo / tempo;
    for (auto& t : tracks)
        for (auto& c : t->clips)
            if (c.kind == ClipKind::audio)
                c.length *= ratio;

    tempo = newTempo;
    changed();
}

//==============================================================================
static void truncateMidi (Clip& c)
{
    if (c.kind != ClipKind::midi)
        return;

    c.notes.erase (std::remove_if (c.notes.begin(), c.notes.end(), [&] (const Note& n) { return n.start >= c.length; }), c.notes.end());
    for (auto& n : c.notes)
        n.length = jmin (n.length, c.length - n.start);

    c.events.erase (std::remove_if (c.events.begin(), c.events.end(), [&] (const MidiEvent& e) { return e.beat >= c.length; }), c.events.end());
}

void Edit::trimClipStart (Clip& c, double newStart)
{
    double delta = newStart - c.start;

    if (c.kind == ClipKind::audio)
    {
        const double spb = samplesPerBeat (sampleRate);
        // Can't reveal audio from before the start of the file.
        delta = jmax (delta, -(double) c.sourceOffset / spb);
        c.sourceOffset = jmax ((int64) 0, c.sourceOffset + (int64) std::llround (delta * spb));
    }
    else
    {
        for (auto& n : c.notes)
            n.start -= delta;

        c.notes.erase (std::remove_if (c.notes.begin(), c.notes.end(), [] (const Note& n) { return n.start + n.length <= 0.0; }), c.notes.end());
        for (auto& n : c.notes)
        {
            if (n.start < 0.0)
            {
                n.length += n.start;
                n.start = 0.0;
            }
        }

        for (auto& e : c.events)
            e.beat -= delta;
        c.events.erase (std::remove_if (c.events.begin(), c.events.end(), [] (const MidiEvent& e) { return e.beat < 0.0; }), c.events.end());
    }

    c.start += delta;
    c.length -= delta;
}

int Edit::splitClip (Track& t, int clipId, double atBeat)
{
    auto index = t.indexOfClip (clipId);
    if (index < 0)
        return -1;

    auto& c = t.clips[(size_t) index];
    if (atBeat <= c.start + 1.0e-6 || atBeat >= c.end() - 1.0e-6)
        return -1;

    Clip right = c;
    right.id = newId();
    trimClipStart (right, atBeat);

    c.length = atBeat - c.start;
    truncateMidi (c);

    t.clips.insert (t.clips.begin() + index + 1, right);
    return right.id;
}

void Edit::clearRange (Track& t, double from, double to, int exceptClipId)
{
    if (to <= from)
        return;

    std::vector<int> ids;
    for (auto& c : t.clips)
        if (c.id != exceptClipId && c.end() > from && c.start < to)
            ids.push_back (c.id);

    for (auto id : ids)
    {
        auto* c = t.findClip (id);
        if (c == nullptr)
            continue;

        if (c->start >= from && c->end() <= to)
        {
            t.clips.erase (t.clips.begin() + t.indexOfClip (id));
        }
        else if (c->start < from && c->end() > to)
        {
            splitClip (t, id, to);   // keeps the part after the range
            c = t.findClip (id);
            c->length = from - c->start;
            truncateMidi (*c);
        }
        else if (c->start < from)
        {
            c->length = from - c->start;
            truncateMidi (*c);
        }
        else
        {
            trimClipStart (*c, to);
        }
    }
}

//==============================================================================
void Edit::beginClipEdit()
{
    undoStack.push_back (clipsToVar());
    if (undoStack.size() > 200)
        undoStack.erase (undoStack.begin());
    redoStack.clear();
}

bool Edit::undo()
{
    if (undoStack.empty())
        return false;

    redoStack.push_back (clipsToVar());
    clipsFromVar (undoStack.back());
    undoStack.pop_back();
    changed();
    return true;
}

bool Edit::redo()
{
    if (redoStack.empty())
        return false;

    undoStack.push_back (clipsToVar());
    clipsFromVar (redoStack.back());
    redoStack.pop_back();
    changed();
    return true;
}

var Edit::clipsToVar() const
{
    Array<var> list;
    for (auto& t : tracks)
    {
        DynamicObject::Ptr o = new DynamicObject();
        o->setProperty ("track", t->id);

        Array<var> clips;
        for (auto& c : t->clips)
            clips.add (clipToVar (c));
        o->setProperty ("clips", clips);
        list.add (var (o.get()));
    }
    return list;
}

void Edit::clipsFromVar (const var& v)
{
    if (auto* arr = v.getArray())
    {
        for (auto& entry : *arr)
        {
            if (auto t = findTrack ((int) entry["track"]))
            {
                t->clips.clear();
                if (auto* clips = entry["clips"].getArray())
                    for (auto& c : *clips)
                        t->clips.push_back (clipFromVar (c, File()));
            }
        }
    }
}

//==============================================================================
String Edit::trackKindToString (TrackKind k)
{
    switch (k)
    {
        case TrackKind::audio:  return "audio";
        case TrackKind::midi:   return "midi";
        case TrackKind::bus:    return "return";
        case TrackKind::master: return "master";
    }
    return "audio";
}

TrackKind Edit::trackKindFromString (const String& s)
{
    if (s == "midi")   return TrackKind::midi;
    if (s == "return") return TrackKind::bus;
    if (s == "master") return TrackKind::master;
    return TrackKind::audio;
}

static String monitorToString (int m)
{
    return m == (int) Monitor::in ? "in" : m == (int) Monitor::off ? "off" : "auto";
}

static int monitorFromString (const String& s)
{
    return s == "in" ? (int) Monitor::in : s == "off" ? (int) Monitor::off : (int) Monitor::automatic;
}

var Edit::clipToVar (const Clip& c) const
{
    DynamicObject::Ptr o = new DynamicObject();
    o->setProperty ("id", c.id);
    o->setProperty ("kind", c.kind == ClipKind::audio ? "audio" : "midi");
    o->setProperty ("name", c.name);
    o->setProperty ("start", c.start);
    o->setProperty ("length", c.length);

    if (c.kind == ClipKind::audio)
    {
        // Paths inside the project folder are stored relative, so the folder can move.
        String path = c.file.getFullPathName();
        if (workDir != File() && c.file.isAChildOf (workDir))
            path = c.file.getRelativePathFrom (workDir).replaceCharacter ('\\', '/');

        o->setProperty ("file", path);
        o->setProperty ("offset", (int64) c.sourceOffset);
        o->setProperty ("gain", c.gainDb);
    }
    else
    {
        Array<var> notes;
        for (auto& n : c.notes)
            notes.add (Array<var> { n.pitch, n.start, n.length, n.velocity });
        o->setProperty ("notes", notes);

        Array<var> events;
        for (auto& e : c.events)
        {
            Array<var> ev { e.beat };
            for (int i = 0; i < e.size; ++i)
                ev.add ((int) e.data[i]);
            events.add (ev);
        }
        if (! events.isEmpty())
            o->setProperty ("events", events);
    }

    return var (o.get());
}

Clip Edit::clipFromVar (const var& v, const File& baseDir) const
{
    Clip c;
    c.id = (int) v["id"];
    c.kind = v["kind"].toString() == "midi" ? ClipKind::midi : ClipKind::audio;
    c.name = v["name"].toString();
    c.start = (double) v["start"];
    c.length = jmax (0.01, (double) v["length"]);

    if (c.kind == ClipKind::audio)
    {
        const auto path = v["file"].toString();
        const auto base = baseDir != File() ? baseDir : workDir;
        c.file = File::isAbsolutePath (path) ? File (path) : base.getChildFile (path);
        c.sourceOffset = (int64) v["offset"];
        c.gainDb = (float) (double) v["gain"];
    }
    else
    {
        if (auto* notes = v["notes"].getArray())
        {
            for (auto& n : *notes)
            {
                Note note;
                note.pitch = jlimit (0, 127, (int) n[0]);
                note.start = (double) n[1];
                note.length = jmax (0.001, (double) n[2]);
                note.velocity = jlimit (0.0f, 1.0f, (float) (double) n[3]);
                c.notes.push_back (note);
            }
        }

        if (auto* events = v["events"].getArray())
        {
            for (auto& e : *events)
            {
                if (auto* a = e.getArray(); a != nullptr && a->size() >= 2)
                {
                    MidiEvent ev;
                    ev.beat = (double) (*a)[0];
                    ev.size = jmin (3, a->size() - 1);
                    for (int i = 0; i < ev.size; ++i)
                        ev.data[i] = (uint8) (int) (*a)[i + 1];
                    c.events.push_back (ev);
                }
            }
        }
    }

    return c;
}

var Edit::trackToVar (const Track& t, bool asTemplate) const
{
    DynamicObject::Ptr o = new DynamicObject();
    o->setProperty ("id", t.id);
    o->setProperty ("kind", trackKindToString (t.kind));
    o->setProperty ("name", t.name);
    o->setProperty ("colour", t.colour.toString());
    o->setProperty ("volume", t.volumeDb.load());
    o->setProperty ("pan", t.pan.load());
    o->setProperty ("mute", t.mute.load());
    o->setProperty ("solo", t.solo.load());
    o->setProperty ("arm", t.arm.load());
    o->setProperty ("monitor", monitorToString (t.monitor.load()));

    if (t.isAudio())
    {
        DynamicObject::Ptr in = new DynamicObject();
        in->setProperty ("channel", t.input.channel);
        in->setProperty ("stereo", t.input.stereo);
        o->setProperty ("input", var (in.get()));
    }

    if (t.groupId >= 0)
        o->setProperty ("group", t.groupId);

    if (t.isMidi())
    {
        o->setProperty ("midiInput", t.midiInput);
        o->setProperty ("midiOutput", t.midiOutput);
        o->setProperty ("midiChannel", t.midiChannel);
    }

    DynamicObject::Ptr out = new DynamicObject();
    out->setProperty ("kind", t.output.kind == OutputRoute::Kind::bus ? "return"
                            : t.output.kind == OutputRoute::Kind::hardware ? "hardware" : "master");
    out->setProperty ("return", t.output.busId);
    out->setProperty ("channel", t.output.channel);
    out->setProperty ("stereo", t.output.stereo);
    o->setProperty ("output", var (out.get()));

    Array<var> sends;
    for (auto& s : t.sends)
    {
        DynamicObject::Ptr so = new DynamicObject();
        so->setProperty ("return", s->busId);
        so->setProperty ("level", s->levelDb.load());
        so->setProperty ("pre", s->preFader.load());
        sends.add (var (so.get()));
    }
    o->setProperty ("sends", sends);

    Array<var> plugins;
    for (auto& p : t.plugins)
    {
        DynamicObject::Ptr po = new DynamicObject();
        if (auto xml = p->description.createXml())
            po->setProperty ("description", xml->toString (XmlElement::TextFormat().singleLine().withoutHeader()));

        MemoryBlock state;
        if (p->instance != nullptr)
            p->instance->getStateInformation (state);
        else
            state = p->savedState;

        po->setProperty ("state", state.toBase64Encoding());
        po->setProperty ("bypassed", p->bypassed.load());
        plugins.add (var (po.get()));
    }
    o->setProperty ("plugins", plugins);
    o->setProperty ("eq", t.eq.toVar());

    if (! asTemplate && t.holdsClips())
    {
        Array<var> clips;
        for (auto& c : t.clips)
            clips.add (clipToVar (c));
        o->setProperty ("clips", clips);
    }

    return var (o.get());
}

std::shared_ptr<Track> Edit::trackFromVar (const var& v, const File& baseDir, const PluginFactory& makePlugin)
{
    auto t = std::make_shared<Track>();
    t->id = (int) v["id"];
    t->kind = trackKindFromString (v["kind"].toString());
    t->name = v["name"].toString();
    t->colour = v["colour"].isString() ? Colour::fromString (v["colour"].toString()) : nextColour();
    t->volumeDb = (float) (double) v["volume"];
    t->pan = (float) (double) v["pan"];
    t->mute = (bool) v["mute"];
    t->solo = (bool) v["solo"];
    t->arm = (bool) v["arm"];
    t->monitor = monitorFromString (v["monitor"].toString());

    if (auto in = v["input"]; in.isObject())
    {
        t->input.channel = (int) in["channel"];
        t->input.stereo = (bool) in["stereo"];
    }

    t->groupId = v.hasProperty ("group") ? (int) v["group"] : -1;

    if (v.hasProperty ("midiInput"))
    {
        t->midiInput = v["midiInput"].toString();
        t->midiOutput = v["midiOutput"].toString();
        t->midiChannel = jlimit (1, 16, (int) v["midiChannel"]);
    }

    if (auto out = v["output"]; out.isObject())
    {
        const auto kind = out["kind"].toString();
        t->output.kind = kind == "return" ? OutputRoute::Kind::bus
                       : kind == "hardware" ? OutputRoute::Kind::hardware : OutputRoute::Kind::master;
        t->output.busId = (int) out["return"];
        t->output.channel = (int) out["channel"];
        t->output.stereo = out.hasProperty ("stereo") ? (bool) out["stereo"] : true;
    }

    if (auto* sends = v["sends"].getArray())
    {
        for (auto& s : *sends)
        {
            auto send = std::make_shared<Send>();
            send->busId = (int) s["return"];
            send->levelDb = (float) (double) s["level"];
            send->preFader = (bool) s["pre"];
            t->sends.push_back (send);
        }
    }

    if (auto* plugins = v["plugins"].getArray())
    {
        for (auto& p : *plugins)
        {
            PluginDescription desc;
            if (auto xml = parseXML (p["description"].toString()))
                desc.loadFromXml (*xml);

            MemoryBlock state;
            state.fromBase64Encoding (p["state"].toString());

            if (auto slot = makePlugin (desc, state))
            {
                slot->bypassed = (bool) p["bypassed"];
                t->plugins.push_back (slot);
            }
        }
    }

    t->eq.fromVar (v["eq"]);

    if (auto* clips = v["clips"].getArray())
        for (auto& c : *clips)
            t->clips.push_back (clipFromVar (c, baseDir));

    return t;
}

var Edit::toVar (bool asTemplate) const
{
    DynamicObject::Ptr o = new DynamicObject();
    o->setProperty ("format", asTemplate ? "lathe-template" : "lathe-project");
    o->setProperty ("version", 1);
    o->setProperty ("sampleRate", sampleRate);
    o->setProperty ("tempo", tempo);
    o->setProperty ("timeSignature", Array<var> { timeSigNum, timeSigDen });

    DynamicObject::Ptr loop = new DynamicObject();
    loop->setProperty ("enabled", loopEnabled);
    loop->setProperty ("start", loopStart);
    loop->setProperty ("end", loopEnd);
    o->setProperty ("loop", var (loop.get()));

    if (! asTemplate)
        o->setProperty ("insertMarker", insertMarker);

    Array<var> trackList, busList;
    for (auto& t : tracks) trackList.add (trackToVar (*t, asTemplate));
    for (auto& t : buses)  busList.add (trackToVar (*t, asTemplate));

    o->setProperty ("tracks", trackList);

    Array<var> groupList;
    for (auto& g : groups)
    {
        DynamicObject::Ptr go = new DynamicObject();
        go->setProperty ("id", g->id);
        go->setProperty ("name", g->name);
        go->setProperty ("colour", g->colour.toString());
        go->setProperty ("mute", g->mute.load());
        go->setProperty ("solo", g->solo.load());
        go->setProperty ("collapsed", g->collapsed);
        if (g->busId >= 0)
            go->setProperty ("bus", g->busId);
        groupList.add (var (go.get()));
    }
    if (! groupList.isEmpty())
        o->setProperty ("groups", groupList);
    o->setProperty ("returns", busList);
    o->setProperty ("master", trackToVar (*master, asTemplate));

    Array<var> cueList;
    for (auto& c : cues)
    {
        DynamicObject::Ptr co = new DynamicObject();
        co->setProperty ("id", c->id);
        co->setProperty ("name", c->name);
        co->setProperty ("colour", c->colour.toString());
        co->setProperty ("channel", c->channel.load());
        co->setProperty ("volume", c->volumeDb.load());
        co->setProperty ("mute", c->mute.load());
        co->setProperty ("click", c->clickDb.load());

        Array<var> levelList;
        for (auto& [id, l] : c->levels)
        {
            if (findTrack (id) == nullptr)
                continue;   // track since deleted
            DynamicObject::Ptr lo = new DynamicObject();
            lo->setProperty ("track", id);
            lo->setProperty ("level", l->levelDb.load());
            lo->setProperty ("pan", l->pan.load());
            lo->setProperty ("mute", l->mute.load());
            levelList.add (var (lo.get()));
        }
        co->setProperty ("levels", levelList);
        cueList.add (var (co.get()));
    }
    if (! cueList.isEmpty())
        o->setProperty ("cues", cueList);
    if (! clickInMain.load())
        o->setProperty ("clickInMain", false);
    return var (o.get());
}

Result Edit::loadFrom (const var& data, const File& baseDir, const PluginFactory& makePlugin)
{
    const auto format = data["format"].toString();
    if (format != "lathe-project" && format != "lathe-template")
        return Result::fail ("This isn't a Lathe project file.");

    sampleRate = data.hasProperty ("sampleRate") ? (double) data["sampleRate"] : 48000.0;
    tempo = jlimit (20.0, 400.0, data.hasProperty ("tempo") ? (double) data["tempo"] : 120.0);

    if (auto* ts = data["timeSignature"].getArray(); ts != nullptr && ts->size() == 2)
    {
        timeSigNum = jlimit (1, 32, (int) (*ts)[0]);
        timeSigDen = jlimit (1, 32, (int) (*ts)[1]);
    }

    if (auto loop = data["loop"]; loop.isObject())
    {
        loopEnabled = (bool) loop["enabled"];
        loopStart = (double) loop["start"];
        loopEnd = jmax (loopStart + 1.0, (double) loop["end"]);
    }

    insertMarker = (double) data["insertMarker"];

    tracks.clear();
    buses.clear();
    groups.clear();

    if (auto* list = data["groups"].getArray())
    {
        for (auto& v : *list)
        {
            auto g = std::make_shared<TrackGroup>();
            g->id = (int) v["id"];
            g->name = v["name"].toString();
            g->colour = Colour::fromString (v["colour"].toString());
            g->mute = (bool) v["mute"];
            g->solo = (bool) v["solo"];
            g->collapsed = (bool) v["collapsed"];
            g->busId = v.hasProperty ("bus") ? (int) v["bus"] : -1;
            groups.push_back (g);
        }
    }

    if (auto* list = data["tracks"].getArray())
        for (auto& v : *list)
            tracks.push_back (trackFromVar (v, baseDir, makePlugin));

    if (auto* list = data["returns"].getArray())
        for (auto& v : *list)
            buses.push_back (trackFromVar (v, baseDir, makePlugin));

    if (auto m = data["master"]; m.isObject())
    {
        master = trackFromVar (m, baseDir, makePlugin);
        master->kind = TrackKind::master;
        if (master->output.kind != OutputRoute::Kind::hardware)
            master->output = { OutputRoute::Kind::hardware, -1, 0, true };
    }

    cues.clear();
    clickInMain = data.hasProperty ("clickInMain") ? (bool) data["clickInMain"] : true;
    const var cueData = data["cues"];
    if (auto* list = cueData.getArray())
    {
        for (auto& v : *list)
        {
            auto c = std::make_shared<CueMix>();
            c->id = (int) v["id"];
            c->name = v["name"].toString();
            c->colour = v["colour"].isString() ? Colour::fromString (v["colour"].toString()) : Colour (0xff5fb3a8);
            c->channel = jmax (0, (int) v["channel"]);
            c->volumeDb = (float) (double) v["volume"];
            c->mute = (bool) v["mute"];
            c->clickDb = v.hasProperty ("click") ? (float) (double) v["click"] : 0.0f;

            const var levelData = v["levels"];
            if (auto* levels = levelData.getArray())
            {
                for (auto& lv : *levels)
                {
                    auto l = std::make_shared<CueLevel>();
                    l->levelDb = (float) (double) lv["level"];
                    l->pan = jlimit (-1.0f, 1.0f, (float) (double) lv["pan"]);
                    l->mute = (bool) lv["mute"];
                    c->levels[(int) lv["track"]] = l;
                }
            }
            cues.push_back (c);
        }
    }

    // Keep new ids clear of everything loaded.
    // Tracks pointing at a group that isn't there are ungrouped; empty groups go.
    for (auto& t : tracks)
        if (t->groupId >= 0 && findGroup (t->groupId) == nullptr)
            t->groupId = -1;
    removeEmptyGroups();
    for (auto& g : groups)
        if (g->busId >= 0 && findGroupBus (g->id) == nullptr)
            g->busId = -1;

    int maxId = master->id;
    for (auto& g : groups)
        maxId = jmax (maxId, g->id);
    for (auto& c : cues)
        maxId = jmax (maxId, c->id);
    for (auto* list : { &tracks, &buses })
        for (auto& t : *list)
        {
            maxId = jmax (maxId, t->id);
            for (auto& c : t->clips)
                maxId = jmax (maxId, c.id);
        }
    lastId = maxId;
    colourIndex = (int) (tracks.size() + buses.size());

    undoStack.clear();
    redoStack.clear();
    dirty = false;
    return Result::ok();
}

} // namespace lathe
