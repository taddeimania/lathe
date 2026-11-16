#include "MidiImport.h"

#include <map>

namespace lathe::midiimport
{
namespace
{
constexpr double kShortestNote = 1.0 / 64.0;   // beats
constexpr double kUnendedNote = 0.25;          // a note-on with no note-off

String partName (const String& trackName, const String& fallback, int channel, bool severalChannels, int index, int count)
{
    String base = trackName.isNotEmpty() ? trackName : fallback;
    if (severalChannels)
        return base + (channel == 10 ? String (" drums") : " ch " + String (channel));
    if (trackName.isEmpty() && count > 1)
        return base + " " + String (index + 1);
    return base;
}
} // namespace

Contents read (const File& file)
{
    FileInputStream in (file);
    if (! in.openedOk())
    {
        Contents c;
        c.error = "Couldn't open " + file.getFileName() + ".";
        return c;
    }
    return read (in, file.getFileNameWithoutExtension());
}

Contents read (InputStream& in, const String& fallbackName)
{
    Contents out;
    MidiFile mf;
    int format = 0;
    if (! mf.readFrom (in, true, &format))
    {
        out.error = "That isn't a MIDI file Lathe can read.";
        return out;
    }

    const short timeFormat = mf.getTimeFormat();
    const bool smpte = timeFormat <= 0;

    // The first tempo and time signature, if the file gives them.
    {
        MidiMessageSequence tempos, sigs;
        mf.findAllTempoEvents (tempos);
        mf.findAllTimeSigEvents (sigs);
        if (tempos.getNumEvents() > 0)
        {
            const double secsPerQuarter = tempos.getEventPointer (0)->message.getTempoSecondsPerQuarterNote();
            if (secsPerQuarter > 0.0)
                out.tempo = 60.0 / secsPerQuarter;
        }
        if (sigs.getNumEvents() > 0)
            sigs.getEventPointer (0)->message.getTimeSignatureInfo (out.timeSigNum, out.timeSigDen);
    }

    // Ticks to beats. SMPTE files count in seconds: at the file's tempo (or 120) that's beats too.
    if (smpte)
        mf.convertTimestampTicksToSeconds();
    const double secondsToBeats = (out.tempo > 0.0 ? out.tempo : 120.0) / 60.0;
    auto beats = [&] (double t) { return smpte ? t * secondsToBeats : t / (double) timeFormat; };

    struct Draft { String trackName; int channel; Part part; };
    std::vector<Draft> drafts;

    for (int ti = 0; ti < mf.getNumTracks(); ++ti)
    {
        MidiMessageSequence seq (*mf.getTrack (ti));
        seq.updateMatchedPairs();

        String trackName;
        std::map<int, Part> byChannel;
        for (int i = 0; i < seq.getNumEvents(); ++i)
        {
            const auto* e = seq.getEventPointer (i);
            const auto& m = e->message;
            if (m.isTrackNameEvent() && trackName.isEmpty())
                trackName = m.getTextFromTextMetaEvent().trim();

            if (m.isNoteOn())
            {
                auto& p = byChannel[m.getChannel()];
                Note n;
                n.pitch = m.getNoteNumber();
                n.start = jmax (0.0, beats (m.getTimeStamp()));
                n.length = e->noteOffObject != nullptr
                               ? jmax (kShortestNote, beats (e->noteOffObject->message.getTimeStamp()) - n.start)
                               : kUnendedNote;
                n.velocity = jlimit (0.0f, 1.0f, m.getFloatVelocity());
                p.notes.push_back (n);
                p.end = jmax (p.end, n.start + n.length);
            }
            else if (m.isController() || m.isPitchWheel() || m.isAftertouch() || m.isChannelPressure())
            {
                auto& p = byChannel[m.getChannel()];
                MidiEvent ev;
                ev.beat = jmax (0.0, beats (m.getTimeStamp()));
                ev.size = jmin (3, m.getRawDataSize());
                std::memcpy (ev.data, m.getRawData(), (size_t) ev.size);
                p.events.push_back (ev);
            }
        }

        int withNotes = 0;
        for (auto& [ch, p] : byChannel)
            if (! p.notes.empty())
                ++withNotes;
        for (auto& [ch, p] : byChannel)
        {
            if (p.notes.empty())
                continue;   // controllers alone (a conductor track, say) make no part
            p.channel = ch;
            drafts.push_back ({ trackName, withNotes > 1 ? -ch : ch, std::move (p) });
        }
    }

    for (size_t i = 0; i < drafts.size(); ++i)
    {
        auto& d = drafts[i];
        const bool severalChannels = d.channel < 0;
        d.part.name = partName (d.trackName, fallbackName, d.part.channel, severalChannels, (int) i, (int) drafts.size());
        auto byStart = [] (const auto& a, const auto& b) { return a.start < b.start; };
        std::stable_sort (d.part.notes.begin(), d.part.notes.end(), byStart);
        std::stable_sort (d.part.events.begin(), d.part.events.end(), [] (auto& a, auto& b) { return a.beat < b.beat; });
        out.parts.push_back (std::move (d.part));
    }

    if (out.parts.empty())
        out.error = "That MIDI file has no notes in it.";
    return out;
}

Part merged (const std::vector<Part>& parts, const String& name)
{
    Part m;
    m.name = name;
    for (auto& p : parts)
    {
        m.notes.insert (m.notes.end(), p.notes.begin(), p.notes.end());
        m.events.insert (m.events.end(), p.events.begin(), p.events.end());
        m.end = jmax (m.end, p.end);
    }
    if (! parts.empty())
        m.channel = parts.front().channel;
    std::stable_sort (m.notes.begin(), m.notes.end(), [] (auto& a, auto& b) { return a.start < b.start; });
    std::stable_sort (m.events.begin(), m.events.end(), [] (auto& a, auto& b) { return a.beat < b.beat; });
    return m;
}

std::vector<std::shared_ptr<Track>> place (Edit& edit, const Contents& contents, std::shared_ptr<Track> onto, double atBeat,
                                           const String& fileName, bool oneTrack)
{
    std::vector<std::shared_ptr<Track>> used;
    if (contents.parts.empty())
        return used;

    std::vector<Part> parts;
    if (onto != nullptr || oneTrack)
        parts.push_back (merged (contents.parts, fileName));
    else
        parts = contents.parts;

    const double bar = edit.beatsPerBar();
    for (auto& p : parts)
    {
        auto track = onto;
        if (track == nullptr)
            track = edit.addTrack (TrackKind::midi, p.name);

        Clip c;
        c.id = edit.newId();
        c.kind = ClipKind::midi;
        c.name = p.name;
        c.start = jmax (0.0, atBeat);
        c.length = jmax (1.0, std::ceil (p.end / bar - 1.0e-6)) * bar;
        for (auto n : p.notes)
            if (n.start < c.length)
            {
                n.length = jmin (n.length, c.length - n.start);
                c.notes.push_back (n);
            }
        for (auto& ev : p.events)
            if (ev.beat < c.length)
                c.events.push_back (ev);

        edit.clearRange (*track, c.start, c.end());
        track->clips.push_back (std::move (c));
        used.push_back (track);
    }
    return used;
}
} // namespace lathe::midiimport
