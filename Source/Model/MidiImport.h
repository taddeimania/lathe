#pragma once

#include "Edit.h"

namespace lathe::midiimport
{
/** One part of a MIDI file: the notes of one track (or one channel of a single-track file),
    in beats from the start of the file. */
struct Part
{
    String name;
    int channel = 1;
    std::vector<Note> notes;
    std::vector<MidiEvent> events;   // CC, pitch bend, aftertouch, beats from the file's start
    double end = 0.0;                // the last note's end, in beats
};

struct Contents
{
    std::vector<Part> parts;
    double tempo = 0.0;          // the file's first tempo (0 = it doesn't say)
    int timeSigNum = 0, timeSigDen = 0;
    String error;                // set when the file couldn't be read or holds no notes
};

/** Reads a Standard MIDI File (.mid). Splits it by track, and a track that plays on several
    channels by channel; parts with no notes are left out. `fallbackName` names unnamed parts. */
Contents read (const File& file);
Contents read (InputStream& in, const String& fallbackName);

/** All parts as one (a drum groove spread over several tracks plays as one clip). */
Part merged (const std::vector<Part>& parts, const String& name);

/** Puts parts into the song as MIDI clips starting at `atBeat`, each clip running to the end of
    the bar its last note ends in. With `onto` (a MIDI track), every part goes onto it as one clip
    (anything there in that time is cleared); otherwise each part gets a new MIDI track named
    after it, or with `oneTrack` they all go on one new track named `fileName`. Doesn't call
    Edit::changed(). Returns the tracks the clips went on, in order. */
std::vector<std::shared_ptr<Track>> place (Edit&, const Contents&, std::shared_ptr<Track> onto, double atBeat,
                                           const String& fileName, bool oneTrack = false);
} // namespace lathe::midiimport
