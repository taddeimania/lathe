#include "Merge.h"
#include "ProjectDiff.h"

namespace lathe::cloud
{
namespace
{
using Conflicts = std::vector<MergeConflict>;
using Locate = std::function<DynamicObject* (var& project)>;   // finds the object a list lives in

var copyOf (const var& v)
{
    if (v.isVoid() || v.isUndefined())
        return {};
    if (! v.isObject() && ! v.isArray())
        return v;   // numbers, text, true/false: values already
    return JSON::parse (JSON::toString (v, true));
}

/** What a person would call the same: where a clip's audio lives on this machine, and the
    readable plug-in values (only there when the plug-in loaded), don't count. */
void canonicalise (var& v)
{
    if (auto* o = v.getDynamicObject())
    {
        if (o->hasProperty ("hash") && o->getProperty ("hash").toString().isNotEmpty())
            o->removeProperty ("file");
        if (o->hasProperty ("state"))
            o->removeProperty ("params");
        for (auto& p : o->getProperties())
        {
            var child = p.value;   // shares the object or array it holds, so changes reach it
            canonicalise (child);
        }
    }
    else if (auto* a = v.getArray())
    {
        for (auto& x : *a)
            canonicalise (x);
    }
}

String canonical (const var& v)
{
    if (v.isVoid() || v.isUndefined())
        return "<none>";
    auto c = copyOf (v);
    canonicalise (c);
    return JSON::toString (c, true);
}

bool same (const var& a, const var& b)
{
    return canonical (a) == canonical (b);
}

enum class Side { theirs, yours, conflict };

Side pick (const var& base, const var& yours, const var& theirs)
{
    if (same (yours, theirs) || same (yours, base))
        return Side::theirs;
    if (same (theirs, base))
        return Side::yours;
    return Side::conflict;
}

void setOrRemove (DynamicObject* o, const Identifier& key, const var& value)
{
    if (o == nullptr)
        return;
    if (value.isVoid() || value.isUndefined())
        o->removeProperty (key);
    else
        o->setProperty (key, copyOf (value));
}

var byId (const var& list, int id)
{
    if (auto* a = list.getArray())
        for (auto& x : *a)
            if ((int) x["id"] == id)
                return x;
    return {};
}

int indexOfId (const Array<var>& list, int id)
{
    for (int i = 0; i < list.size(); ++i)
        if ((int) list.getReference (i)["id"] == id)
            return i;
    return -1;
}

Array<int> idsIn (const var& list)
{
    Array<int> out;
    if (auto* a = list.getArray())
        for (auto& x : *a)
            out.add ((int) x["id"]);
    return out;
}

/** Puts item into list after the nearest item before it in `order` that's in the list. */
void insertInOrder (Array<var>& list, const Array<int>& order, const var& item)
{
    const int id = (int) item["id"];
    const int at = order.indexOf (id);
    if (at < 0 || at == order.size() - 1)
    {
        list.add (item);   // added at the end: stays at the end
        return;
    }
    for (int i = at - 1; i >= 0; --i)
    {
        const int idx = indexOfId (list, order[i]);
        if (idx >= 0)
        {
            list.insert (idx + 1, item);
            return;
        }
    }
    list.insert (0, item);
}

Array<var>* listIn (DynamicObject* container, const Identifier& key)
{
    if (container == nullptr)
        return nullptr;
    if (! container->hasProperty (key) || container->getProperty (key).getArray() == nullptr)
        container->setProperty (key, Array<var>());
    return container->getProperty (key).getArray();
}

//==============================================================================
// Ids: one counter makes tracks, returns, the master, clips, groups and monitor mixes, so
// two people adding things on their own computers can make the same id.

void collectIds (const var& p, std::set<int>& out)
{
    if (p["master"].isObject())
        out.insert ((int) p["master"]["id"]);
    for (auto* key : { "tracks", "returns" })
        if (auto* a = p[key].getArray())
            for (auto& t : *a)
            {
                out.insert ((int) t["id"]);
                if (auto* clips = t["clips"].getArray())
                    for (auto& c : *clips)
                        out.insert ((int) c["id"]);
            }
    for (auto* key : { "groups", "cues" })
        if (auto* a = p[key].getArray())
            for (auto& x : *a)
                out.insert ((int) x["id"]);
}

var renumberYours (const var& base, const var& yours, const var& theirs)
{
    std::set<int> b, y, t;
    collectIds (base, b);
    collectIds (yours, y);
    collectIds (theirs, t);
    int next = 1;
    for (auto* s : { &b, &y, &t })
        if (! s->empty())
            next = jmax (next, *s->rbegin() + 1);

    std::map<int, int> remap;
    for (int id : y)
        if (b.count (id) == 0 && t.count (id) > 0)
            remap[id] = next++;
    if (remap.empty())
        return yours;

    auto out = copyOf (yours);
    auto fix = [&remap] (const var& v, const char* key)
    {
        if (auto* o = v.getDynamicObject(); o != nullptr && o->hasProperty (key))
            if (auto it = remap.find ((int) o->getProperty (key)); it != remap.end())
                o->setProperty (key, it->second);
    };
    auto fixChannel = [&] (const var& c)
    {
        fix (c, "id");
        fix (c, "group");
        fix (c["input"], "track");
        fix (c["output"], "return");
        if (auto* sends = c["sends"].getArray())
            for (auto& s : *sends)
                fix (s, "return");
        if (auto* clips = c["clips"].getArray())
            for (auto& clip : *clips)
                fix (clip, "id");
    };
    for (auto* key : { "tracks", "returns" })
        if (auto* a = out[key].getArray())
            for (auto& c : *a)
                fixChannel (c);
    fixChannel (out["master"]);
    if (auto* a = out["groups"].getArray())
        for (auto& g : *a)
            fix (g, "id"), fix (g, "bus");
    if (auto* a = out["cues"].getArray())
        for (auto& c : *a)
        {
            fix (c, "id");
            if (auto* levels = c["levels"].getArray())
                for (auto& l : *levels)
                    fix (l, "track");
        }
    return out;
}

//==============================================================================
// Words

String keyName (const String& key)
{
    static const std::map<String, String> names {
        { "volume", "fader" }, { "pan", "pan" }, { "mute", "mute" }, { "solo", "solo" }, { "name", "name" },
        { "colour", "colour" }, { "input", "input" }, { "output", "output" }, { "sends", "sends" }, { "eq", "EQ" },
        { "plugins", "plug-ins" }, { "group", "group" }, { "kind", "track type" }, { "midiInput", "MIDI input" },
        { "midiOutput", "MIDI output" }, { "midiChannel", "MIDI channel" } };
    auto it = names.find (key);
    return it != names.end() ? it->second : key;
}

String afterLabel (const String& line)
{
    // describeChanges writes '"Guitar": fader -8.0 dB -> -6.0 dB'; the label is already shown.
    return line.startsWithChar ('"') || line.startsWith ("Master:") ? line.fromFirstOccurrenceOf (": ", false, false) : line;
}

String wordsFor (const var& before, const var& after)
{
    StringArray lines;
    for (auto& l : describeChanges (before, after))
        lines.add (afterLabel (l));
    return lines.isEmpty() ? String ("changed it") : lines.joinIntoString ("; ");
}

var wrapChannel (const String& listKey, const var& channel)
{
    DynamicObject::Ptr p = new DynamicObject();
    if (listKey == "master")
        p->setProperty ("master", channel);
    else
        p->setProperty (listKey, Array<var> { channel });
    return var (p.get());
}

/** What one side did to a channel, compared with the base. */
String channelWords (const String& listKey, const var& base, const var& side, const String& onlyKey = {})
{
    if (! side.isObject())
        return "removed it";
    auto after = copyOf (side);
    if (onlyKey.isNotEmpty())
    {
        after = copyOf (base);
        setOrRemove (after.getDynamicObject(), onlyKey, side[Identifier (onlyKey)]);
    }
    return wordsFor (wrapChannel (listKey, base), wrapChannel (listKey, after));
}

String songWords (const var& base, const var& side, const String& key)
{
    DynamicObject::Ptr b = new DynamicObject(), a = new DynamicObject();
    b->setProperty (key, copyOf (base[Identifier (key)]));
    a->setProperty (key, copyOf (side[Identifier (key)]));
    if (side[Identifier (key)].isVoid())
        return "removed it";
    // The row is already labelled "Tempo": just the numbers.
    const auto words = wordsFor (var (b.get()), var (a.get()));
    return key == "tempo" && words.startsWith ("Tempo ") ? words.substring (6) : words;
}

String channelLabel (const var& ch, const String& listKey)
{
    if (listKey == "master")
        return "Master";
    return "\"" + ch["name"].toString() + "\"" + (listKey == "returns" ? " (return)" : String());
}

String clipLabel (const var& clip)
{
    const auto name = clip["name"].toString();
    return name.isNotEmpty() ? "clip \"" + name + "\"" : String ("a clip");
}

//==============================================================================

class Merger
{
public:
    Merger (const var& b, const var& y, const var& t) : base (b), yours (y), theirs (t) {}

