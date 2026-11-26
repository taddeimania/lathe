#pragma once

#include "Keys.h"

#include <array>

namespace lathe
{
//==============================================================================
// Chords: the chords that belong to a key (I, ii, iii... with their 7ths, sus and power chords),
// and where to play them on a guitar in standard tuning (open chords, barre shapes up the neck,
// triads on the top strings). The Play Along window's chord sheet draws these; nothing here
// needs a window, so the tests check it directly.
//==============================================================================
namespace chords
{
    enum class Quality
    {
        major, minor, diminished, augmented,
        dominant7, major7, minor7, halfDiminished7, diminished7, minorMajor7, augmentedMajor7,
        sus2, sus4, power
    };

    /** The chord's notes as semitones above its root, root first: minor7 is 0 3 7 10. */
    const std::vector<int>& intervals (Quality);
    /** What follows the root in a chord's name: "m7", "dim", "maj7", "sus4", "5"; "" for major. */
    String suffix (Quality);
    /** A chord tone's name in the chord: "R", "3", "b3", "5", "b5", "#5", "7", "b7", "bb7", "2", "4". */
    String toneLabel (Quality, int semitonesAboveRoot);

    /** What kind of variant a chord is on its degree, so the sheet can keep "the 7th" picked
        across a key change. */
    enum class Variant { triad, seventh, dominant7, sus2, sus4, power };
    String variantName (Variant);   // "Triad", "7th", "Dom 7", "sus2", "sus4", "Power"

    struct Chord
    {
        int root = 0;                    // pitch class
        Quality quality = Quality::major;
        Variant variant = Variant::triad;
        String name;                     // "F#m7", "Bb", "C#dim", spelled as the key spells it
        String numeral;                  // "ii7", "bVII", "vii°"
    };

    /** One step of the scale and its chords: chords[0] is the triad, then the 7th, sus chords
        and the power chord (only those whose notes are in the scale). */
    struct Degree
    {
        int degree = 0;                  // 0 = the tonic
        int root = 0;                    // pitch class
        String numeral;                  // of the triad: "I", "ii", "bIII", "vii°"
        std::vector<Chord> chords;
    };

    /** The key's chords, degree by degree. Seven-note scales stack thirds on each step; the
        pentatonic scales use their parent (major or minor); blues takes the minor chords and adds
        the dominant 7ths (I7, IV7, V7) a blues is played on. */
    std::vector<Degree> diatonic (int root, Scale);

    /** A chord's tones as pitch classes. */
    std::vector<int> tones (const Chord&);
    /** A chord's tones by name, root first, each spelled from the root's letter: A7 is A C# E G
        (not Db), Bdim7 is B D F Ab. `flats` breaks ties a letter can't settle. */
    std::vector<String> toneNames (const Chord&, bool flats);

    //==========================================================================
    /** The instruments a chord can be shown on, in the order fretboard::instruments() lists them. */
    enum class Instrument { guitar, bass, ukulele, keyboard };

    /** One way to play a chord on an instrument. */
    struct Voicing
    {
        Instrument instrument = Instrument::guitar;
        std::vector<int> frets;          // per string, as fretboard::instruments() lists them (guitar low E
                                         // first, ukulele G first): -1 not played, 0 open. Empty on a keyboard.
        std::vector<int> keys;           // on a keyboard: the MIDI notes, low to high
        enum Group { shape, upTheNeck }; // keyboard: close voicings, then spread ones
        Group group = shape;
        String name;                     // "Open", "E shape", "Triad on G B E", "1st inversion"
        String detail;                   // "Fret 5", "Root position, fret 8", "C4 E4 G4"
        int barreFret = 0;               // 0: no barre; else the fret the index finger lays across
        int barreFrom = -1, barreTo = -1; // the strings the barre covers

        int lowestFret() const;          // of the fretted notes (0 if every note is open)
        int highestFret() const;
        int bassString() const;          // the lowest string played
        bool usesOpenStrings() const;
        /** The MIDI pitches, low string first (or the keys). */
        std::vector<int> pitches() const;
    };

    /** Ways to play a chord, easiest first.
        Guitar: open chords, barre shapes by position (root in the bass), then triads (or
        four-note shapes for 7ths) on the top strings in each inversion, higher up the neck.
        Ukulele: all four strings, open chords first, then shapes up the neck.
        Bass: the root on the lowest string played, with the 3rd (and 7th) above it.
        Keyboard: close voicings in each inversion, then spread ones with the root an octave
        or more below, all within the two octaves Play Along draws. */
    std::vector<Voicing> voicings (const Chord&, Instrument = Instrument::guitar);

    constexpr int guitarFrets = 15;
} // namespace chords
} // namespace lathe
