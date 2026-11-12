#include "SessionWatch.h"
#include "../Cloud/ProjectDiff.h"

namespace lathe::assistant
{
namespace
{
const StringArray liveChannelKeys { "volume", "pan", "mute", "solo", "sends", "arm", "monitor" };
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
} // namespace

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
        c.any = c.structural = true;
        c.otherProject = file != File() ? file.getFileNameWithoutExtension() : String ("Untitled");
        return c;
    }
    if (same (comparable (last, false), comparable (project, false)))
        return c;
    c.any = true;
    c.structural = ! same (comparable (last, true), comparable (project, true));
    c.words = cloud::describeChanges (last, project);
    return c;
}

bool SessionWatch::changesSong (const String& tool)
{
    return tool == "edit_session" || tool == "write_notes" || tool == "set_plugin_params";
}
} // namespace lathe::assistant