    MergeResult run()
    {
        MergeResult r;
        r.project = copyOf (theirs);
        auto* out = r.project.getDynamicObject();
        if (out == nullptr)
            return r;

        mergeSongSettings (*out);
        mergeChannels (*out, "tracks");
        mergeChannels (*out, "returns");
        mergeMaster (*out);
        mergeWholeObjects (*out, "groups", [] (const var& g) { return "Group \"" + g["name"].toString() + "\""; });
        mergeWholeObjects (*out, "cues", [] (const var& c) { return "Monitor mix \"" + c["name"].toString() + "\""; });

        r.conflicts = std::move (conflicts);
        r.yoursChangedAnything = ! same (yours, base);
        return r;
    }

private:
    void conflict (const String& what, const String& y, const String& t, std::function<void (var&)> takeYours)
    {
        conflicts.push_back ({ what, y, t, std::move (takeYours) });
    }

    // --- tempo, time signature, loop...
    void mergeSongSettings (DynamicObject& out)
    {
        static const StringArray lists { "tracks", "returns", "master", "groups", "cues", "notes", "insertMarker" };
        StringArray keys;
        for (auto* v : { &base, &yours, &theirs })
            if (auto* o = v->getDynamicObject())
                for (auto& p : o->getProperties())
                    if (! lists.contains (p.name.toString()))
                        keys.addIfNotAlreadyThere (p.name.toString());

        for (auto& key : keys)
        {
            const Identifier k (key);
            const auto side = pick (base[k], yours[k], theirs[k]);
            if (side == Side::yours || (side == Side::conflict && key == "loop"))   // the loop region is yours to keep
                setOrRemove (&out, k, yours[k]);
            else if (side == Side::conflict)
            {
                const auto mine = copyOf (yours[k]);
                conflict (key == "timeSignature" ? String ("Time signature") : key == "tempo" ? String ("Tempo") : key,
                          songWords (base, yours, key), songWords (base, theirs, key),
                          [k, mine] (var& p) { setOrRemove (p.getDynamicObject(), k, mine); });
            }
        }
    }

