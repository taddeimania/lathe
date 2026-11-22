#pragma once

#include "../Common.h"

#include <optional>

namespace lathe
{
//==============================================================================
// Keys: the song's key lane. Each key change says which key the song is in from a beat on,
// until the next one; before the first, the song has no key. The Play Along window lights up
// the notes of the key at the playhead, and the assistant can set the lane over MCP.
//==============================================================================

enum class Scale
{
    major, minor, majorPentatonic, minorPentatonic, blues, dorian, mixolydian, harmonicMinor
};

struct KeyChange
{
    double beat = 0.0;            // where it starts
    int root = 9;                 // pitch class, 0 = C ... 11 = B
    Scale scale = Scale::minor;

    bool operator== (const KeyChange& o) const noexcept
    {
        return exactlyEqual (beat, o.beat) && root == o.root && scale == o.scale;
    }
    bool operator!= (const KeyChange& o) const noexcept   { return ! operator== (o); }
};

namespace keys
{
    /** Every scale, in the order the key lane's menu lists them. */
    const std::vector<Scale>& allScales();
    /** The notes of a scale as semitones above its root, starting with 0. */
    const std::vector<int>& intervals (Scale);
    /** "minor pentatonic" */
    String scaleName (Scale);

    /** Whether the key is spelled with flats (F major, D minor, Bb...) rather than sharps. */
    bool usesFlats (int root, Scale);
    /** A pitch class's name as the key spells it: "Bb" in F major, "A#" in B major. */
    String noteName (int pitchClass, bool flats);
    /** "A minor pentatonic" */
    String keyName (int root, Scale);
    inline String keyName (const KeyChange& k)   { return keyName (k.root, k.scale); }
    /** "Am pent" (for the key lane's narrow blocks). */
    String shortName (int root, Scale);

    /** Reads a key name: "A minor pentatonic", "Am pentatonic", "Am pent", "F# blues", "Eb",
        "Bbm", "D dorian", "c harmonic minor". A root alone is major; "m" or "minor" after it is
        minor. Returns nothing if it isn't one. */
    std::optional<std::pair<int, Scale>> parse (const String&);

    /** Whether a MIDI pitch (any octave) is in the key. */
    bool contains (int root, Scale, int pitch);
    inline bool isRoot (int root, int pitch)   { return ((pitch % 12) + 12) % 12 == root; }

    /** The key change in effect at a beat (the last one at or before it), or null.
        `list` must be in order of beat, as Edit keeps it. */
    const KeyChange* at (const std::vector<KeyChange>& list, double beat);
    /** The next key change after a beat, or null. */
    const KeyChange* after (const std::vector<KeyChange>& list, double beat);

    /** Puts a list in order of beat, keeping the last of any that share a beat. */
    void tidy (std::vector<KeyChange>& list);

    /** One key change as the project file saves it: { "beat": 64, "key": "C major" }. */
    var toVar (const KeyChange&);
    std::optional<KeyChange> fromVar (const var&);
} // namespace keys

//==============================================================================
// Where an instrument plays each note, for the Play Along window (and its tests).
namespace fretboard
{
    struct Instrument
    {
        String name;
        std::vector<int> strings;   // open-string MIDI pitches, from the string a player holds nearest their face
                                    // (guitar low E, ukulele G); empty for a keyboard
        int frets = 0;
    };
    /** Guitar, bass, ukulele and keyboard, in standard tuning. */
    const std::vector<Instrument>& instruments();

    /** One position the window draws: a string and fret (or, on a keyboard, string -1 and the
        MIDI pitch in `fret`), and whether it's in the key and the root. */
    struct Spot
    {
        int string = 0, fret = 0, pitch = 0;
        bool inKey = false, root = false;
    };
    /** Every position on the instrument, marked for the key. Keyboard: the two octaves from C3. */
    std::vector<Spot> spots (const Instrument&, int root, Scale);

    constexpr int keyboardLow = 48, keyboardHigh = 71;   // C3 to B4
} // namespace fretboard
} // namespace lathe
