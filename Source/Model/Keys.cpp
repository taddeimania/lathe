#include "Keys.h"

namespace lathe
{
namespace keys
{
namespace
{
    struct ScaleInfo
    {
        Scale scale;
        const char* name;
        const char* shortName;            // after the root: "m pent"
        std::vector<int> steps;
        int toRelativeMajor;              // semitones from the root to the major key with the same signature
    };

    const std::vector<ScaleInfo>& table()
    {
        static const std::vector<ScaleInfo> t {
            { Scale::major,           "major",             "",         { 0, 2, 4, 5, 7, 9, 11 }, 0 },
            { Scale::minor,           "minor",             "m",        { 0, 2, 3, 5, 7, 8, 10 }, 3 },
            { Scale::majorPentatonic, "major pentatonic",  " pent",    { 0, 2, 4, 7, 9 },        0 },
            { Scale::minorPentatonic, "minor pentatonic",  "m pent",   { 0, 3, 5, 7, 10 },       3 },
            { Scale::blues,           "blues",             " blues",   { 0, 3, 5, 6, 7, 10 },    3 },
            { Scale::dorian,          "dorian",            " dorian",  { 0, 2, 3, 5, 7, 9, 10 }, 10 },
            { Scale::mixolydian,      "mixolydian",        " mixo",    { 0, 2, 4, 5, 7, 9, 10 }, 5 },
            { Scale::harmonicMinor,   "harmonic minor",    "m harm",   { 0, 2, 3, 5, 7, 8, 11 }, 3 },
        };
        return t;
    }

    const ScaleInfo& info (Scale s)
    {
        for (auto& i : table())
            if (i.scale == s)
                return i;
        return table().front();
    }