    // --- one channel present in all three: per setting, and per clip
    void mergeChannel (DynamicObject& out, const var& b, const var& y, const var& t, const String& listKey, Locate locate)
    {
        const auto label = channelLabel (t, listKey);
        StringArray keys;
        for (auto* v : { &b, &y, &t })
            if (auto* o = v->getDynamicObject())
                for (auto& p : o->getProperties())
                    keys.addIfNotAlreadyThere (p.name.toString());

        for (auto& key : keys)
        {
            if (key == "id")
                continue;
            const Identifier k (key);
            if (key == "clips")
            {
                mergeClips (out, b[k], y[k], t[k], label, locate);
                continue;
            }
            const auto side = pick (b[k], y[k], t[k]);
            // Record arm and monitoring are how you have your own computer set up.
            if (side == Side::yours || (side == Side::conflict && (key == "arm" || key == "monitor")))
                setOrRemove (&out, k, y[k]);
            else if (side == Side::conflict)
            {
                const auto mine = copyOf (y[k]);
                conflict (label + ": " + keyName (key), channelWords (listKey, b, y, key), channelWords (listKey, b, t, key),
                          [locate, k, mine] (var& p) { setOrRemove (locate (p), k, mine); });
            }
        }
    }

    void mergeClips (DynamicObject& channel, const var& b, const var& y, const var& t, const String& label, Locate locate)
    {
        auto* list = listIn (&channel, "clips");
        if (list == nullptr)
            return;
        const auto yourOrder = idsIn (y);
        std::set<int> ids;
        for (auto* v : { &b, &y, &t })
            for (int id : idsIn (*v))
                ids.insert (id);

        auto clipsOf = [locate] (var& p) { return listIn (locate (p), "clips"); };
        for (int id : ids)
        {
            const auto cb = byId (b, id), cy = byId (y, id), ct = byId (t, id);
            if (! cb.isObject())
            {
                if (cy.isObject() && ! ct.isObject())
                    insertInOrder (*list, yourOrder, copyOf (cy));   // a clip you made
                continue;
            }
            if (! cy.isObject() && ! ct.isObject())
                continue;
            if (! cy.isObject())   // you removed it
            {
                if (same (ct, cb))
                    list->remove (indexOfId (*list, id));
                else
                    conflict (label + ": " + clipLabel (ct), "removed it", "edited it",
                              [clipsOf, id] (var& p) { if (auto* l = clipsOf (p)) l->remove (indexOfId (*l, id)); });
                continue;
            }
            if (! ct.isObject())   // they removed it
            {
                if (! same (cy, cb))
                {
                    const auto mine = copyOf (cy);
                    conflict (label + ": " + clipLabel (cy), "edited it", "removed it",
                              [clipsOf, mine, yourOrder] (var& p) { if (auto* l = clipsOf (p)) insertInOrder (*l, yourOrder, mine); });
                }
                continue;
            }
            const auto side = pick (cb, cy, ct);
            if (side == Side::yours)
                list->set (indexOfId (*list, id), copyOf (cy));
            else if (side == Side::conflict)
            {
                const auto mine = copyOf (cy);
                conflict (label + ": " + clipLabel (ct), "edited it", "edited it too",
                          [clipsOf, id, mine] (var& p) { if (auto* l = clipsOf (p)) l->set (indexOfId (*l, id), mine); });
            }
        }
    }

