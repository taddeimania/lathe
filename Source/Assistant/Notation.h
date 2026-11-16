#pragma once
#include <juce_core/juce_core.h>
#include <vector>

/** Lathe's note notation (docs/notation.md): a short text form of MIDI parts for the AI assistant's
    write_notes and get_notes. Drum-machine step grids, Strudel-style token lines, and optional
    velocity, microtiming and length on any note. No dependence on the engine or the project. */
namespace lathe::notation
{
struct Note
{
    int pitch = 60;
    double start = 0.0;    // beats from the start of the range
    double length = 0.25;  // beats
    int velocity = 100;    // 1-127
};

struct Context
{
    double beatsPerBar = 4.0;   // quarter-note beats in a bar (4/4: 4, 7/8: 3.5)
    double tempo = 120.0;       // BPM, for +ms / -ms
    double spanBeats = 16.0;    // the range the pattern fills (lines loop to fill it)
};

/** One line of a pattern: how long it is and how many times it plays to fill the range. */
struct LineSummary
{
    juce::String label;     // the line's label, or "line N" when it has none
    int bars = 1;           // the line's own length
    double plays = 1.0;     // range / line length (2.5: the last time round is cut short)
};

/** Text to notes, starts in [0, spanBeats). On failure, out is left empty and the Result says
    which line, bar and step went wrong. lines, if given, gets one summary per line of notes.
    warnings, if given, gets what parsed but probably isn't what was meant (a swing line that
    moved no notes). */
juce::Result parse (const juce::String& text, const Context&, std::vector<Note>& out,
                    std::vector<LineSummary>* lines = nullptr, juce::StringArray* warnings = nullptr);

/** The lines that looped, in words: "kick and hat: 1 bar, played 16 times; bass: 4 bars, played
    2.5 times". Lines heard once are left out; "" if none looped. */
juce::String describeLoops (const std::vector<LineSummary>&);

/** Notes (starts in [0, spanBeats)) to text that parse() turns back into the same notes. drums:
    one lane per pitch, grid lines where they fit, drum names where there are any. */
juce::String format (std::vector<Note> notes, const Context&, bool drums);

/** The full reference, for the notation_guide tool. */
juce::String guide();

/** "kick" -> 36 (and the short forms); -1 if it isn't a drum name. */
int drumPitch (const juce::String& name);
/** 36 -> "kick"; "" if the note has no drum name. */
juce::String drumName (int pitch);
/** "C3" -> 60, "F#2", "Bb-1", "60" (C3 = middle C). -1 if it isn't a note. */
int parsePitchName (const juce::String& s);
/** 60 -> "C3". */
juce::String pitchName (int pitch);
} // namespace lathe::notation
