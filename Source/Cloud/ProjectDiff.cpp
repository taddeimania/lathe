#include "ProjectDiff.h"
#include "../Model/ChannelEq.h"

namespace lathe::cloud
{
namespace
{
const Array<var>& arrayOf (const var& v)
{
    static const Array<var> empty;
    if (auto* a = v.getArray())
        return *a;
    return empty;
}

bool same (const var& a, const var& b)
{
    return JSON::toString (a, true) == JSON::toString (b, true);
}

bool near (double a, double b, double tolerance = 0.01)
{
    return std::abs (a - b) <= tolerance;
}

String db (double v)
{
    if (v <= kSilenceDb + 0.01)
        return "-inf dB";
    return (v > 0.0 ? "+" : "") + String (v, 1) + " dB";
}

String panText (double p)
{
    const int v = roundToInt (p * 50.0);
    return v == 0 ? String ("C") : String (std::abs (v)) + (v < 0 ? "L" : "R");
}

String hz (double f)
{
    return f >= 1000.0 ? String (f / 1000.0, f >= 10000.0 ? 1 : 2) + " kHz" : String (roundToInt (f)) + " Hz";
}

String quoted (const String& s)
{
    return "\"" + s + "\"";
}

String describeOutput (const var& o, const std::map<int, String>& returnNames)
{
    const auto kind = o["kind"].toString();
    if (kind == "return")
    {
        auto it = returnNames.find ((int) o["return"]);
        return it != returnNames.end() ? it->second : String ("a return");
    }
    if (kind == "hardware")
        return "Out " + String ((int) o["channel"] + 1) + ((bool) o["stereo"] ? "/" + String ((int) o["channel"] + 2) : String());
    return "Master";
}

void diffEq (const var& a, const var& b, StringArray& changes)
{
    if (same (a, b))
        return;
    if ((bool) a["on"] != (bool) b["on"])
        changes.add (String ("EQ ") + ((bool) b["on"] ? "on" : "off"));

    const auto& ba = arrayOf (a["bands"]);
    const auto& bb = arrayOf (b["bands"]);
    for (int i = 0; i < bb.size(); ++i)
    {
        const var before = i < ba.size() ? ba[i] : var();
        const var& after = bb.getReference (i);
        if (same (before, after))
            continue;

        const auto name = ChannelEq::bandName (i);
        const bool isCut = i == 0 || i == ChannelEq::numBands - 1;
        if ((bool) before["on"] != (bool) after["on"])
        {
            changes.add ("EQ " + name + ((bool) after["on"] ? " on at " + hz ((double) after["freq"]) : String (" off")));
            continue;
        }
        String c = "EQ " + name + " " + hz ((double) after["freq"]);
        if (! isCut)
            c << " " << db ((double) after["gain"]);
        if (! near ((double) before["gain"], (double) after["gain"]) && ! isCut)
            c << " (was " << db ((double) before["gain"]) << ")";
        else if (! near ((double) before["freq"], (double) after["freq"], 0.5))
            c << " (was " << hz ((double) before["freq"]) << ")";
        else if (! near ((double) before["q"], (double) after["q"]))
            c << ", width Q " << String ((double) after["q"], 2);
        changes.add (c);
    }
}

void diffPluginParams (const String& name, const var& a, const var& b, StringArray& changes)
{
    StringArray moved;
    if (auto* objB = b.getDynamicObject())
    {
        for (auto& p : objB->getProperties())
        {
            const auto before = a[p.name].toString();
            const auto after = p.value.toString();
            if (before != after)
                moved.add (p.name.toString() + " " + (before.isNotEmpty() ? before + " -> " : String()) + after);
        }
    }
    if (moved.isEmpty())
    {
        changes.add (name + " settings changed");
        return;
    }
    String c = name + ": " + moved.joinIntoString (", ", 0, 4);
    if (moved.size() > 4)
        c << " (+" << (moved.size() - 4) << " more)";
    changes.add (c);
}

void diffPlugins (const var& a, const var& b, StringArray& changes)
{
    const auto& pa = arrayOf (a);
    const auto& pb = arrayOf (b);

    auto nameOf = [] (const var& p)
    {
        if (auto xml = parseXML (p["description"].toString()))
            if (auto n = xml->getStringAttribute ("name"); n.isNotEmpty())
                return n;
        return String ("Plug-in");
    };

    StringArray namesA, namesB;
    for (auto& p : pa) namesA.add (nameOf (p));
    for (auto& p : pb) namesB.add (nameOf (p));

    if (namesA == namesB)
    {
        for (int i = 0; i < pb.size(); ++i)
        {
            const auto& x = pa.getReference (i);
            const auto& y = pb.getReference (i);
            if ((bool) x["bypassed"] != (bool) y["bypassed"])
                changes.add (namesB[i] + ((bool) y["bypassed"] ? " bypassed" : " back on"));
            if (x["state"].toString() != y["state"].toString())
                diffPluginParams (namesB[i], x["params"], y["params"], changes);
        }
        return;
    }

    StringArray added (namesB), removed (namesA);
    for (auto& n : namesA) { const int i = added.indexOf (n); if (i >= 0) added.remove (i); }
    for (auto& n : namesB) { const int i = removed.indexOf (n); if (i >= 0) removed.remove (i); }
    for (auto& n : added)   changes.add ("added " + n);
    for (auto& n : removed) changes.add ("removed " + n);
    if (added.isEmpty() && removed.isEmpty())
        changes.add ("plug-in order changed");
}

/** Two clips are the same if everything but where their audio lives matches. Audio is
    compared by hash when both know it, else by file name (each machine keeps its own copy). */
bool sameClip (const var& x, const var& y)
{
    auto normalised = [] (const var& c)
    {
        var copy = JSON::parse (JSON::toString (c, true));
        if (auto* o = copy.getDynamicObject())
        {
            o->removeProperty ("hash");
            o->setProperty ("file", c["file"].toString().replaceCharacter ('\\', '/').fromLastOccurrenceOf ("/", false, false));
        }
        return copy;
    };
    const auto hx = x["hash"].toString(), hy = y["hash"].toString();
    if (hx.isNotEmpty() && hy.isNotEmpty() && hx != hy)
        return false;
    return same (normalised (x), normalised (y));
}

void diffClips (const var& a, const var& b, StringArray& changes)
{
    std::map<int, var> before, after;
    for (auto& c : arrayOf (a)) before[(int) c["id"]] = c;
    for (auto& c : arrayOf (b)) after[(int) c["id"]] = c;

    int added = 0, removed = 0, edited = 0;
    for (auto& [id, c] : after)
    {
        auto it = before.find (id);
        if (it == before.end())                 ++added;
        else if (! sameClip (it->second, c))    ++edited;
    }
    for (auto& [id, c] : before)
        if (after.count (id) == 0)
            ++removed;

    auto plural = [] (int n, const char* word) { return String (n) + " " + word + (n == 1 ? "" : "s"); };
    if (added > 0)   changes.add (plural (added, "new clip"));
    if (removed > 0) changes.add (plural (removed, "clip") + " removed");
    if (edited > 0)  changes.add (plural (edited, "clip") + " edited");
}

void diffSends (const var& a, const var& b, const std::map<int, String>& returnNames, StringArray& changes)
{
    std::map<int, var> before, after;
    for (auto& s : arrayOf (a)) before[(int) s["return"]] = s;
    for (auto& s : arrayOf (b)) after[(int) s["return"]] = s;

    for (auto& [id, s] : after)
    {
        const double levelBefore = before.count (id) ? (double) before[id]["level"] : (double) kSilenceDb;
        const double levelAfter = (double) s["level"];
        if (! near (levelBefore, levelAfter))
        {
            auto it = returnNames.find (id);
            changes.add ("send to " + (it != returnNames.end() ? it->second : String ("return")) + " "
                         + db (levelBefore) + " -> " + db (levelAfter));
        }
    }
}

/** Every channel (track, return, master) keyed by id, plus what kind it is. */
struct Channel { var data; String role; };

std::map<int, Channel> channelsOf (const var& project)
{
    std::map<int, Channel> out;
    for (auto& t : arrayOf (project["tracks"]))  out[(int) t["id"]] = { t, "track" };
    for (auto& t : arrayOf (project["returns"])) out[(int) t["id"]] = { t, "return" };
    if (project["master"].isObject())
        out[(int) project["master"]["id"]] = { project["master"], "master" };
    return out;
}

StringArray channelChanges (const var& a, const var& b, const std::map<int, String>& returnNames)
{
    StringArray c;
    if (a["name"].toString() != b["name"].toString())
        c.add ("renamed from " + quoted (a["name"].toString()));
    if (! near ((double) a["volume"], (double) b["volume"]))
        c.add ("fader " + db ((double) a["volume"]) + " -> " + db ((double) b["volume"]));
    if (! near ((double) a["pan"], (double) b["pan"]))
        c.add ("pan " + panText ((double) a["pan"]) + " -> " + panText ((double) b["pan"]));
    if ((bool) a["mute"] != (bool) b["mute"])
        c.add ((bool) b["mute"] ? "muted" : "unmuted");
    if ((bool) a["solo"] != (bool) b["solo"])
        c.add ((bool) b["solo"] ? "soloed" : "unsoloed");
    if (! same (a["input"], b["input"]))
        c.add ("input changed");
    if (! same (a["output"], b["output"]))
        c.add ("output -> " + describeOutput (b["output"], returnNames));
    if (a["midiChannel"] != b["midiChannel"] || a["midiOutput"] != b["midiOutput"] || a["midiInput"] != b["midiInput"])
        c.add ("MIDI routing changed");
    if (a["group"] != b["group"])
        c.add (b.hasProperty ("group") ? "moved to a group" : "left its group");
    if (a["colour"] != b["colour"])
        c.add ("new colour");
    diffSends (a["sends"], b["sends"], returnNames, c);
    diffEq (a["eq"], b["eq"], c);
    diffPlugins (a["plugins"], b["plugins"], c);
    diffClips (a["clips"], b["clips"], c);
    return c;
}
} // namespace

StringArray describeChanges (const var& before, const var& after)
{
    StringArray lines;
    if (! before.isObject())
    {
        lines.add ("First version: " + String (arrayOf (after["tracks"]).size()) + " tracks, "
                   + String (arrayOf (after["returns"]).size()) + " returns");
        return lines;
    }

    // --- song settings
    if (! near ((double) before["tempo"], (double) after["tempo"]))
        lines.add ("Tempo " + String ((double) before["tempo"], 1) + " -> " + String ((double) after["tempo"], 1) + " BPM");
    if (! same (before["timeSignature"], after["timeSignature"]))
    {
        const auto& sig = arrayOf (after["timeSignature"]);
        if (sig.size() >= 2)
            lines.add ("Time signature -> " + String ((int) sig[0]) + "/" + String ((int) sig[1]));
        else
            lines.add ("Time signature changed");
    }
    if (! same (before["loop"], after["loop"]))
        lines.add ((bool) after["loop"]["enabled"] != (bool) before["loop"]["enabled"]
                       ? String ("Loop ") + ((bool) after["loop"]["enabled"] ? "on" : "off")
                       : String ("Loop region moved"));

    std::map<int, String> returnNames;
    for (auto& r : arrayOf (after["returns"]))  returnNames[(int) r["id"]] = r["name"].toString();
    for (auto& r : arrayOf (before["returns"])) returnNames.insert ({ (int) r["id"], r["name"].toString() });

    // --- channels, in the new project's order, then removed ones
    const auto a = channelsOf (before), b = channelsOf (after);
    auto label = [] (const Channel& ch)
    {
        if (ch.role == "master") return String ("Master");
        return quoted (ch.data["name"].toString()) + (ch.role == "return" ? " (return)" : String());
    };

    std::vector<int> order;
    for (auto& t : arrayOf (after["tracks"]))  order.push_back ((int) t["id"]);
    for (auto& t : arrayOf (after["returns"])) order.push_back ((int) t["id"]);
    if (after["master"].isObject())            order.push_back ((int) after["master"]["id"]);

    for (int id : order)
    {
        const auto& ch = b.at (id);
        auto it = a.find (id);
        if (it == a.end())
        {
            String kind = ch.role == "return" ? "return" : ch.data["kind"].toString() + " track";
            lines.add ("+ " + quoted (ch.data["name"].toString()) + " (new " + kind + ")");
            continue;
        }
        const auto changes = channelChanges (it->second.data, ch.data, returnNames);
        if (! changes.isEmpty())
            lines.add (label (ch) + ": " + changes.joinIntoString ("; "));
    }
    for (auto& [id, ch] : a)
        if (b.count (id) == 0)
            lines.add ("- " + label (ch) + " removed");

    // --- order, groups, monitor mixes
    auto ids = [] (const var& list)
    {
        Array<int> out;
        for (auto& t : arrayOf (list))
            out.add ((int) t["id"]);
        return out;
    };
    auto common = [] (Array<int> x, const Array<int>& keep)
    {
        x.removeIf ([&] (int v) { return ! keep.contains (v); });
        return x;
    };
    const auto oldOrder = ids (before["tracks"]), newOrder = ids (after["tracks"]);
    if (common (oldOrder, newOrder) != common (newOrder, oldOrder))
        lines.add ("Track order changed");

    if (! same (before["groups"], after["groups"]))
        lines.add ("Groups changed");

    std::map<int, var> cuesA, cuesB;
    for (auto& c : arrayOf (before["cues"])) cuesA[(int) c["id"]] = c;
    for (auto& c : arrayOf (after["cues"]))  cuesB[(int) c["id"]] = c;
    for (auto& [id, c] : cuesB)
    {
        auto it = cuesA.find (id);
        if (it == cuesA.end())                  lines.add ("+ Monitor mix " + quoted (c["name"].toString()));
        else if (! same (it->second, c))        lines.add ("Monitor mix " + quoted (c["name"].toString()) + " changed");
    }
    for (auto& [id, c] : cuesA)
        if (cuesB.count (id) == 0)
            lines.add ("- Monitor mix " + quoted (c["name"].toString()) + " removed");

    return lines;
}

String summariseChanges (const var& before, const var& after)
{
    const auto lines = describeChanges (before, after);
    return lines.isEmpty() ? String ("No changes") : lines.joinIntoString ("\n");
}
} // namespace lathe::cloud