    // --- tracks and returns
    void mergeChannels (DynamicObject& out, const String& listKey)
    {
        const Identifier key (listKey);
        auto* list = listIn (&out, key);
        const auto bl = base[key], yl = yours[key], tl = theirs[key];
        const auto yourOrder = idsIn (yl);
        std::set<int> ids;
        for (auto* v : { &bl, &yl, &tl })
            for (int id : idsIn (*v))
                ids.insert (id);

        auto listOf = [key] (var& p) { return listIn (p.getDynamicObject(), key); };
        for (int id : ids)
        {
            const auto b = byId (bl, id), y = byId (yl, id), t = byId (tl, id);
            if (! b.isObject())
            {
                if (y.isObject() && ! t.isObject())
                    insertInOrder (*list, yourOrder, copyOf (y));   // a track you added
                continue;
            }
            if (! y.isObject() && ! t.isObject())
                continue;
            if (! y.isObject())   // you removed it
            {
                if (same (t, b))
                    list->remove (indexOfId (*list, id));
                else
                    conflict (channelLabel (t, listKey), "removed it", channelWords (listKey, b, t),
                              [listOf, id] (var& p) { if (auto* l = listOf (p)) l->remove (indexOfId (*l, id)); });
                continue;
            }
            if (! t.isObject())   // they removed it
            {
                if (! same (y, b))
                {
                    const auto mine = copyOf (y);
                    conflict (channelLabel (y, listKey), channelWords (listKey, b, y), "removed it",
                              [listOf, mine, yourOrder] (var& p) { if (auto* l = listOf (p)) insertInOrder (*l, yourOrder, mine); });
                }
                continue;
            }
            const int at = indexOfId (*list, id);
            if (auto* channel = list->getReference (at).getDynamicObject())
                mergeChannel (*channel, b, y, t, listKey, [key, id] (var& p) -> DynamicObject*
                {
                    if (auto* l = listIn (p.getDynamicObject(), key))
                        if (const int i = indexOfId (*l, id); i >= 0)
                            return l->getReference (i).getDynamicObject();
                    return nullptr;
                });
        }

        // If you reordered the tracks and they didn't, your order wins.
        auto reordered = [] (const Array<int>& before, const Array<int>& after)
        {
            Array<int> x, y;
            for (int id : before) if (after.contains (id)) x.add (id);
            for (int id : after)  if (before.contains (id)) y.add (id);
            return x != y;
        };
        const auto baseOrder = idsIn (bl), theirOrder = idsIn (tl);
        if (reordered (baseOrder, yourOrder) && ! reordered (baseOrder, theirOrder))
        {
            Array<var> sorted;
            for (int id : yourOrder)
                if (const int i = indexOfId (*list, id); i >= 0)
                    sorted.add (list->getReference (i));
            for (auto& item : *list)
                if (indexOfId (sorted, (int) item["id"]) < 0)
                    insertInOrder (sorted, theirOrder, item);
            *list = sorted;
        }
    }

