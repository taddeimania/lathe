#pragma once

#include "Edit.h"

/** Convert to MIDI: the notes in a recording. Two kinds of part:

    - a melody (a voice, a bass, a lead: one note at a time). The pitch is read along the sound
      with the Tuner's YIN detector; a note starts where the sound starts, where the pitch moves to
      another note, or where it's played again; quiet and very short stretches are left out.
    - drums. Each hit is found where the sound jumps up, and sorted by where its energy is: low
      (kick), middle (snare) or high (hi-hat), or all made one note (`oneDrum`, for a track of
      just a kick or just a snare).

    Velocity is how loud each note is against the loudest. Times are in seconds from the start of
    the sound; the caller puts them in beats. Any thread. */
namespace lathe::audiotomidi
{
enum class Kind { melody, drums, oneDrum };

/** General MIDI's drum notes, which drum plug-ins (EZdrummer, Addictive Drums, ...) follow. */
constexpr int kick = 36, snare = 38, hihat = 42;

struct Found
{
    int pitch = 60;
    double start = 0.0, length = 0.1;   // seconds
    float velocity = 0.8f;
};

/** `progress` gets 0..1 and returns false to stop (the result is then empty). */
using Progress = std::function<bool (double)>;

std::vector<Found> melody (const float* mono, int numSamples, double sampleRate, const Progress& = {});
std::vector<Found> drums (const float* mono, int numSamples, double sampleRate, bool oneDrum, const Progress& = {});

struct Result
{
    std::vector<Note> notes;   // beats from the clip's start
    String error;              // the file couldn't be read, or there was no sound
    bool cancelled = false;
};

/** Reads what an audio clip plays (its stretch of its file, as mono) and finds its notes.
    `tempo` is the song's, to put the notes in beats. */
Result convertClip (const Clip&, Kind, double tempo, AudioFormatManager&, const Progress& = {});

/** Puts the notes found in some of `source`'s clips on a new MIDI track right under it, named
    "<source> MIDI": a clip for each, at the same place and length, with the same name. Doesn't
    call Edit::changed(). Returns the new track. */
std::shared_ptr<Track> place (Edit&, const Track& source, const std::vector<std::pair<Clip, std::vector<Note>>>& converted);

/** "melody", "drums", "one_drum" and back (what the assistant's tool and the menu use). */
String kindName (Kind);
bool parseKind (const String&, Kind&);
} // namespace lathe::audiotomidi
