#include "SessionWatch.h"
#include "../Cloud/ProjectDiff.h"
#include <map>

namespace lathe::assistant
{
namespace
{
const StringArray liveChannelKeys { "volume", "pan", "mute", "solo", "sends", "arm", "monitor", "eq" };
const StringArray liveSongKeys { "loop", "insertMarker", "notes" };

/** A copy to compare: plug-ins by their readable settings where Lathe could read them (their saved
    state can change by itself, say when their window opens), and without the live keys if asked. */
var comparable (const var& project, bool withoutLive)
{
    auto copy = JSON::parse (JSON::toString (project, true));
    auto* o = copy.getDynamicObject();
    if (o == nullptr)
        return copy;

    auto channel = [withoutLive] (const var& c)
    {
        auto* co = c.getDynamicObject();
        if (co == nullptr)
            return;
        if (withoutLive)
            for (auto& k : liveChannelKeys)
                co->removeProperty (Identifier (k));
        if (auto* plugins = c["plugins"].getArray())
            for (auto& p : *plugins)
                if (auto* po = p.getDynamicObject(); po != nullptr && po->hasProperty ("params"))
                    po->removeProperty ("state");
    };
    for (auto* key : { "tracks", "returns" })
        if (auto* a = copy[key].getArray())
            for (auto& c : *a)
                channel (c);
    channel (copy["master"]);

    o->removeProperty ("insertMarker");
    o->removeProperty ("notes");
    if (withoutLive)
        for (auto& k : liveSongKeys)
            o->removeProperty (Identifier (k));
    return copy;
}

bool same (const var& a, const var& b)
{
    return JSON::toString (a, true) == JSON::toString (b, true);
}

/** Every channel (tracks, returns, master) by id, and the song with each channel replaced by
    its id, so a change can be placed in one channel or the song as a whole. */
void split (const var& comparableProject, std::map<int, String>& channels, String& song)
{
    auto copy = JSON::parse (JSON::toString (comparableProject, true));
    for (auto* key : { "tracks", "returns" })
        if (auto* a = copy[key].getArray())
            for (auto& c : *a)
            {
                const int id = (int) c["id"];
                channels[id] = JSON::toString (c, true);
                c = id;
            }
    if (auto m = copy["master"]; m.isObject())
    {
        const int id = (int) m["id"];
        channels[id] = JSON::toString (m, true);
        copy.getDynamicObject()->setProperty ("master", id);
    }
    song = JSON::toString (copy, true);
}
} // namespace

bool SessionChange::overlaps (const Array<int>& targets, bool targetsSongWide) const
{
    if (! structural)
        return false;
    if (songWide || targetsSongWide)
        return true;
    for (auto id : targets)
        if (channels.contains (id))
            return true;
    return false;
}

String SessionChange::list (int maxLines) const
{
    if (otherProject.isNotEmpty())
        return "- opened a different project: \"" + otherProject + "\"";
    StringArray out;
    for (int i = 0; i < words.size() && i < maxLines; ++i)
        out.add ("- " + words[i]);
    if (words.size() > maxLines)
        out.add ("- and " + String (words.size() - maxLines) + " more");
    if (out.isEmpty())
        out.add ("- something Lathe can't describe in words (a plug-in's own settings, say)");
    return out.joinIntoString ("\n");
}

void SessionWatch::seen (const var& project, const File& file)
{
    last = project;
    lastFile = file;
    has = true;
}

SessionChange SessionWatch::compare (const var& project, const File& file) const
{
    SessionChange c;
    if (! has)
        return c;
    if (file != lastFile)
    {
        c.any = c.structural = c.songWide = true;
        c.otherProject = file != File() ? file.getFileNameWithoutExtension() : String ("Untitled");
        return c;
    }
    if (same (comparable (last, false), comparable (project, false)))
        return c;
    c.any = true;
    const auto before = comparable (last, true), after = comparable (project, true);
    c.structural = ! same (before, after);
    c.words = cloud::describeChanges (last, project);
    if (c.structural)
    {
        std::map<int, String> a, b;
        String songA, songB;
        split (before, a, songA);
        split (after, b, songB);
        c.songWide = songA != songB;   // ids in the same order, so the track list is the same too
        for (auto& [id, text] : a)
            if (auto it = b.find (id); it == b.end() || it->second != text)
                c.channels.add (id);
        for (auto& [id, text] : b)
            if (a.find (id) == a.end())
                c.channels.addIfNotAlreadyThere (id);
    }
    return c;
}

bool SessionWatch::changesSong (const String& tool)
{
    return tool == "edit_session" || tool == "write_notes" || tool == "set_plugin_params" || tool == "import_midi";
}
} // namespace lathe::assistant