    int pc (int pitch)   { return ((pitch % 12) + 12) % 12; }
}

const std::vector<Scale>& allScales()
{
    static const std::vector<Scale> all = []
    {
        std::vector<Scale> v;
        for (auto& i : table())
            v.push_back (i.scale);
        return v;
    }();
    return all;
}

const std::vector<int>& intervals (Scale s)   { return info (s).steps; }
String scaleName (Scale s)                    { return info (s).name; }

bool usesFlats (int root, Scale s)
{
    // The major keys whose signatures have flats: F, Bb, Eb, Ab, Db. (F#/Gb is spelled with sharps.)
    const int major = pc (root + info (s).toRelativeMajor);
    return major == 5 || major == 10 || major == 3 || major == 8 || major == 1;
}

String noteName (int pitchClass, bool flats)
{
    static const char* sharps[] { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    static const char* flatNames[] { "C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B" };
    return (flats ? flatNames : sharps)[pc (pitchClass)];
}

String keyName (int root, Scale s)    { return noteName (root, usesFlats (root, s)) + " " + scaleName (s); }
String shortName (int root, Scale s)  { return noteName (root, usesFlats (root, s)) + info (s).shortName; }

std::optional<std::pair<int, Scale>> parse (const String& text)
{
    auto t = text.trim();
    if (t.isEmpty())
        return {};

    // The root: a letter, then any sharps or flats.
    static const int letters[] { 9, 11, 0, 2, 4, 5, 7 };   // A to G
    const auto letter = CharacterFunctions::toUpperCase (t[0]);
    if (letter < 'A' || letter > 'G')
        return {};
    int root = letters[letter - 'A'];
    int i = 1;
    for (; i < t.length(); ++i)
    {
        const auto c = t[i];
        if (c == '#' || c == (juce_wchar) 0x266f)        { ++root; continue; }
        if (c == 'b' || c == (juce_wchar) 0x266d)
        {
            // "b" is a flat unless it starts "blues".
            if (t.substring (i).toLowerCase().startsWith ("blues"))
                break;
            --root;
            continue;
        }
        break;
    }
    root = pc (root);

    // What follows the root, as single-spaced lower-case words. A bare "m" (or "min") stuck to
    // the root means minor: "Am", "Am pent", "Amin blues" (but "A mixolydian" is a mode).
    auto words = StringArray::fromTokens (t.substring (i).toLowerCase().replace ("-", " ").replace ("_", " "), " ", "");
    words.removeEmptyStrings();
    bool minorMark = false;
    if (! words.isEmpty() && ! CharacterFunctions::isWhitespace (t[i]) && (words[0] == "m" || words[0] == "min"))
    {
        minorMark = true;
        words.remove (0);
    }
    if (! words.isEmpty() && words[words.size() - 1] == "scale")
        words.remove (words.size() - 1);
    const auto rest = words.joinIntoString (" ");

    const bool pent = rest.contains ("pent");
    if (rest.isEmpty())                                        return std::pair { root, minorMark ? Scale::minor : Scale::major };
    if (rest == "major" || rest == "maj" || rest == "ionian")  return std::pair { root, minorMark ? Scale::minor : Scale::major };
    if (rest == "minor" || rest == "aeolian" || rest == "natural minor")
                                                               return std::pair { root, Scale::minor };
    if (pent && (rest.contains ("minor") || minorMark))        return std::pair { root, Scale::minorPentatonic };
    if (pent)                                                  return std::pair { root, Scale::majorPentatonic };
    if (rest == "blues" || rest == "minor blues")              return std::pair { root, Scale::blues };
    if (rest == "dorian" || rest == "dor")                     return std::pair { root, Scale::dorian };
    if (rest == "mixolydian" || rest == "mixo" || rest == "mix")
                                                               return std::pair { root, Scale::mixolydian };
    if (rest == "harmonic minor" || rest == "harmonic" || rest == "harm" || rest == "harm minor")
                                                               return std::pair { root, Scale::harmonicMinor };
    return {};
}

bool contains (int root, Scale s, int pitch)
{
    const int step = pc (pitch - root);
    for (auto v : intervals (s))
        if (v == step)
            return true;
    return false;
}

const KeyChange* at (const std::vector<KeyChange>& list, double beat)
{
    const KeyChange* found = nullptr;
    for (auto& k : list)
    {
        if (k.beat > beat + 1.0e-9)
            break;
        found = &k;
    }
    return found;
}

const KeyChange* after (const std::vector<KeyChange>& list, double beat)
{
    for (auto& k : list)
        if (k.beat > beat + 1.0e-9)
            return &k;
    return nullptr;
}

void tidy (std::vector<KeyChange>& list)
{
    std::stable_sort (list.begin(), list.end(), [] (auto& a, auto& b) { return a.beat < b.beat; });
    std::vector<KeyChange> out;
    for (auto& k : list)
    {
        auto c = k;
        c.beat = jmax (0.0, c.beat);
        c.root = pc (c.root);
        if (! out.empty() && std::abs (out.back().beat - c.beat) < 1.0e-6)
            out.back() = c;
        else
            out.push_back (c);
    }
    list = std::move (out);
}

var toVar (const KeyChange& k)
{
    DynamicObject::Ptr o = new DynamicObject();
    o->setProperty ("beat", k.beat);
    o->setProperty ("key", keyName (k));
    return var (o.get());
}

std::optional<KeyChange> fromVar (const var& v)
{
    if (! v.isObject())
        return {};
    auto parsed = parse (v["key"].toString());
    if (! parsed)
        return {};
    KeyChange k;
    k.beat = jmax (0.0, (double) v["beat"]);
    k.root = parsed->first;
    k.scale = parsed->second;
    return k;
}
} // namespace keys

//==============================================================================
namespace fretboard
{
const std::vector<Instrument>& instruments()
{
    static const std::vector<Instrument> list {
        { "Guitar",   { 40, 45, 50, 55, 59, 64 }, 15 },
        { "Bass",     { 28, 33, 38, 43 },         15 },
        { "Ukulele",  { 67, 60, 64, 69 },         12 },
        { "Keyboard", {},                         0 },
    };
    return list;
}

std::vector<Spot> spots (const Instrument& inst, int root, Scale scale)
{
    std::vector<Spot> out;
    auto add = [&] (int string, int fret, int pitch)
    {
        out.push_back ({ string, fret, pitch, keys::contains (root, scale, pitch), keys::isRoot (root, pitch) });
    };
    if (inst.strings.empty())
    {
        for (int p = keyboardLow; p <= keyboardHigh; ++p)
            add (-1, p, p);
        return out;
    }
    for (int s = 0; s < (int) inst.strings.size(); ++s)
        for (int f = 0; f <= inst.frets; ++f)
            add (s, f, inst.strings[(size_t) s] + f);
    return out;
}
} // namespace fretboard
} // namespace lathe