    void mergeMaster (DynamicObject& out)
    {
        const auto b = base["master"], y = yours["master"];
        if (! b.isObject() || ! y.isObject() || ! out.getProperty ("master").isObject())
            return;
        if (auto* m = out.getProperty ("master").getDynamicObject())
            mergeChannel (*m, b, y, theirs["master"], "master",
                          [] (var& p) { return p["master"].getDynamicObject(); });
    }

    // --- groups and monitor mixes, each as a whole
    void mergeWholeObjects (DynamicObject& out, const String& listKey, std::function<String (const var&)> label)
    {
        const Identifier key (listKey);
        const auto bl = base[key], yl = yours[key], tl = theirs[key];
        if (! bl.isArray() && ! yl.isArray() && ! tl.isArray())
            return;
        auto* list = listIn (&out, key);
        const auto yourOrder = idsIn (yl);
        std::set<int> ids;
        for (auto* v : { &bl, &yl, &tl })
            for (int id : idsIn (*v))
                ids.insert (id);

        auto listOf = [key] (var& p) { return listIn (p.getDynamicObject(), key); };
        for (int id : ids)
        {
            const auto b = byId (bl, id), y = byId (yl, id), t = byId (tl, id);
            if (! b.isObject())
            {
                if (y.isObject() && ! t.isObject())
                    insertInOrder (*list, yourOrder, copyOf (y));
                continue;
            }
            if (! y.isObject() && ! t.isObject())
                continue;
            if (! y.isObject())
            {
                if (same (t, b))
                    list->remove (indexOfId (*list, id));
                else
                    conflict (label (t), "removed it", "changed it",
                              [listOf, id] (var& p) { if (auto* l = listOf (p)) l->remove (indexOfId (*l, id)); });
                continue;
            }
            if (! t.isObject())
            {
                if (! same (y, b))
                {
                    const auto mine = copyOf (y);
                    conflict (label (y), "changed it", "removed it",
                              [listOf, mine, yourOrder] (var& p) { if (auto* l = listOf (p)) insertInOrder (*l, yourOrder, mine); });
                }
                continue;
            }
            const auto side = pick (b, y, t);
            if (side == Side::yours)
                list->set (indexOfId (*list, id), copyOf (y));
            else if (side == Side::conflict)
            {
                const auto mine = copyOf (y);
                conflict (label (t), "changed it", "changed it too",
                          [listOf, id, mine] (var& p) { if (auto* l = listOf (p)) l->set (indexOfId (*l, id), mine); });
            }
        }
        if (list->isEmpty())
            out.removeProperty (key);
    }

    const var& base;
    const var yours;
    const var& theirs;
    Conflicts conflicts;
};
} // namespace

MergeResult mergeProjects (const var& base, const var& yours, const var& theirs)
{
    if (! base.isObject())
    {
        // No common starting point (shouldn't happen for a song that was pushed): keep theirs,
        // and say everything of yours differs.
        MergeResult r;
        r.project = copyOf (theirs);
        r.yoursChangedAnything = ! same (yours, theirs);
        return r;
    }
    return Merger (base, renumberYours (base, yours, theirs), theirs).run();
}
} // namespace lathe::cloud
