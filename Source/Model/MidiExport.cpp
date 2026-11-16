#include "MidiExport.h"

namespace lathe::midiexport
{
namespace
{
constexpr int kTicksPerQuarter = 960;

/** The notes a clip plays: starting inside it, cut at its end (as playback does). */
template <typename Fn>
void forEachPlayedNote (const Clip& c, Fn&& fn)
{
    for (auto& n : c.notes)
        if (n.start >= 0.0 && n.start < c.length)
            fn (n, jmin (n.start + n.length, c.length));
}

struct Timed
{
    int64 tick;
    int order;   // at the same tick: note-offs, then controllers, then note-ons (so repeats retrigger)
    MidiMessage message;
};
} // namespace

std::vector<Item> songItems (const Edit& edit)
{
    std::vector<Item> out;
    for (auto& t : edit.tracks)
    {
        if (! t->isMidi())
            continue;
        Item item { t->name, jlimit (1, 16, t->midiChannel), {} };
        for (auto& c : t->clips)
            if (c.kind == ClipKind::midi)
                item.clips.push_back (c);
        if (countNotes ({ item }) > 0)
            out.push_back (std::move (item));
    }
    return out;
}

int countNotes (const std::vector<Item>& items)
{
    int n = 0;
    for (auto& item : items)
        for (auto& c : item.clips)
            forEachPlayedNote (c, [&] (const Note&, double) { ++n; });
    return n;
}

MidiFile build (const Edit& edit, const std::vector<Item>& items, double fromBeat, const String& title)
{
    MidiFile file;
    file.setTicksPerQuarterNote (kTicksPerQuarter);
    auto toTick = [&] (double songBeat) { return (int64) std::llround ((songBeat - fromBeat) * kTicksPerQuarter); };

    {
        MidiMessageSequence conductor;
        if (title.isNotEmpty())
            conductor.addEvent (MidiMessage::textMetaEvent (3, title), 0);
        conductor.addEvent (MidiMessage::tempoMetaEvent ((int) std::llround (60000000.0 / jmax (1.0, edit.tempo))), 0);
        conductor.addEvent (MidiMessage::timeSignatureMetaEvent (edit.timeSigNum, edit.timeSigDen), 0);
        conductor.addEvent (MidiMessage::endOfTrack(), 0);
        file.addTrack (conductor);
    }

    for (auto& item : items)
    {
        const int ch = jlimit (1, 16, item.channel);
        std::vector<Timed> timed;
        int64 last = 0;
        for (auto& c : item.clips)
        {
            if (c.kind != ClipKind::midi)
                continue;
            forEachPlayedNote (c, [&] (const Note& n, double end)
            {
                const auto on = toTick (c.start + n.start);
                if (on < 0)
                    return;
                const auto off = jmax (on + 1, toTick (c.start + end));
                const auto vel = (uint8) jlimit (1, 127, (int) std::lround (n.velocity * 127.0f));
                timed.push_back ({ on, 2, MidiMessage::noteOn (ch, n.pitch, vel) });
                timed.push_back ({ off, 0, MidiMessage::noteOff (ch, n.pitch) });
                last = jmax (last, off);
            });
            for (auto& e : c.events)
            {
                if (e.beat < 0.0 || e.beat >= c.length || e.size <= 0)
                    continue;
                const auto tick = toTick (c.start + e.beat);
                if (tick < 0)
                    continue;
                MidiMessage m (e.data, e.size);
                if (m.getChannel() > 0)
                    m.setChannel (ch);   // the track's channel, as playback sends it
                timed.push_back ({ tick, 1, m });
                last = jmax (last, tick);
            }
        }
        std::stable_sort (timed.begin(), timed.end(), [] (const Timed& a, const Timed& b)
        {
            return a.tick != b.tick ? a.tick < b.tick : a.order < b.order;
        });

        MidiMessageSequence seq;
        seq.addEvent (MidiMessage::textMetaEvent (3, item.name), 0);
        for (auto& t : timed)
            seq.addEvent (t.message, (double) t.tick);
        seq.addEvent (MidiMessage::endOfTrack(), (double) last);
        file.addTrack (seq);
    }
    return file;
}

String write (const MidiFile& midi, const File& file)
{
    if (file.getParentDirectory().createDirectory().failed())
        return "Couldn't make the folder " + file.getParentDirectory().getFullPathName() + ".";
    const auto temp = file.getSiblingFile (file.getFileName() + ".writing");
    {
        FileOutputStream out (temp);
        if (! out.openedOk())
            return "Couldn't write to " + file.getFullPathName() + ".";
        out.setPosition (0);
        out.truncate();
        if (! midi.writeTo (out, 1))
        {
            out.flush();
            temp.deleteFile();
            return "Couldn't write the MIDI file.";
        }
        out.flush();
        if (out.getStatus().failed())
            return "Couldn't write to " + file.getFullPathName() + ": " + out.getStatus().getErrorMessage();
    }
    if (! temp.moveFileTo (file))
    {
        temp.deleteFile();
        return "Couldn't write to " + file.getFullPathName() + ".";
    }
    return {};
}

String fileNameFor (const String& name)
{
    auto s = File::createLegalFileName (name.replace ("/", "-").replace ("\\", "-").replace (":", "-"));
    while (s.contains ("  "))
        s = s.replace ("  ", " ");
    s = s.trim();
    return s.isEmpty() ? String ("MIDI") : s;
}
} // namespace lathe::midiexport
